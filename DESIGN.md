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
  `ALIAS`, `DXD`, `CXD`, `END`,
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
| `DXD`, `CXD`, `Q` constants | PR in `B_PRV`, RLD of type Q/CXD |
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
