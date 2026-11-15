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
* listing-control statements accepted and ignored: `TITLE`, `EJECT`,
  `SPACE`, `PRINT`, `PUNCH`, `ICTL`, `ISEQ` (real HLASM sources still
  assemble; the preprocessor's `#pragma` is the intended form, see 8),
* good diagnostics (see 6).

Out (for now):

* the HLASM **macro language and conditional assembly** (`MACRO`, `&vars`,
  `AIF`, `SETC`...) and `COPY`. They are replaced by a C style
  preprocessor stage (see 8); `COPY` is an error (`ZA9001`) with the hint
  "use #include". The statement reader takes an array of source lines, so
  the preprocessor sits in front of it without the core knowing,
* ESD-less OBJ/XOBJ output, `ADATA`, `SYSLIB` macro libraries, listings in
  HLASM layout.

## 2. Source syntax

Differences from HLASM:

| HLASM | zosasm |
|---|---|
| 80-column card images, columns 72/16 | **no column rules**; a line may be any length |
| continuation = non-blank in column 72 | continuation = operand field ends in `,` (see below) |
| names fold to upper case | **symbols are case sensitive**; mnemonics, directives, register names and attribute letters are not. `-fold` may be added later |
| registers are numbers or EQU'd names (`LGR R1,R2` needs `R1 EQU 1`, unless predefined) | all three forms work: `LGR 1,2`, `LGR R1,R2` (`R0`-`R15`, `F0`.., `A0`.., `C0`.., `V0`.. are predefined unless the program defines the symbol) and the GNU style `LGR %r1,%r2` (`%r` general, `%f` floating point, `%a` access, `%c` control, `%v` vector; also in `D(%r1,%r2)` and `USING`/`DROP`). The register class is checked (`%f1` where a general register is needed is ZA2055) |
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
* `-W` flags select warnings (`-Werror` makes them fatal), `-color`,
  `-diag-format=json` produces machine readable output (editor integration).

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
  `-DNAME=val`, `-Idir`, `-E` (print the preprocessed lines and stop);
  `__FILE__` (bare file name), `__LINE__`.
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
  `space n`, `print on|off`, later `arch z13`. Today they are validated
  and have no effect (there is no listing yet). The HLASM spellings
  (`TITLE`, `EJECT`, `SPACE`, `PRINT`, `PUNCH`, `ICTL`, `ISEQ`) stay
  accepted no-ops; no hint toward the pragma form is given, to keep real
  HLASM sources free of noise.
* Only the operation, name and operand fields are expanded; the remark
  and comment lines are not (`remark_start` uses the reader's operand
  scanner). Arguments are pre-expanded except next to `##`; a macro is not
  expanded inside its own expansion.
* Error codes: `ZA4010-4012` include, `ZA4020-4023` define/undef,
  `ZA4030-4034` conditionals, `ZA4040-4042` macro calls, `ZA4050/4051`
  `#error`/`#warning`, `ZA4060/4061` pragma, `ZA4070` comma continuation.
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
  true only for symbols defined *before* the statement. `S'` is always 0
  until scale modifiers exist; `I'` follows the HLASM formulas (F/H
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
  uses RLD action SUB, reference type 7, several SDs and ED/PR combinations.
* Warnings mean the object could not have been written by the assembler;
  then only valid source and equal text bytes are guaranteed, not a fixed
  point.
* A class can have 64 MiB at most.
* `CATTR` cannot change the existing `B_TEXT`; `RSECT` stands for read-only.
* Not implemented (reader, writer or both): `ADAEsdId` of an LD,
  extended-attribute ESDID, SD tasking and binding scope, ER indirect bit,
  action SUB relocation pairs (`A(X-Y)`), reference type 7, LEN records from
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
