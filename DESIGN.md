# zosasm design

`zosasm` is a cross assembler for an **HLASM-like language** that writes
**z/OS GOFF** object files. It is written in **portable C99** and builds with nothing but a C compiler
and `make`, so it runs on small platforms without a Go toolchain (Linux,
macOS, z/OS, embedded hosts) and needs no z/OS at assembly time.

It is a *variant* of HLASM, not a clone: the syntax follows HLASM where that
is convenient, and departs from it where HLASM's card-image heritage or
macro machinery gets in the way. The point of the project is full control
over the **GOFF structure** (sections, classes, parts, labels, externals,
attributes) from the assembler source.

## 0. Why C

The first implementation was written in Go. It was ported to C99 because
the tool must be buildable on small platforms that have a C compiler but no
Go compiler, and because the build must not need anything else (no
generators run at build time; `src/isa_tab.c` and `src/ebcdic_tab.c` are
checked in). The Go version is no longer in the repository (the history was
squashed to drop GPL-derived table data). Rules for the C code: `-std=c99 -Wall -Wextra
-pedantic` clean (gcc and clang), no locale dependent functions
(`za_isalpha`, `za_toupper`, ... in `util.c`), all memory from
`xmalloc`/`xcalloc` (aborts on failure; the assembler is a short lived
process and does not free).

## 1. Scope

In:

* all z/Architecture machine instructions (generated from IBM's Principles
  of Operation, see 5),
* the base assembler language: sections, location counters, symbols,
  expressions, `DC`/`DS`/`EQU`/`ORG`/`LTORG`/`CNOP`, literals, `USING`/`DROP`,
* the GOFF-oriented directives: `CSECT`, `RSECT`, `START`, `DSECT`, `COM`,
  `LOCTR`, `CATTR`, `XATTR`, `AMODE`, `RMODE`, `ENTRY`, `EXTRN`, `WXTRN`,
  `DXD`, `CXD`, `END`,
* listing-control statements `TITLE`, `EJECT`, `SPACE`, `PRINT` (they act in
  the listing, see 15), and `PUNCH`, `ICTL`, `ISEQ`, accepted and ignored
  (real HLASM sources still assemble; the preprocessor's `#pragma` is the
  intended form, see 8),
* good diagnostics (see 6).

Out (for now):

* the HLASM **macro language and conditional assembly** (`MACRO`, `&vars`,
  `AIF`, `SETC`...) and `COPY`. They are replaced by a C style
  preprocessor stage (see 8); `COPY` is an error (`ZA9001`) with the hint
  "use #include". The statement reader takes an array of source lines, so
  the preprocessor sits in front of it without the core knowing,
* ESD-less OBJ/XOBJ output, `ADATA`, `SYSLIB` macro libraries, listings in
  HLASM layout (the listing, section 15, has its own layout).

## 2. Source syntax

Differences from HLASM:

| HLASM | zosasm |
|---|---|
| 80-column card images, columns 72/16 | **no column rules**; a line may be any length |
| continuation = non-blank in column 72 | continuation = operand field ends in `,` (see below) |
| names fold to upper case | **symbols are case sensitive**; mnemonics, directives, register names and attribute letters are not. `-fold` may be added later |
| registers are numbers or EQU'd names (`LGR R1,R2` needs `R1 EQU 1`, or the `REGEQU` macro) | the same: `LGR 1,2`, `LGR R1,R2` (only when the program defines `R1`; a bare `R1` is NOT predefined, the error has a hint) and the GNU style, which says plainly that it is a register, `LGR %r1,%r2` (`%r` general, `%f` floating point, `%a` access, `%c` control, `%v` vector; also in `D(%r1,%r2)` and `USING`/`DROP`). The register class is checked (`%f1` where a general register is needed is ZA2055). **`-strict-registers`** (off by default; decided with the user 2026-11-15) accepts in register positions only the `%` form and names made with `NAME EQU %r<n>` (`Symbol.regkind`); a number or other symbol is ZA2067, a register name used as a number ZA2068; the operand kinds come from the instruction table, so masks, lengths, immediates and shift counts stay numbers; with `-disasm` (`DISASM_STRICT`, `strict_operands` in `disasm.c`) the register operands of the ZSYM statements are rewritten as `%r<n>` using the instruction variant's slots (`asm_choose_variant`, `asm_split_addr`), numbers and ABS symbols are converted, `EQU %r` names kept, literals skipped; the generic converter always writes `%r` |
| symbols are 1-63 characters of `A-Z 0-9 @ # $ _`; `ALIAS` gives an external name other characters or up to 256 | symbols have **no length limit** (ESD names up to 32767 bytes) and any name can be written as a **quoted symbol**, see below; there is no `ALIAS` |
| `MNOTE`, `&SYSPARM`, ... | not supported |
| fixed 8-char name field in the listing | n/a |

Statement layout, one per line:

```
[name]  operation  [operands]  [remark]
```

* `name` starts in column 1 (no leading blank); an optional trailing `:` is
  accepted (`loop:`). A line that starts with blank has no name.
* `*` in column 1, or a line holding only blanks, is a comment. `;` in
  column 1 is a comment too.
* `operands` end at the first blank that is not inside quotes `'...'`
  or parentheses. Everything after is a remark. (Same rule as HLASM; the
  diagnostic engine warns when a remark looks like a forgotten operand.)
* **Continuation**: if the operand field ends with `,` the next line
  continues it; its leading blanks are skipped. A remark may follow the
  comma. There is no length limit, so continuation is rarely needed.
* **Quoted symbols.** A symbol is plain (`A-Z a-z 0-9 @ # $ _`, not starting
  with a digit) or written between double quotes: `"a.b"`, `"ext sym"`,
  `"C++::f"`. Inside the quotes any character may be used, `\"` is a quote,
  `\\` a backslash and `\xHH` the source character U+00HH (so any EBCDIC
  byte of an ESD name can be written; `\x00` and raw control characters
  cannot). `"ABC"` and `ABC` are the same symbol. A quoted symbol is allowed
  wherever a symbol is: name field (the name may hold blanks; `:` may follow),
  expressions, `L'"x"` and the other attribute references, `ENTRY`, `EXTRN`,
  `WXTRN`, `V()`, `END`, `PART()`. It replaces HLASM's `ALIAS` (an unquoted
  symbol cannot hold `.`, `-`, blanks...); the name is used as written, so
  the ESD of `"a.b"` is `a.b`. `-disasm` writes such names this way.
  The preprocessor leaves a quoted symbol alone. Errors: `ZA0004` (bad
  escape, empty, not closed), `ZA1021` (not a valid symbol).
* Source is UTF-8 or Latin-1. Character constants are converted to
  **IBM-1047** unless a `CA'...'` (ASCII) type is used. See `internal/ebcdic`.

## 3. Architecture

```
 source text
    |  pp.c (planned)             #include / #define / #if  -> logical lines
    v
    |  reader.c                   statements (name, op, operand text, spans)
    v
 expr.c / instr.c / dc.c ------- diag.c (spans, codes, suggestions)
    |
    v
 pass 1: define symbols, advance location counters, size instructions/DC/DS
    |                        (section/class/LOCTR model: asm.h, directives.c)
    v
 layout: assign final offsets to LOCTR pieces and classes
    v
 pass 2: evaluate expressions, encode instructions (isa.c),
         generate DC data, record relocations
    v
 object model  ->  emit.c + goff.c writer  ->  .o  (80-byte records)
```

Files in `src/`:

* `util.c`      allocation, `Buf`, `Vec(T)`, `StrMap`, UTF-8, file reading.
* `ebcdic.c`    IBM-1047 <-> Latin-1 (`ebcdic_tab.c` generated by
  `tools/genebcdic.py`).
* `goff.c`, `goff_read.c`  GOFF record writer and reader/dumper. The reader
  is used by tests and by `-dump`; it does not depend on the assembler.
* `isa.c`, `isa_tab.c`  instruction table + encoder. `isa_tab.c` is
  generated from `isa/isa.json` by `tools/isagen.c` (`make isa`).
* `diag.c`      diagnostics: spans, codes, notes, help, "did you mean".
* `asm.h`       the object model and the assembler state.
* `reader.c`    lines -> statements, operand splitting.
* `expr.c`      expression parser and value algebra.
* `assembler.c` passes, symbols, layout, directive table.
* `directives.c` section/class/GOFF directives, `EQU`, `ORG`, `CNOP`, `END`.
* `dc.c`, `float.c`  `DC`/`DS`, hex and binary floating point (exact bignum
  conversion, no dependency on the C library's float formatting).
* `instr.c`     operand classes, `USING`/`DROP`, instruction encoding.
* `literal.c`   literal pools.
* `emit.c`      Module -> GOFF.
* `main.c`      command line.

## 4. GOFF mapping

Object model: *module* -> *sections* -> *classes* -> *LOCTR pieces* ->
bytes + relocations. External symbols are ESD items of the module.

| Source | GOFF |
|---|---|
| `X CSECT`, `X RSECT`, `X START` | SD `X`, ED `B_TEXT` (default class) under it, LD `X` at offset 0 |
| `X CATTR attrs` | ED named `X` (class) in the current section; following statements go there until the next section/class/LOCTR statement |
| `CATTR ...,PART(P)` | PR `P` in that class; text is attached to the PR |
| `X XATTR attrs` | attributes (linkage, scope, reference, data type) of the ESD of `X` (LD/ER/PR) |
| `AMODE n`, `RMODE n` | behavioral attributes of the section (SD/ED/LD) |
| `ENTRY X` | LD `X` with module scope |
| `EXTRN X`, `WXTRN X` | ER (strong / weak binding strength) |
| `DXD`, `CXD`, `Q`, `J`, `R` constants | ED `B_PRV` (merge) with a PR per external dummy section; RLD items, see 4.4 |
| `LOCTR` | another piece of the same class; pieces are concatenated in order of first appearance, as in HLASM |
| `DSECT`, `DS` of `DSECT` | no ESD (dummy section), only symbol values |
| `COM` | SD/ED with the COMMON flag |
| `DC A()/AD()/V()/Y()/S()` referring to another element | RLD item (R-address, label referent) pointing at the LD/ER/PR |
| `L ...,=A(X)` literals | collected by `LTORG` / `END` into a pool in the current class |
| `END entry` | END record, entry point by name or by ESDID+offset |

Relocations to labels of the same module use the class origin: an A-con
to a local label gets an RLD with *referent = class (ED)*, R-ID = the ED
(or the PR when the class has a PART), and the offset of the label as the
addend in the text. No LD is needed for local targets (GOFF spec C.6/C.7).
Branch-relative operands inside one class are resolved by the assembler;
across classes or to an ER they get an RLD of type "relative immediate".
Only `ENTRY` labels become LDs.

Literals (`=F'1'`, `=CL8'TEXT'`) are collected per pool. `LTORG` closes the
pool (doubleword aligned, constants ordered by alignment 8,4,2,1, identical
literal text shared); the last pool is placed at `END` at the end of the
first control section. Literals become DC statements with generated labels
`$LTn_m`, so the rest of the assembler needs no special case.

The GOFF ESD record layout, behavioral attribute byte layout and RLD flag
encoding implemented in `internal/goff` follow
*z/OS MVS Program Management: Advanced Facilities*, appendix C (also
summarized in `_context/goff-format-knowledge.md`).

### 4.3 USING, DROP and qualified symbols

`USING` tells the assembler which register holds which address; an
instruction operand without a base register is turned into
displacement + base from the active USINGs (pass 2, `src/instr.c`).

```
         USING base,reg1,reg2,...        ordinary: reg1 holds base, reg2 base+4096 ...
W1       USING base,reg                  labeled: only symbols written W1.NAME use it
         USING base,address              dependent: base is at ADDRESS, whose register
                                         comes from the USING that covers ADDRESS
         USING (base,end,lower,upper),reg limits (all but base optional)
```

* **Ranges.** The default range is the displacement field: 0-4095 from each
  register, or -2^19 .. 2^19-1 for long-displacement instructions. `end`
  limits only the 12-bit displacements (`USING base,reg` is
  `USING (base,base+4096),reg`); `lower` and `upper` (exclusive) limit both.
  Limits are addresses in the section of `base` (numbers for an absolute
  base); `X'80000000'` for `lower`/`upper` means no limit and an empty
  item is skipped, as in `(base,,lower,upper)`. `end` and `upper` must be
  greater than the base and `lower`.
* **Choice.** Among the active USINGs that cover an address, the one with the
  smallest non-negative displacement wins (negative ones only as a last
  resort, for long displacements); if several give the same displacement the
  higher register wins, then the later USING.
* **Unqualified and qualified symbols.** An unqualified symbol uses only
  unlabeled USINGs. `W1.FIELD` uses only the USING labeled `W1` (an error,
  `ZA3068`, when `W1` is not active). `.` is not part of a symbol, so the
  qualifier cannot be mistaken for a quoted name such as `"a.b"`; `L'W1.FIELD`
  is accepted too. A USING label is not a symbol: it conflicts with a symbol
  of the same name (`ZA3067`).
* **When a USING ends.**
  * An ordinary USING ends when a later ordinary USING names the same
    register, or `DROP reg`.
  * A labeled USING ends with `DROP label` or when the same label is used
    again, never by `DROP reg`; the registers of one labeled USING form one
    range.
  * A USING of an address ends with `DROP address` (the USING that covers the
    address).
  * Dependent USINGs end with the USING they depend on (the *root*: a
    dependent USING is never based on another dependent USING but on the
    ordinary or labeled USING that finally supplies the register), or by
    their own label or address. `DROP` without operands ends everything.
* **Absolute bases.** `USING 0,3` makes `LA 7,5` assemble as `LA 7,5(,3)`;
  without such a USING an absolute displacement uses base 0.

Codes: `ZA3060` (DROP of nothing), `ZA3061`/`ZA3062` (base is external /
not a simple address), `ZA3063` (range item count), `ZA3064` (limit in
another section), `ZA3065` (end/upper not greater), `ZA3066` (several
operands of a dependent USING, address among registers), `ZA3067` (label is
also a symbol), `ZA3068` (qualifier is not an active USING label).

### 4.4 External dummy sections: DXD, CXD, Q, J, R

```
WORK     DXD   CL100,F        external dummy section: 104 bytes, word aligned
         DC    Q(WORK)        offset of WORK in the pseudo register area (4 bytes; QD 8, QLn)
         DC    J(WORK)        length of WORK (J, JD, JLn: 2, 3, 4 or 8 bytes)
         CXD                  fullword: total length of the pseudo registers (4 byte aligned)
         DC    R(ENT)         address in the PSECT of ENT (see XATTR PSECT); R, RD, RL3
EXT      XATTR REFERENCE(DATA,INDIRECT)
```

* `DXD name operands` takes DS operands (length and alignment) and stores
  nothing. The name is a symbol of length attribute 1; it is only valid in
  `Q`, `J` and `XATTR`, otherwise `ZA2062`. Not allowed in a DSECT (`ZA2063`)
  or without a name (`ZA2061`).
* A DSECT named in `Q` or `J` becomes an external dummy section of the
  section where it is first used (`resolve_xd`, between the passes); its
  length and alignment are those of the DSECT. `J` also takes a class name
  or a part name; `Q` also a part name (`ZA2060` for anything else).
* `R(name)` names an external symbol, a section or an ENTRY label
  (`ZA2060`); `CXD` and `R` use the labels the same way as `A`.
* Lengths: `ZA2029`. `QY` is not supported yet.
* `REFERENCE(CODE|DATA, DIRECT|INDIRECT)`: one value from each group, in
  either order (`ZA3037` for a repeat or an unknown word). `INDIRECT` sets the
  *indirect* bit of the behavioral attributes of the ESD.

GOFF: a section that uses any of these gets an ED `B_PRV` (bind MERGE, data)
with one PR per external dummy section (name space 2, length and alignment
of the section). The RLD items (reference/referent types follow the tables
of *Program Management: Advanced Facilities*, appendix C):

| Constant | reference | referent | R-ID | store |
|---|---|---|---|---|
| `Q(x)` | offset (1) | label (0) | the PR | no |
| `J(x)` DXD, DSECT, part | length (2) | part (3) | the PR | yes |
| `J(x)` class | length (2) | class (2) | the ED | yes |
| `CXD` | length (2) | class (2) | the `B_PRV` ED | yes |
| `R(x)` | R-type (7) | label (0) | the ER or LD | yes |

**Not verified against an IBM object.** The documentation lists the Q
constant as `X'1200'` (reference *address*, referent *class*); zosasm writes
*offset/label* because the R-ID is a PR. The encodings are in one function,
`reloc_xd` in `emit.c`, and are read back by `xcon_reloc` in `disasm.c`.
Compare with an object assembled by IBM `as` when a z/OS system is available.

`-disasm` writes the pseudo registers as `DXD` statements (alignment as a
`0H/0F/0D` item, then `XLn`; sizes over 32767 as `nXL32768`), the constants
as `Q(...)`, `J(...)`, `R(...)` and `CXD`. A DSECT that was used in `Q` comes
back as a `DXD` (the object is the same). A `B_PRV` class without PRs and
without a CXD is dropped.

### 4.7 Assembler controls (`src/dirctl.c`)

`ACONTROL`, `PUSH`/`POP`, `CEJECT`, `EXITCTL` and `REPRO` live in
`dirctl.c`. State-changing ones run in both passes (`each()` resets the
state before pass 2): `Asm.afpr`, `Asm.strict` (`TYPECHECK(REGISTER)` /
`NOTYPECHECK`), `print_depth`, `using_stack`, `ac_stack`.
* `ACONTROL AFPR|NOAFPR` makes `check_reg` (`instr.c`) reject FP registers
  other than 0,2,4,6 (ZA2071); `TYPECHECK(REGISTER|NOREGISTER)` toggles the
  register type checks; `CA(n)`/`CE(n)` are validated (ZA2073); other
  selections (`COMPAT`, `FLAG`, `MACHINE`, `OPTABLE`, `RA2`, `ILMA`, ...) are
  checked for form and ignored (ZA2072).
* `PUSH`/`POP` take `PRINT`, `USING`, `ACONTROL` (ZA2074, `POP` without
  `PUSH` ZA2075). Three independent stacks: listing print state, `UsingVec`
  copies (pass 2 only, as `USING` itself) and `AcState{afpr,strict}`.
* `EXITCTL` checks the exit type (ZA2076) and does nothing. `REPRO` warns
  (ZA2077); the reader skips the next physical line.
Not compared with IBM.

## 5. Instruction table

`isa/isa.json` is the source of truth: mnemonic, format name (POP names such
as `RXY-a`), opcode bytes, operand list (width, bit position, kind) in
assembler syntax order, the number of optional trailing operands, the
facility code and the instruction name. Vector extended mnemonics such as
`VFAEBS` carry an `or` value that is ORed into the instruction after the
operands (`[M5]|X'1'`). `tools/isagen.c` turns the JSON into `src/isa_tab.c`;
run `make isa` after editing the JSON.

Provenance: the table is generated by `tools/pop2json.py` from the
*z/Architecture Principles of Operation* (SA22-7832-14) and from nothing else.
The script reads Appendix B (mnemonic, name, format, facility, opcode of
1272 instructions), the syntax lines of the instruction descriptions
(operand order, optional operands, the mnemonic tables of instruction
clusters), Figure 5-1 (field positions of every format) and Appendix J
(1097 extended mnemonics). Register classes are not in the syntax lines;
they follow the chapter (floating point: chapters 18-20 plus a short list)
and a few overrides, all in the script. Development needs `python3` and
`pdftotext`; building and using zosasm needs neither (the generated JSON and
`src/isa_tab.c` are checked in). Regenerate with
`python3 tools/pop2json.py SA22-7832-14.pdf > isa/isa.json && make isa`.

Known limits of the generated table:

* immediate operands accept any value that fits the field, signed or
  unsigned (`AHI 1,40000` assembles): the POP states the signedness only
  in prose,
* register pairs (even-odd GPR pairs, floating-point extended registers)
  are not checked; a wrong pair assembles and fails with a specification
  exception at run time,
* a few extended mnemonics are not generated: `NOTR`, `NOTGR`, `VNOT`
  (one operand twice), those built on `RISBHGZ`/`RISBLGZ` (`LHHR`,
  `SLLHH`, ...), and mnemonics identical to a base instruction. The
  extended mnemonics of Appendix J of the POP are the only ones; aliases
  of other assemblers (`BHE`, `JG*`, ...) do not exist. `JLE`, `JLH` and
  the other `JL*` forms are `BRCL` as in HLASM.

HLASM operand syntax is mapped to table fields by *format class*
(`D(X,B)`, `D(B)`, `D(L,B)`, `D(,B)`, register, immediate, mask,
relative-immediate); see `src/instr.c`.

## 6. Diagnostics

Goals: help find the mistake, do not stop at the first error, never print
a message without a location.

* Every diagnostic has a severity, a **stable code** (`ZA0xxx` syntax,
  `ZA1xxx` symbols/expressions, `ZA2xxx` instructions, `ZA3xxx` sections and
  GOFF directives, `ZA9xxx` internal), a primary span and optional label,
  notes (related locations, e.g. "first defined here") and help lines.
* Output is an annotated source excerpt, rustc style:

```
error[ZA2001]: unknown instruction "LGRR"
  --> t.za:2:10
  |
2 |          LGRR  1,2
  |          ^^^^ not an instruction or directive
  help: did you mean "LGR"?
```

* Recoverable errors skip the statement and continue; the object file is
  not written if any error was reported. `-max-errors N` stops early.
* Suggestions use edit distance against mnemonics, directive names, symbols
  and operand keywords (`CATTR` attributes etc.).
* `-W` flags select warnings (`-Werror` makes them fatal), `-color`.
  `-diag json` (implemented, `rep_print_json()` in `diag.c`) writes one
  JSON document `{"diagnostics":[...],"errors":n,"warnings":n}` to stderr
  instead of the text excerpts (`-diag text` is the default). Each entry has
  code, severity, message, file/line/column/length of the primary span,
  label, notes and help lines. Only `zosasm` has it; `hlasm2za` and the MCP
  server do not.

## 7. Testing strategy

* `make test`: unit tests per module (`tests/test_*.c`) and command line
  tests (`tests/run_cli_tests.sh`); sanitizers with
  `CFLAGS="-O1 -g -fsanitize=address,undefined" make test` (leaks are by
  design: nothing is freed),
* `tests/test_isa_pop.c` assembles about 50 instruction forms of every
  format class and compares the bytes with encodings written out by hand
  from the POP formats (including RXB and the `or` mnemonics), and checks
  the whole table (field overlap, lengths, duplicates),
* golden files in `testdata/` (planned): source -> expected GOFF dump (`-dump` text),
  so GOFF structure regressions are visible in review,
* GOFF self-check: every object written by tests is parsed back by
  `goff_parse`, and structural rules are verified (ESD order, parents,
  text inside element length, RLD inside text),
* **oracle tests on z/OS**: when z/OS is reachable the same sources are
  assembled with IBM HLASM (`as`) and the structure is compared
  (see PLAN.md); results update the "verified" column there,
* final proof: object files must go through the z/OS binder (`ld`) and run.

## 8. Preprocessor (implemented, `src/pp.c`)

A C style preprocessor replaces `COPY` and the HLASM macro language. The
marker is `#` (one constant in `pp.c`; `%` is the fallback should it ever
conflict).

* Directives: `#include "file"`, `#define` (object-like and function-like,
  `##` paste), `#undef`, `#if`/`#ifdef`/`#ifndef`/`#elif`/`#else`/`#endif`,
  `#error`, `#warning`, `#pragma`. `#if` expressions: integers, `defined(X)`,
  `! && || == != < > <= >= + - * / % ~`, parentheses. Command line
  `-DNAME=val`, `-Idir`, `-E` (print the preprocessed lines and stop).
* Built-in names (they cannot be `#define`d, `defined(X)` is true for
  them, ZA4021): `__FILE__` (the file being read), `__LINE__`,
  `__BASE_FILE__` (the file named on the command line), `__INCLUDE_LEVEL__`
  (0 in the main file), `__COUNTER__` (0, 1, 2, ... one per use in this
  run), `__ZASM_VERSION__` (`major*10000 + minor*100 + patch`, 0.1.0 is
  `100`), `__DATE__` (`2026-11-15`) and `__TIME__` (`20:21:05`), both UTC.
  `__DATE__` and `__TIME__` use `SOURCE_DATE_EPOCH` when it is set; with
  `-reproducible` and no `SOURCE_DATE_EPOCH` they are an error (ZA4080), so
  an object cannot silently change between builds. A malformed
  `SOURCE_DATE_EPOCH` is ZA4081. All of these are plain text, so they work
  in operands but not inside a quoted string (see `#(x)` below).
* **Stringify `#(x)`**: in the body of a function-like macro, `#(x)` where
  *x* is a parameter name becomes the argument as a quoted text, `'` doubled:
  `#define STR(x) C ## #(x)` and `DC STR(__DATE__)` give `DC C'2026-11-15'`.
  The spelling differs from C's `#x` on purpose: `#` is a symbol character
  here (`#COUNT` is a label), so `#x` cannot be told from a name. Unlike C,
  the argument is macro-expanded first (`STR(N)` with `#define N 10` gives
  `'10'`), so no two-level trick is needed. `#(` followed by anything but a
  parameter name is left alone.
* A `#` in column 1 is a directive only when followed by one of those
  keywords; otherwise it is an ordinary label such as `#COUNT`.
* No expansion inside quoted strings, and none when an identifier is
  directly followed by `'` (`L'X`, `C'..'`). `\`-newline is a plain line
  splice applied first. In a `#define` body, `\n` outside quotes starts a
  new physical line (a non-blank first character is then a label, a leading
  blank means the operation field).
* The trailing-comma statement continuation stays in the reader; the
  preprocessor adds a warning when a line ends in `,` and the next line
  starts in column 1.
* `#pragma` replaces listing/assembler control: `title "text"`, `eject`,
  `space n`, `print on|off|data|nodata|gen|nogen`, later `arch z13`. They
  are validated here and act in the listing (section 15). The HLASM spellings
  (`TITLE`, `EJECT`, `SPACE`, `PRINT`, `PUNCH`, `ICTL`, `ISEQ`) stay
  accepted no-ops; no hint toward the pragma form is given, to keep real
  HLASM sources free of noise.
* Only the operation, name and operand fields are expanded; the remark
  and comment lines are not (`remark_start` uses the reader's operand
  scanner). Arguments are pre-expanded except next to `##`; a macro is not
  expanded inside its own expansion.
* Error codes: `ZA4010-4012` include, `ZA4020-4023` define/undef,
  `ZA4030-4034` conditionals, `ZA4040-4042` macro calls, `ZA4050/4051`
  `#error`/`#warning`, `ZA4060/4061` pragma, `ZA4070` comma continuation,
  `ZA4080/4081` the time of `__DATE__`/`__TIME__`.
  When the preprocessor reports an error the assembler does not run.
* Diagnostics map each output line to its original file and line; expanded
  lines get a note "in expansion of macro X at file:line"; included files
  are registered with `rep_add_source`.

## 9. Diagnostic code changes since the Go version

* `ZA2053` is the floating point *range* error (the Go version reused
  `ZA2051`, "empty literal").
* `ZA3043` is "LTORG in a dummy section cannot hold literals" (the Go
  version reused `ZA3040`, "AMODE names a section that is not defined").
* Attribute references (assembler operands): `L' T' S' I'` and `D' O'` are
  evaluated, `L'*` for machine instructions and for a DC/DS whose first
  operand has an explicit length or a fixed-length type. `T'`, `D'` and
  `O'` yield the EBCDIC code of the letter (`T'`: DC type, `I` instruction,
  `J` section, `T` external, `U` unknown/EQU; `O'`: `A` directive, `O`
  machine instruction, `U` unknown, no `E`/`M`) or 0/1 for `D'`, which is
  true only for symbols defined *before* the statement. `S'` is the scale
  modifier of an F/H/E/D/L constant (else 0, section 13); `I'` follows the HLASM formulas (F/H
  `8L-S-1`, P `2L-S-1`, Z `L-S`, E/D/L `2(L-1)-S`, otherwise 0). `K'` and
  `N'` belong to the macro language and give `ZA1032`; `ZA1031` is a
  `X'*` that cannot be evaluated; `ZA1033` is an EQU type operand outside
  0-255. The third EQU operand sets the type attribute (it was ignored).

## 10. Disassembler (`-disasm`, `src/disasm.c`)

`disasm_module()` turns a parsed GOFF module (`goff_parse`) into zosasm
source. The aim is a *reliable* round trip for objects the assembler wrote:
source -> object -> source -> object gives the same object bytes and the
second source equals the first one. `main.c` assembles the result again
and warns if it does not assemble or the object differs (first differing
byte and record).

**Canonical object.** For the source to be free to place labels and
statements, the object must not depend on statement order. `emit.c`
therefore creates the ERs in order of first use in the offset-sorted
relocations (class order, then offset) and sorts the ENTRY LDs of a section
by (class, offset, definition order).

**Model.** Per class: bytes, an `occ` array (1 = in an address constant,
2 = in a relative-immediate relocation field), `rel_at`, `ins_at` and a
label array (bit 1 fixed: LD or END; 2 derived: branch target; 4 named;
8 target of an address constant).

1. `load_esds`, `load_text`, `load_labels`, `load_relocs`: unsupported
   relocations are ignored with a warning.
2. `drop_split_adcons`: a label inside an address constant cannot be
   written (the constant would have to be split); its relocation is dropped
   and the bytes stay as data.
3. `discover`: recursive descent from offset 0, the labels (bits 1 and 8)
   and the END entry, using `isa_decode()` (`isa.c`), which finds the table
   entry with the fewest operand bits that reproduces the bytes exactly, so
   extended mnemonics (`br`, `be`, `brel`) are preferred. The scan stops
   after an unconditional branch. Relative fields use relocations to ERs and
   classes or turn into labels.
4. `settle`: derived labels; an instruction that has a label inside it, or
   whose target would lie inside an address constant, is written as data
   instead (`bad[]` remembers it, and the decoding is repeated so that code
   behind the instruction is still found).
5. Emission: `CSECT`/`RSECT`/`COM`, `AMODE`, `RMODE`, `ENTRY`, `EXTRN`/
   `WXTRN` (only ERs that something refers to), `XATTR`, then the class
   bodies with `CATTR` for every class but the first `B_TEXT`.

Instructions are written with GNU style registers (`%r1`), memory operands
as `D(X,B)`, `D(L,B)` or `D(B)`. Data is `DC XL<n>'0'` for zero runs of 8
or more bytes, `DC C'...'` for runs of 6 or more safe characters, else
`DC X'...'` (16 bytes per statement), and `DC AL<n>(label)` / `VL<n>(er)`
for address constants. Extra labels at one offset use `DS 0X`. Generated
labels are `L%05X`, or `L<class>_%05X` for classes other than the first.

**Limits.**

* Objects from other producers are best effort. The clang object
  `dev/src/debug/goff/testdata/hello.goff` still fails cleanly: its names
  (`hello.c50332088#hello#C`, `.&ppa2`) are written as quoted symbols, but it
  uses reference type 7, several SDs and ED/PR combinations.
* Warnings mean the object could not have been written by the assembler;
  then only valid source and equal text bytes are guaranteed, not a fixed
  point.
* A class can have 64 MiB at most.
* `CATTR` cannot change the existing `B_TEXT`; `RSECT` stands for read-only.
* Not implemented (reader, writer or both): `ADAEsdId` of an LD,
  extended-attribute ESDID, SD tasking and binding scope, ER indirect bit,
  reference type 7, LEN records from
  source, the amode-sensitive bit of V-constants.

**GOFF study (LLVM `GOFFObjectWriter`/`GOFFObjectFile`, Go `GOFF-tool`).**
ESD name space 2 is the pseudo-register space (`GOFF_NS_PSEUDO`, formerly
misnamed); bits 5-7 of the ESD flags byte at offset 41 are
`reserved_qwords`; RLD flag byte 0: `0x80` same R-id, `0x40` same P-id,
`0x20` same offset, `0x02` 8-byte offset and `0x04` 8 bytes of extended
attributes after the item (LLVM's reading; the IBM text calls `0x02` a
2-byte offset, which contradicts the 28-byte maximum item size). The reader
follows LLVM and the writer never emits either bit. LLVM sorts the RLD by
(P, R, offset), writes an END record count of 0 and zero HDR fields other
than the architecture level; zosasm does not copy that.

## 11. ZSYM: symbolic information in the object (`src/zsym.c`, ZSYM.md)

**Status.** Writer and reader (Z1) and the smart `-disasm` that replays
the statements (Z2) are implemented. `-generic` makes `-disasm` ignore the
class; the output is then exactly the same as for an object without ZSYM.

**Class.** `C_ADATAE9C1` is an ED under the first SD (last ESD of the
object), NOLOAD, text style unstructured, bind CAT, length = size of the
chunks. The text records hold the chunks at running offsets. The module
library default is `Module.zlevel = 0` (no class); `zosasm` sets 1
(`-gz1`, default), 0 (`-gz0`) or 2 (`-gz2`, adds the original sources: section 14).

**Chunks.** `'Z' 'S' version:u8 kind:u8 length:u32BE payload crc32:u32BE`
(crc over everything before it, IEEE polynomial). Kinds: 1 MODULE (producer,
level, file names, scopes = (section, class, kind) in section/class order,
number of symbols and statements), 2 SYMS (up to 256 symbols per chunk), 3
STMTS (up to 256), 4 END (counts). Integers are LEB128 (zigzag when signed),
strings are length plus bytes. A module block is MODULE SYMS* STMTS* END;
the binder concatenates several blocks and the reader skips zero padding
between them.

**Symbols.** name, kind (label, absolute EQU, relocatable EQU, external,
section, DXD), flags (weak, entry), scope, value (class offset; EQU value;
section size), length/type/scale attributes, and for a relocatable EQU its
terms (coefficient and scope or symbol). Symbol numbers are the order of
`Module.sym_order` (literal pool labels `$LT...` are in it).

**Statements.** One record per statement of pass 2, in source order: kind,
file/line, scope, class offset and length of the bytes the statement wrote
(0 when it switches location counter), crc32 of those bytes with the bytes
covered by a relocation taken as zero (the binder changes them; not for
DSECTs or empty ranges), and the label, operation and operands. Operands
are *templates*, not text: a sequence of text, symbol and attribute
(`L'SYM`) items, where a name is an item only when the module defines it. A
renamed symbol therefore changes every place that uses it. `END` covers the
literal pool.

**Reader** (`zsym_parse`) never fails on content: unknown versions, bad
checksums, truncation and junk are counted in `bad_chunks` and mark the
block `damaged`; a block is `complete` only when it has an END chunk, no
damaged chunk and agreeing counts. Every single bit flip of a block is
detected (CRC-32). `zosasm -dump` prints the blocks after the ESD/TXT listing.

**Round trip invariants** tested in `tests/test_zsym.c`: the object with and
without ZSYM is identical apart from the class (`goff_equal`); crc masks
match after the relocated bytes change; chunk damage is contained to its
block; 4000 random mutations do not crash the reader (also under ASan).

**Smart `-disasm`** (`smart_plan`/`smart_emit` in `src/disasm.c`).
The statements of the single complete block are written back in source
order: label, operation and operands rendered from the templates, so the
output has the original names, EQUs, USINGs, DSECTs, literals (`=F'5'`) and
symbolic operands (`FLD2(L'FLD)`), but no comments and no macro/`#include`
structure (the preprocessed text is replayed). The object is first checked
**per section**:

* each class of the object belongs to one ZSYM scope and vice versa;
* every statement range lies inside its class and its crc agrees with the
  bytes (bytes covered by an RLD item are masked);
* bytes no statement wrote (padding, class gaps) equal the fill byte.

A section that fails any check is written by the generic converter at the
place of its first statement and a warning names the statement; the
labels ZSYM knows for that section are used as its generic label names, so
the other sections still find them. The END record covers the literal pool of
the first section (a synthetic END record exists when the source had none),
and in a failing section the END is the generic one. A damaged, incomplete,
inconsistent or multi-module (bound) block means the generic converter for
the whole object, with a warning.

`zosasm -disasm` then assembles the text (without ZSYM) and compares it with
the object (`goff_equal`, ignoring the ZSYM class). If the ZSYM text does not
give the object back (for instance because ESD attributes or relocations
changed in a way the crc does not see), it warns and writes the generic
text instead, so the result is always a source that assembles to the
object, or a warning that says it does not.

**Verified** (`tests/test_disasm.c`, `tests/test_zsym.c`): for every test
program, ZSYM source assembles to the same object, assembling it with ZSYM
and converting again gives the same text (fixed point), `-generic` equals the
conversion of an object without ZSYM; a patched byte degrades only its
section (code, data, literal pool, unwritten padding); a flipped bit in the
ZSYM class falls back to generic; editing the source (`F1 DS F` -> `F1 DS
2F`) moves the offsets of the fields after it (the point of keeping symbols).

**Known gaps.** `ORG` that moves back and rewrites bytes makes the crc of
the earlier statement fail (that section falls back to the generic
converter). Comments, remarks and the original spelling of a symbol's case
are not kept in ZSYM (tier 2, `-gz2`, keeps the whole text: section 14). The file/line of a statement come from the
preprocessed input. Nothing here is checked against an
IBM tool; the class name and attributes follow the ADATA description in the
GOFF documentation.

## 12. Built-in functions, comparisons and ASSERT (`expr.c`, `assembler.c`)

These replace the `*-LABEL` length tricks and the `MNOTE`/conditional
assembly checks of HLASM. They are assembly time only; nothing is written to
the object except through ZSYM (the statements are kept as written, so
`-disasm` of an object with ZSYM shows `sizeof(S)` again, while the generic
converter writes the number).

**Functions.** `sizeof(x)`, `offsetof(S,f)`, `alignof(x)`, `countof(x)`; the
names are lower case and are recognised only when directly followed by `(`
(a symbol `SIZEOF` is an ordinary symbol; in an operand such as
`LA 1,sizeof(S)(2)` the first parenthesis is the call and `(2)` the index).
The arguments are plain or quoted symbol names.

| call | meaning |
|---|---|
| `sizeof(label)` | bytes of the first operand of its `DC`/`DS`, `L'label * countof(label)`. `W DS 4CL10` is 40, `T DC 3H'1'` is 6, an instruction label is the instruction length. (`L'` itself stays the element length, as in HLASM.) |
| `sizeof(section)` | bytes of the first class (`B_TEXT`) of a DSECT or CSECT |
| `sizeof(dxd)` | the length of an external dummy section (`DXD`) |
| `offsetof(S,f)` | offset of the label *f* in the section *S* (a DSECT, or any section whose first class holds *f*) |
| `alignof(label)` | the alignment of the item in bytes: its type's boundary, 1 when it has an explicit length, 2 for an instruction |
| `alignof(section)` | the strictest alignment used in it (at least 1); of a `DXD`, its alignment |
| `countof(label)` | duplication factor times the number of values of the first operand: `4CL10` 4, `3H'1'` 3, `F'1,2,3'` 3, `0F` 0 |

**When a value is known.** A label's size, count and alignment are known
immediately after its statement. The length of a section is known only at
the end of pass 1, so:

* a **DSECT** is answered at once from what has been read so far, and the
  answer is recorded; at the end of pass 1 it is compared with the final
  length and a DSECT that grew afterwards is an error (**ZA1044**, "was 8
  here, but it is 12 at the end"). A closed DSECT therefore works anywhere,
  including `SLEN EQU sizeof(S)` and `DS CL(sizeof(S))`.
* a **CSECT** answers in pass 2 only (forward reference semantics): in
  `DC A(sizeof(M))` or an instruction operand it is fine; where the value
  would size storage or define an `EQU` it is **ZA1042**.
* `offsetof` of a DSECT field is known when the field is defined; of a
  CSECT field in pass 2.

Diagnostics: `ZA1040` bad argument list, `ZA1041` the symbol has no size /
count / alignment (an `EQU`, an external symbol; `countof` of a section),
`ZA1042` too early, `ZA1043` *f* is not a field of *S*, `ZA1044` DSECT grew.
An undefined name is the usual `ZA1010` (pass 1, where needed) or `ZA1011`.

**Comparisons and logic** (all expressions, lowest precedence; result 1 or
0, operands must be absolute): `==  !=  <  <=  >  >=`, `&&`, `||`, and
unary `!` (which binds like unary minus). `&&` and `||` evaluate both sides.
A relocatable difference (`A-B`, same section) is absolute and can be
compared. Parentheses group (`(1<2)*7` is 7). A single `&` or `|` is still
an error. These are needed by `ASSERT`, but any operand can use them:
`LHI 1,sizeof(S)>256`.

**`ASSERT condition[,'message']`.** Checked in pass 2, so sizes and forward
references work (`ASSERT sizeof(M)<=4096,'M is bigger than a page'`). A
zero condition is **ZA1052** (`assertion failed: message`, with the
condition text as help; without a message the condition text is the
message); the object is not written. The message is a quoted text with `''`
for a quote. `ZA1051` bad message, `ZA1053` a name on the statement,
`ZA1013` relocatable condition. The statement generates no bytes and is
recorded in ZSYM as "other".

**Not done.** `sizeof(type)` (`sizeof(F)`), `sizeof` of an expression, and
`offsetof` with a nested path. `sizeof(label)` of an `EQU` is an error
rather than its `L'` (the length attribute of an `EQU` is rarely
meaningful).

**Preprocessor side** (section 8): `__COUNTER__`, `__BASE_FILE__`,
`__INCLUDE_LEVEL__`, `__ZASM_VERSION__`, `__DATE__`, `__TIME__`, the `#(x)`
stringify operator, `-reproducible`, `-version`.

## 13. DC modifiers, CU, CCW, floating point forms (`dc.c`, `float.c`)

**None of this has been compared with IBM `as` / HLASM output yet** (no
z/OS access at the time of writing). The values follow the HLASM Language
Reference, and the tests (`tests/test_dc_mod.c`) check the manual's own
examples (`FS24'6.5'`, `HS6'-25.46'`, `FS4'-10,25.3,U268435455'`) and
published IEEE 754 patterns; the first run on z/OS must re-check the rest.

**Modifier order** `dup type [ext] [Ln] [Sn] [En] ['value']`. `n` is a
decimal number or `(expression)`, with an optional sign. `DcSpec` has
`has_scale/scale/has_exp/exp10`.

**Scale `Sn`.** F and H: -187..346, the value is multiplied by 2^n after the
conversion and rounded half away from zero to a whole number (the leftmost
lost bit decides). E, D, L (hexadecimal): 0..14 (E, D) or 0..28 (L); the
fraction is shifted right by n hex digits (an unnormalized fraction, the
characteristic is raised). EB/DB/LB: warning `ZA2059`, the modifier is
ignored. Decimal float and every other type: error (`ZA2057`, `ZA2056`).
The scale becomes the `S'` attribute of the label (`I'` already used it).

**Exponent `En`** (power of 10, applied before the conversion, added to the
exponent in the nominal value). -85..75; with the B or H type
extension -2^31..2^31-1 (the decimal D extension keeps -85..75). Only
F, H, E, D, L. `ZA2058`.

**F and H nominals** are now decimal numbers with fraction and `E`
exponent; a leading `U` means unsigned (no sign allowed). Whole numbers keep
the old permissive range (an n-byte constant accepts up to 2^(8n)-1 and
down to -2^(8n-1)); 8-byte signed values stop at 2^63-1 unless `U`.
Arithmetic is exact (`float.c` bignums). `ZA2022` not a number, `ZA2023`
does not fit.

**CU.** `CU'text'`: UTF-16 big endian, two bytes per code unit, characters
above U+FFFF become surrogate pairs. Source text is UTF-8, so any Unicode
character can be written. Length modifier is in **bytes**, must be even
(`ZA2042`), truncates on the right (`ZA2049` if it would cut a surrogate
pair) and pads with U+0020. Implicit length is the data length; `DS CU` is 2
bytes. HLASM's `CU(1202/1208)` options (UTF-16LE, UTF-8) do not exist.

**CCW, CCW0, CCW1** (`dir_ccw`). Four operands: command code, data address,
flags, count. Doubleword aligned, 8 bytes, the label has length 8 and type
`W`. Format 0: `cmd, addr(3 bytes, relocated like AL3), flags, 0, count`;
format 1: `cmd, flags, count, addr(4 bytes, relocated like A)`. Operand
ranges `ZA2064` (command and flags 0..255, count 0..65535); `ZA2065` is a
warning for flags bits 38-39 set in format 0. Needs an active section, like
every data directive. The statement is ZSYM kind "other" and round trips
through `-disasm`.

**Floating point.** One entry point `float_const(text, ext, n, exp10,
scale, ...)` returns 1 ok, 2 ok with a warning (`ZA2066`), 0 not a number
(`ZA2050`), -1 error (`ZA2053`).

* Type extensions: none/`H` hexadecimal, `B` binary (IEEE 754, 4/8/16
  bytes), `D` decimal (IEEE 754-2008 DPD, 4/8/16 bytes), `LQ` hexadecimal
  extended aligned on a quadword.
* **Rounding mode** at the end of the nominal value: `EB'0.1R5'`. `B`
  and `H`: `R1` (nearest, ties away), `R4` (nearest even), `R5` toward zero,
  `R6` toward +infinity, `R7` toward -infinity. `D`: `R8` nearest even, `R9`
  toward zero, `R10` up, `R11` down, `R12` nearest ties away, `R13` nearest
  ties toward zero, `R14` away from zero, `R15` round for re-round (truncate,
  but a result ending in 0 or 5 with a dropped non-zero tail is incremented).
  Defaults: plain and H `R1` (as before), B `R4`, D `R8`. Without B, D or H
  a mode is an error.
* **Special values** `'(INF)'`, `'-(INF)'`, `(QNAN)`, `(SNAN)`, `(NAN)`,
  `(MAX)`, `(MIN)`, `(DMIN)` for B and D; `(MAX)`, `(MIN)`, `(DMIN)` for
  hexadecimal. Bit patterns follow the manual's notes (SNAN has `01`, QNAN
  `11` and NAN `10` in the two high fraction bits; decimal NaNs have no
  payload and NAN = QNAN). `MIN` is the smallest normal number, `DMIN` the
  smallest subnormal. Decimal `MIN` is 1000000E-101 (short), i.e. 1E-95.
* **Range**: binary overflow gives a warning and infinity or the largest
  number depending on the mode (and `-` sign); underflow to zero or to a
  subnormal with lost bits is a warning. Decimal overflow and underflow are
  the same. Hexadecimal overflow stays an error. A negative zero keeps its
  sign (`EB'-0'` is `80000000`).
* **Decimal float** works on the digit string, not on a rational: the
  preferred exponent is kept (`DD'1.50'` is 150E-2, `DD'1.5'` is 15E-1);
  more digits than the format has are rounded by the mode; an exponent too
  large with spare digits is folded down (`ED'1E96'` is 1000000E90); too
  small becomes subnormal. Short 7 digits, long 16, extended 34.

**Relocatable differences `A(X-Y)`.** An A-type constant (3, 4 or 8 bytes)
may hold up to five relocatable terms, each with coefficient +1 or -1; every
term is one RLD item on the same field, additions first, then subtractions
(`A(EXTA-*)` is +EXTA -class of the statement; `AL3(-B1)` is a lone SUB).
The constant holds the absolute part (the offsets inside the classes), the
binder adds and subtracts the origins. Items use fetch (not store), action
`GOFF_ACTION_ADD` or `GOFF_ACTION_SUB` (flag byte `X'0002'` for SUB, as
the GOFF reference lists for "A-con with subtraction"). Terms with the same
base cancel before this (two labels of one class give an absolute value).
`-disasm` joins an ADD and a SUB at the same field into `A(P-S+k)` with
labels at the class origins, or `A(-S+k)` for a lone SUB; the object is the
same afterwards. A SUB on a V, Q, R or X constant, or one with bit lengths,
is still ignored with a warning. Other binder producers may order the items
differently: the reader takes additions first, whatever the order.

**Bit lengths `L.n`.** `DcSpec.bits` holds the length in bits; `has_len` and
`length` are set to the ceiling in bytes, so the byte code produces the
field. `bits_add`/`bits_flush` pack the fields of the operands of one `DC`
(and of `DS`) into bytes without alignment between them: `C`, `E`, `D`, `L`
are left aligned in the field, the other types take the low-order bits.
Relocatable values with a bit length are an error (ZA2070), a type that has
no bit form too (ZA2069).

**`S`/`SY`.** `asm_base_disp()` (`instr.c`) resolves the operand through the
active `USING`s as an instruction operand does (`S(address)` or
`S(disp(base))`); an absolute number with no `USING` gives base 0. `S` is two
bytes `[base<<4 | D>>8][D&0xff]`, `SY` three, `[base<<4 | DL>>8][DL&0xff][DH]`.
`S(*)` in a literal gives the address of the pool entry (HLASM: the place of
use). Not compared with IBM.

**`QY`.** Relocation kind `REL_QY`; GOFF reference type 9
(`GOFF_REF_LONGDISP`), length 2, bit length 4, bit offset 4. `-disasm` prints
it as `QY(name)`; `-elf` rejects it. Not verified against IBM.

**`G`.** There are no DBCS tables: the nominal value is hex quads (blanks and
`<` `>` are ignored), padding is `4040`, truncation is on the right, `T'` is
`@`.

**Not done.** The `CU` code page options and the exponent modifier warning
for values that are not exactly representable.

## 14. Original sources, `-gz2` (tier 2)

**What and why.** ZSYM (section 11) keeps the *statements*; it cannot keep
what the preprocessor removed (comments, `#define`, `#if`, macro spelling,
blank lines, `__LINE__` as a name). At `-gz2` the object also holds the
files the assembler read, byte for byte: the main file and every
`#include`, and the `-D` options. `zosasm -dump` lists them, `zosasm -unpack
file.o -o dir` writes them below `dir` (default `.`) with their relative
names, and assembling the main file there with the same options gives the
same object (tested). `-disasm` does not use the text (it still works from
ZSYM); that is future work.

**Class.** `C_ADATAE9C2`, an ED like the ZSYM class (NOLOAD, unstructured,
bind CAT, under the first SD), written after `C_ADATAE9C1` (both are the
last ESDs). `goff_equal` takes the number of skipped ESDs so `-disasm`
compares an object without either class. Like the first class, it does not
change a byte of the loaded image.

**Chunks** (`src/zsrc.c`, shared framing and integer coding in `src/zio.h`):
the same `'Z' 'S' version kind length payload crc32` framing as ZSYM, own
kinds: 1 HEAD (producer, level, the `-D` strings, number of files), 2 FILE
(name, length, crc32 of the original bytes, method, data), 3 END (number
of files). Method 0 is stored, 1 is an LZ4 block. A file is compressed only
when it is at least 64 bytes and the result is smaller. The reader checks
chunk crc, decompressed length and the crc of the original, and never
reads or writes outside its buffers (fuzzed in `tests/test_zsrc.c`); a
damaged block is reported incomplete and unpacked files are only those that
verified. The binder concatenates classes of several modules, so a reader
finds several blocks; `-unpack` writes the later ones under `dir/2/`,
`dir/3/` and so on.

**Compression** (`src/lz.c`): the LZ4 block format, written under the
reference encoder's end rules, so any LZ4 block decoder reads it (no
external library; the decoder was tested with hand made blocks and random
damage, not against another implementation: none is installed here). One
hash table of 2^14 entries, matches within 64 KB. Repetitive source text
compresses three times or better (`tests/test_zsrc.c`).

**Unpack names.** The name stored is the file name as read (relative to
the working directory, or as given for the main file). Names are made
harmless before writing: leading `/` and `.` components are dropped, `..`
becomes `__`, so a hostile object cannot write outside the directory.

**Not done.** `-disasm` from the stored sources; recording which `#include`
resolved to which file in a search directory (the name stored is the path
that was opened); `-I` directories are not stored (they are machine
specific).

## 15. The listing (`-l`, `src/listing.c`)

**What.** `-l FILE` (`-` = stdout) writes the listing after assembling,
also when there were errors (`main.c` then calls `asm_assemble_all`, which
returns the module whatever happened, and does not write the object).
`-ldata`, `-lpage N`, `-lbrief` (guide 11.2). There is no date or time in it,
so a listing is reproducible.

**Where the information comes from.** The listing is a view of data the
assembler keeps for it, none of which changes the object:

| need | source |
|---|---|
| lines, directives, skipped lines, expansion | `PpEvent` records of `pp_run` (`PpOptions.events`): one per source line (joined backslash lines are one), with the output lines (`first`, `n`) it produced |
| statement of a line | `Stmt.li`/`nli`: index of its first line (and the number of continuation lines) in the line array given to `read_statements` |
| location and bytes | `Module.zrecs` (the ZSYM records): class and offset of the final layout; `ZRec.pad` = alignment bytes before the first byte, which the row does not show; the bytes are `cls->data` |
| literals | `Module.lits` (`LitRec`, written by `asm_close_pool` in pass 2); `owner` is the zrec of the LTORG or END that holds the pool |
| USING table | `Module.usings`: a copy of each USING when it is made, the dropping line noted on DROP (`list_usings`) |
| cross reference | `Symbol.refs`, filled in pass 2 by `asm_note_ref` (`sym_value`, attribute and built-in references, ENTRY, XATTR, END) |

**Rows.** `listing_write` walks the events. A line that the preprocessor did
not change is one row with its statement; a changed one is shown as written,
then one `+` row per produced line. Rows of `#` directives and of lines
skipped by `#if` have no statement. The statement of a line maps to its
`ZRec` by a merge of `Module.stmts` and `Module.zrecs` (both in source order;
a statement that errored in pass 1 has no record). A synthetic END record
(the source had no END but literals were pooled) is listed after the last
row. Diagnostics are put after the last row of the event of their line
(`file:line` from the span); those without a line are listed at the end.

**Print control.** `TITLE`, `EJECT`, `SPACE`, `PRINT` and the pragma forms are
applied while walking: `PRINT OFF` hides rows (a row with a diagnostic is
shown anyway), `NOGEN` hides `+` rows, `DATA` shows all bytes. A page header
is written when the first line of the page is written, so a `TITLE` before it
is in it. Table sections start a page with `-lpage`.

**`CEJECT n`** starts a page when fewer than n lines are left (`ctl_word`
code 5; the row itself is hidden). `PUSH`/`POP` with `PRINT` save and restore
the print state (own stack of 64 in the listing; `NOPRINT` forms are hidden
like `PRINT` rows). **Carets:** `put_carets()` writes a caret line under the
source row of a diagnostic when the row shows the source text unchanged (not
for macro-expanded rows).

**Not done.** Keeping a statement's diagnostics on the same page; a cross reference use inside a
literal; a listing option for a different column layout.

## 16. Converting HLASM source (`hlasm2za`, `src/convert.c`, `src/regops.c`)

`hlasm2za` is a separate program (`build/hlasm2za`, `src/hlasm2za.c` holds only
`main`) around the library function `hlasm_convert`. It links the same objects
as `zosasm` (the instruction table, `split_operands`, the directive list).

**Stages.**

1. *Physical lines to statements* (`join_statement`). Tabs are expanded, CR
   removed. The source columns are `ICTL begin,end,continue` (default
   1,71,16; a leading `ICTL` changes them, `end` = 80 means no
   continuation). A non blank character after the end column is a
   continuation mark; the next line's text starts at the continue column.
   How the pieces are joined depends on where the first one ends: inside a
   quoted string (the piece is padded with blanks to the end column and the next
   piece is appended as it is), after a comma (trailing blanks are dropped and
   the leading blanks of the next piece are stripped), in a remark (the next
   piece is added to the remark), or in the middle of a token (appended as
   it is). Sequence numbers (after the continuation column) are dropped.
2. *Fields* (`split_fields`): name, operation, operands (up to the first blank
   outside quotes and parentheses, with the quote rules of the zosasm
   reader, `is_attr_quote` included), remark. Identifiers in name, operation
   and operands are folded to upper case outside quoted strings (`fold`; the
   digits of an `X'..'` string too; zosasm accepts lower case hex digits,
   so this is only for a uniform look). The
   text after an operation that takes no operands (`CSECT`, `LTORG`, an
   instruction whose variants all have zero slots) is a remark; it is
   moved to a comment line before the statement because zosasm would read
   it as operands.
3. *Registers* (`analyse_registers`, `write_stmt`): see below.
4. *Output* (`put_stmt`): name padded to 9, operation to 6 (operands start in
   column 16), remark in column 36 or two blanks after the operands; an operand
   list beyond the width (`-w`, default 100) is broken after a top level
   comma (zosasm continues a statement after a trailing comma).

**Registers.** `regops_map` (`src/regops.c`, also used by `-disasm
-strict-registers`) walks the register positions of a statement through a
callback: for an instruction the variant chosen for the operand count
(`asm_choose_variant`), its register slots, and inside an address `D(X,B)` /
`D(L,B)` / `D(R,B)` the index/register part (a length is not a register) and the
base; for `USING` every operand but the first, for `DROP` all. The converter
runs it twice. The analysis pass counts, for every symbol that an `EQU` makes
equal to a number (or to a symbol of that kind: an alias, which forms a group with
its target), the uses as a whole register operand per kind (r f a c v)
and all occurrences in operands (`count_names`; quoted strings skipped,
attribute references not). A group is *clean* when no member occurs other
than as a whole register operand, and then it is written `EQU %r<n>` if its
uses have one kind (or, with no uses, if the name suggests it: `R5` = 5) and
the uses keep the name. Otherwise the `EQU` stays and uses in register positions
become literals `%r<n>`. Names `R<n>`, `FR<n>`... that no `EQU` defines are
taken as that register when the kind fits (YREGS, `COPY` members), with a
warning. `YREGS` is replaced by `R0`..`R15` as ordinary `EQU` statements,
which then go through the same analysis.

**Left alone, reported.** Macro definitions are commented out (the macro name is
remembered to report its calls), `COPY X` becomes `#include "x.inc"`, `ISEQ`
is dropped, conditional assembly statements, `&` variables, sequence
symbols, unsupported directives (`OPSYN`, `PUSH`, `POP`, `ACONTROL`, `ADATA`,
`CEJECT`, `EXITCTL`, `REPRO`, `ALIAS`) and unknown operations (macro calls)
stay as written with a warning. Warnings are sorted by line.

**Tests.** `tests/test_convert.c` (layout, columns, continuation, ICTL,
remarks, registers, relaxed, width, unconverted), `testdata/hlasm/` (fixed
column inputs; `prog.za` is the expected output), the command line tests (strict and
relaxed output assemble to the same object with `-gz0`; the guide's
`hlasm-demo` pair), and a run over the macro library files for crashes.

**Not done.** Expansion of common IBM macros, `EQUREGS`, DC forms zosasm does not
have (`L.n`, `G`, `S`, `SY`, `QY`) are not checked, `-relaxed` keeps
EQU names unchanged, tabs in the middle of a statement are expanded to 8 column
stops (HLASM has no tabs), and the case of member names in `COPY` is always
folded to lower case.

### 16.1 Macro conversion (C1, `src/mconv.c`, `src/mflow.c`)

`macro_def()` in `convert.c` collects the logical statements of a MACRO..MEND
definition (`MHStmt`: name, operation, operands, remark; comments kept) and
calls `mconv_macro()`. When it returns NULL the definition is commented out as
before, with the reason in the warning. The member modes (`-m`, `-macro-dir`)
write only `*` comment lines and the macros. Columns are counted in characters
(`u8off`/`u8len`): the library is UTF-8 and `¬` etc. are multibyte.

*mconv.c* translates statement by statement into **nodes** (`MNode`, in
`mconv_int.h`): `N_CODE` (finished zosasm lines), `N_COND` (condition,
negated condition, target), `N_GOTO`, `N_RETURN`, `N_LABEL`. Expressions are
parsed into a typed tree (`E`: text, type int/string/bool, precedence) so
that `K'&X GT 0` becomes `len(x) > 0`, `'&X'(2,3)` `substr(x, 1, 3)` and
conditions can be negated (`!(a == b)` -> `a != b`, De Morgan). Model
statements keep `&name`; a pre-pass declares what was not declared and
resolves sequence symbols (an undefined one becomes `#error`). Zosasm line
rules that matter: a model operand ending in `,` is read as continued (the
converter adds `&{""}`), a label starting with `#` is quoted, a lone `&` is
`&&`.

*mflow.c* writes the nodes. `-flat` (or a failure) writes `#label`/`#goto`
one to one. Otherwise `structure()` recovers control flow by **ranges of
nodes**, not by a flow graph:

* a *point* is the first node after a label group (labels and `//` comments
  are transparent; `*` lines are output and count as code);
* `cls[point]`: 1 end of macro or `MEXIT`, 2 a block of at most 6 code nodes
  ending in `MEXIT`, 3 such a block at the end of the macro. Jumps to 1 and 2
  become `#return` (2 copies the block: the error exit idiom), to 3 only
  when no structure fits;
* `emit_body` walks a range. A forward `COND` to a label at `q` writes
  `#if (not cond) { [i+1,q) }`; when the node before `q` is a `GOTO` over a
  following region it writes `#if/#else`; a `COND` followed by a leaving
  `GOTO` or `MEXIT` is a guard clause; a `COND`+`GOTO` pair in the order
  "branch first" is turned around. A label with back jumps starts a loop:
  `#while (not cond)` when the head node is a `COND` to the loop exit, else
  `#while (true)`; jumps to the head are `#continue` (dropped at the end of
  the body), to the end `#break`. A range is only structured when no jump
  from outside enters it (`closed()`);
* everything else is a `#goto`, and the label is written where it is in the
  structure (`#label`). Zosasm allows a `#goto` out of blocks but not into
  one, so `gotos_valid()` checks every goto against the block tree (`bpar`);
  when one enters a block the macro is written flat (`mflow_write` returns
  -1 and `mconv_macro` warns);
* `try_chain()` handles a run of `COND`s that jump forward to blocks which each
  end in a `GOTO` to one join (or leave): `AIF a .A / AIF b .B / AGO .D / .A.. AGO .J /
  .B.. AGO .J / .D.. / .J`. Every block must be entered only by its test (a
  `closed()`-like check over all jumps), the join comes after the last block,
  and the default block is either the one the trailing `GOTO` names or the code
  that follows the tests. It writes `#if / #else if / #else`; a join that is a
  short exit block (class 2 or 3) is not copied into the arms but stays after
  the chain;
* `closed()` ignores jumps whose target is a class 1-3 block (they are copied,
  never entered), which structures many more error-exit macros;
* code after an unconditional jump is dead and skipped up to the next live
  label (`//` comments are kept). `landed[]` makes a class 3 block live only
  when a structure ends there. The emission runs until `need` (labels a goto
  targets) and `landed` stop changing.

*Default fallback.* When `structure()` fails or leaves a `#goto`, `write_dispatch()`
writes a **dispatch loop**: `#let pc = "(start)"`, `#while (true) { #switch (pc) { ... } }`.
A block starts at a label some jump names (case name = the first such label of a
group), ends by setting `pc` and `#continue`, and a block that falls into the next
one sets `pc` and leaves the switch (zosasm's `#switch` has no fall through, so the
`#while` runs it again). The variable is `pc`, then `pc_`, `pc__` ... if the macro
has a parameter or declared name of that name (`mconv.c`). `-keep-goto` skips the
fallback and keeps the old result (partial `#goto`, or the whole macro flat when a
`#goto` would enter a block); `-flat` writes `#goto`/`#label` for everything.

*mpost.c* runs over the text of a structured body (not the nodes): `#} else {`
followed by only an `#if` becomes `#} else if (...) {` (the inner lines are
de-indented), and a chain of three or more arms whose conditions are all
`LHS == literal` (or `||` of those) with one `LHS`, no repeated value and no
`#break`/`#continue` inside becomes `#switch (LHS)` with `#case a, b:` /
`#default:`. The dispatch loop is never rewritten.

**Tests.** `tests/test_mconv.c` converts thirteen idiom macros flat and
structured and expands both for several argument sets: texts and sorted
diagnostic codes must be equal, and the shapes of the common idioms are
checked (`else if`, `#switch`, the dispatch loop, `-keep-goto`).
`tests/maclib_check.sh` (`ZASM_MACLIB=DIR make test-maclib`, local: the IBM
library is not in the repository; `ZASM_CONV_FLAGS=-keep-goto` checks the
other form) does that for a whole library and compiles every result. On
`SYS1.MACLIB` (2080 members, 2076 with a definition) 2210 macros convert
(nested macros and `COPY` members are counted) and every output compiles;
with the default dispatch fallback **no macro has a `#goto`**, and 1128 of
the 2076 outputs use a dispatch loop. With `-keep-goto` 948 outputs need no
`#goto` (891 before `else if`, fork/join chains and the class 1-3 rule of
`closed()`) and 72,413 `#goto` remain (73,884 before). 8 members are not
converted (4 PL/S members without `MEND`, `IXGCON` with stray text, `IGWRMRCD`
with a `!` operand, `IHBRDWRA` with `LCLB @IDS3270` without `&`, `CPF` with `?`
in an expression). Not compared with HLASM.

**Not done.** `SETAF`/`SETCF`, computed `AGO`, created `&(` symbols, `C'x'`
terms in conditional assembly expressions, `AREAD` with operands, macro
members that are not valid HLASM. A
`#goto` that leaves a nested loop towards a label after it is written by the
dispatch loop, not as a labelled `#break`.

## 17. ELF output and the ASCII character set (`src/elf.c`, `src/ebcdic.c`)

**Why.** The assembler model (sections, classes, relocations with a symbol or
class target and an inline addend) does not depend on GOFF, so a second
writer can sit next to `emit.c`. `-elf` writes a relocatable ELF64 object for
Linux on Z, big endian, `EM_S390`, `ET_REL`; the user's guide section 17 has the
mapping table.

**Decisions.**

* One ELF section per class (not per CSECT), because a class is the unit with
  its own alignment, read-only and no-load attributes. A GOFF CSECT therefore
  becomes `.text.NAME` + `.data.NAME.CLASS`..., which a GNU linker script merges
  into `.text` and `.data` (`.text.*`, `.data.*`, `.rodata.*`, `.bss.*`).
* Relocations use RELA. The inline value (which the GOFF writer leaves in the
  text) is read as the addend and the field is cleared. A class target uses
  the `STT_SECTION` symbol of the target section; a label target (also of the
  CSECT name) is that section symbol plus the label offset, so local symbols
  are not needed to link; they are written for debugging.
* Relative immediates (`REL_RI`): the PC of the instruction is two bytes
  before the field in every format that has a relocatable 16 or 32 bit
  immediate, so the addend is `2 * inline + field offset`, which for an
  external symbol is the usual +2. Branch instructions (`A7x4/5`, `C0x4/5`) to
  external symbols get the PLT variants, which GNU `ld` handles like the plain
  ones for defined symbols and needs for shared objects; `LARL` stays PC.
* Not representable: `Q`, `J`, `R`, `CXD`, `LEN` (GOFF specific), several terms
  at one offset (ELF has one relocation per field), subtracted terms, 3 byte
  address constants. They give an error instead of a wrong object.
* The class `esd` field is reused as the ELF section number (1 based). It is
  cleared first, so a module emitted as GOFF and then as ELF still works.

**Character set.** `za_ascii` (`ebcdic.c`) selects ISO-8859-1 instead of
IBM-1047 in `charset_from_source` (DC `C`, `CL`, literals through DC, the
`C'..'` self-defining term) and in `ZA_E` (type attribute and the attribute of
unknown names). Padding of character constants becomes `0x20`. `CE` always
uses `ebcdic_from_source`; `CA` was always ASCII (also as self-defining terms `CA'a'`/`CE'a'` in expressions, `parse_quoted` in `expr.c`, which switches `za_ascii` for the one term). Names in GOFF
(`emit.c`, ZSYM) never use the flag. `-elf` sets it unless `-charset` is given.

**Predefined macros.** `pp_run` defines `__ELF__` (`PpOptions.elf`, set by `-elf`) or `__GOFF__`, and `__ASCII__` or `__EBCDIC__` from `za_ascii`, before the `-D` options. They are normal macros (a `-D` of the same name is a ZA4023 warning), not built-in names, and are not recorded as `-D` options in the `-gz2` object.

**Tests.** `tests/test_elf.c` parses the object with a small ELF reader
(sections, flags, symbol binding and order, relocation types and addends,
common symbols, rejected constructs, character set); command line tests
check the magic, `-charset` and the error exit; during development the
objects were linked with `s390x-linux-gnu-ld` and read with
`s390x-linux-gnu-objdump` (the displacements were correct).

**Not done.** 31-bit `ELFCLASS32` (Linux on Z has been 64-bit only for
years), debug information (`.debug_*`, `-gz` is ignored), a `-elf` listing
note, `.note` sections for the entry point, `SHT_GROUP` for `COMDAT`-like
merging, and a run on a real machine or under `qemu-s390x`.

## 18. The MCP server (`src/mcp.c`)

`build/zosasm-mcp` is a Model Context Protocol server for AI agents (guide
chapter 18). It is one C file with its own JSON reader and writer (the rest
of zosasm has no JSON) and uses POSIX (`popen`, `mkdtemp`, `realpath`,
`setenv`); only `util.o` is linked.

**Transport and protocol.** MCP "stdio": one JSON-RPC 2.0 message per line.
Methods: `initialize` (answers with the client's protocol version if it is a
known one, else `2025-06-18`; tools and resources capabilities, and
`instructions`), `ping`, `tools/list`, `tools/call`, `resources/list`,
`resources/templates/list`, `resources/read`, `prompts/list` (empty);
notifications are ignored; unknown methods give -32601, unparsable lines
-32700 and unknown tools -32602 (as the specification asks). Batches and
server-initiated requests (sampling, roots) are not used.

**No assembler state.** The tools run `zosasm` and `hlasm2za` as
subprocesses (found next to the server, or `ZOSASM_BIN_DIR`) in a temporary
directory (`mkdtemp` below `$TMPDIR`, removed after each call). Source passed
inline is written to `input.za` (or `name`) and `files` next to it, which is
also added with `-I` and `-M` (so a `NAME.zmac` in `files` is a library macro; `macro_dirs`, `sysparm` and
`macro_steps` map to `-M`, `-sysparm` and `-macro-steps`). `zosasm_convert_hlasm` passes `macros`, `flat`,
`keep_goto`, `comment_macros`, `copy_dir` and, for a whole library, `macro_dir` + `output_dir` (the one tool
that writes files outside the temporary directory; it is therefore not marked read only). The temporary directory name is removed from the
diagnostics and the listing so that results are reproducible. Standard error
is redirected to a file and read back, so a failing program cannot block the
pipe. A tool failure is a result with `isError` true, not a JSON-RPC error,
so that the agent sees the diagnostics.

**Self describing.** Each tool has a description (input, output, where in the
guide to read more) and a JSON schema of its arguments with a description of
each. The strings contain the absolute path of `GUIDE.md`, found next to the
program (`../docs/GUIDE.md`), through `ZOSASM_GUIDE`/`--guide`, or the
build directory layout. `zosasm_guide` selects sections by the `##`/`###`
headings (number, or a part of the title) and searches lines; the same text
is `zosasm://guide`. Examples are listed from `docs/examples`.

**Tests.** `tests/run_cli_tests.sh` feeds JSON lines to the server and checks
the replies (initialize, tool list, assemble ok/error/ELF/`files`, preprocess,
guide section and search, resources, unknown method, parse error, and the
object round trip through `object_base64`, `zosasm_dump`, `zosasm_disasm`).

**Not done.** Prompts, progress notifications, cancellation, the HTTP
transport, JSON-RPC batches, restricting `path`/`output` to a directory, a
`zosasm_link`-style tool (there is nothing to link with here).

## 19. Macro language (M1 implemented)

See `MACRO.md`: a C-like macro language in the preprocessor stage (braced `#if/#while/#for/#switch` blocks written on `#` lines with free indentation, model statements substituted with `&name`, 0-based lists, block scoped variables, library lookup of macros by name). It keeps the semantics of HLASM conditional assembly, not its syntax; `hlasm2za` is to recover structured control flow from `AIF`/`AGO` (the survey of SYS1.MACLIB is in MACRO.md section 0). **M1 is implemented** (2026-11-16, not compared with HLASM): `src/macro.h` (API), `src/macro_int.h`, `src/macro_val.c` (values, list parsing, functions), `src/macro_expr.c` (lexer, Pratt parser, evaluator), `src/macro.c` (collector, compiler, runtime, library). A `#` line is classified by `mac_top_kind` (0 not macro, 1 `#macro`, 2 other macro statement; a classic `#if` without `{` stays a preprocessor directive). Bodies are compiled once to a flat `MStmt` array with jumps; every statement has a block depth and a variable lives at the depth of its block, being dropped when a statement of a lower depth runs (block scope without a scope stack; `#for` hidden variables `%lN`/`%iN` and the switch value `%sN` use the same rule, the first hidden `#let` carries the truncation depth). Frames are per call, `ms->top` is the open-code frame, globals are shared `MVal*` slots. `pp.c` provides the host callbacks (`emit`, `sect`, `is_defined`); generated lines are rescanned by `emit_line` (so they can call macros), multi-line statements travel as text with `\n`, calls are found in `emit_part_or_call` and `pp_file` (a call ending in a comma is joined first), library members by `is_macro_op`/`try_library_dir` (`-M` then `-I`, negative lookups cached in `pp->lib_tried`). Diagnostics ZA4050-4052, ZA4100-4130; `rep_notef` is a note severity that is not counted. Names that are instructions or directives (`mac_known_op`) are never looked up in the library. C1 (`hlasm2za` conversion of MACRO..MEND) is implemented, see section 16.1.

## 20. Oracle comparison with IBM HLASM (2026-11)

Tools: `tools/oracle.sh` (runs `as -mgoff` on a z/OS host over `ssh`, fetches
the digest of the listing made by `tools/oracle_cmp.py --digest`, assembles the
same source with zosasm and compares per statement; `ZORACLE_HOST`,
`ZORACLE_DIR`, ...), `tools/oracle_cmp.py` (statement pairing by source line
number, tolerance for alignment padding, which HLASM lists as trailing zeros),
`tools/oracle_gen.c` + `tools/oracle_probe.py` + `tools/oracle_rules.py`
(generate probes of every instruction operand and turn the HLASM answers into
`isa/oracle_rules.json`; `tools/pop2json.py` merges it into `isa/isa.json`,
`src/isa.c` has `isa_rule()` and `isa_rules_ok()`). Probe sources are in
`testdata/oracle/` (at most 71 columns; the second statement is `PRINT DATA`
because HLASM lists only the first 8 bytes of a DC otherwise).

What came out of it: the instruction diagnostics ZA2080-ZA2084, the DC/DS
length windows and value rules (`dc_len_range` in `dc.c`, ZA2086 for a 2 byte
relocatable address, `fixed_bytes`, `dc_decimal`, `split_nominal` dropping
blanks, `float_set_type`/`hfp_core` in `float.c`), term limits in `expr.c`,
GOFF details in `emit.c` (SD flags, a local label as LABEL referent, entry
AMODE/RMODE) and the ELF addend. Remaining differences are listed in
PLAN.md ("Oracle checks"). The listings of HLASM and the generated probe
output are IBM material and are not stored in the repository; only our own
sources are.

**Binder check** (`tools/oracle_ld.sh`, `testdata/oracle/ld/`): objects made by zosasm and by HLASM are interchangeable for `ld`/`xlc`; the four mixes give the same result at run time. The same holds for plain `ld -o x obj` of `g1`, `g2`, `g3`, `dc2b`: the same IEW messages (only the expected unresolved externals). Observation: an `A(SDNAME)` stored in a second class (`CATTR`) of the same CSECT is 504 bytes off after binding with HLASM's own object as well (no class: correct); not investigated.

**Object comparison** (`tools/oracle_obj.sh`, `tools/oracle_obj.py`, 2026-11-16): the HLASM object is fetched with z-scp and read by `zosasm -dump`, as is ours; the script compares the ESD tree (names path, attributes), text and RLD items structurally. It ignores the extra `B_IDRL` and an empty `B_PRV`, `C_ADATA*`, the class offsets of the HLASM location space (HLASM puts one space over all classes of a section; zosasm's offsets are class relative) and the ED offset stored in A-con and relative-immediate text. Found and fixed: CATTR defaults (`ClassAttr.cattr`), CNOP pad bytes, COM, Q/QY referent CLASS, B_PRV/DXD attributes, owner of a pseudo register (`Section.prv_owner`, `dir_dxd`), `R(local)` ER (`Symbol.r_er`), one-sided AMODE/RMODE, CSECT resume (`Section.text_cur`), LOCTR pieces without padding. The last needs an alignment that depends on the final start of the piece: `asm_assemble_all` repeats the assembly with the starts of the previous run (`Piece.hint`, a map keyed `section\x01class\x01piece`) until all starts agree modulo 4096; after 5 rounds it falls back to doubleword aligned piece starts (`Asm.round8`). A module without a second location counter is assembled once. Diagnostics of the discarded runs are removed with `rep_mark`/`rep_rewind`.
