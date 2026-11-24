# zosasm

A cross assembler for an HLASM-like language that writes z/OS GOFF object
files. Portable C99; it needs only a C compiler and `make`.

Three workflows are supported:

* **Assemble zosasm source** (`.za`) directly into a GOFF object.
* **Convert existing HLASM source** (`.asm`) to zosasm source with
  `hlasm2za`, then assemble it. This includes HLASM `MACRO`..`MEND`
  definitions, which are recovered as structured zosasm macros (`#macro`).
* **Convert whole HLASM macro libraries**: `hlasm2za -macro-dir` can convert
  an entire `SYS1.MACLIB`-style library to `.zmac` files for use with
  `zosasm -Mdir`.

## Quick start: convert HLASM to a GOFF object

```sh
make                              # build zosasm,hlasm2za,zafmt,zosasm-mcp

build/hlasm2za old.asm            # HLASM source -> old.za (zosasm source)
build/zosasm -o old.o old.za      # assemble old.za -> GOFF object
build/zosasm -dump old.o          # print the GOFF structure (ESD/TXT/RLD/END)
build/zosasm -disasm old.o -o back.za   # GOFF object -> zosasm source
```

## Building

```sh
make          # build/zosasm, build/hlasm2za, build/zafmt and build/zosasm-mcp
make test     # unit tests and command line tests
make clean
```

Any C99 compiler works (`make CC=clang`). No generator, python or other tool
is run at build time: the instruction table (`src/isa_tab.c`) and the EBCDIC
tables (`src/ebcdic_tab.c`) are generated files that are checked in.

`make test` itself needs `python3`, `od`, `seq` and `strings` for the command
line tests. `make isa-check` and `make ebcdic-check` additionally need
`python3` and, for the latter, `iconv`; they are run by CI.

## Converting from HLASM (`hlasm2za`)

`hlasm2za` converts existing HLASM source into zosasm source, so you can
assemble it with `zosasm` and keep using normal source-control and editing
workflows.

```sh
build/hlasm2za old.asm                 # one source file -> old.za
build/hlasm2za -m sys1.maclib/IHASAVER.mac           # one library member
build/hlasm2za -macro-dir sys1.maclib -o out_macros  # whole SYS1.MACLIB
```

### HLASM source conversion

* Reads HLASM source (card columns, upper case, `COPY`, registers as numbers
  or `EQU` names) and writes `.za` source.
* Converts registers to `%rN` by default so the output assembles with
  `-strict-registers` (`-relaxed` keeps relaxed register form).
* Converts `COPY` to `#insert`; maps `&SYS*` symbols to
  `sys.loc|date|datc|time|parm`.
* Converts open-code conditional assembly (`AIF`, `AGO`, `ANOP`, `LCL`,
  `GBL`, `SETA`, `SETB`, `SETC`) into a synthetic `OPENCODE` macro.

### HLASM macro conversion

HLASM `MACRO`..`MEND` definitions are converted into zosasm macros
(`#macro ... #}`):

* Recovers control flow from `IF/AGO`-style branches into structured
  `#if/#while/#switch` and dispatch loops, so no `#goto` is left in the
  structured output.
* `-flat`/`-keep-goto` preserves the older `#goto`-based form when needed.
* Handles macro libraries with `-m` (single member), `-macro-dir` (whole
  library), `-copy-dir`, `-include-extension`, and `-sysparm STR`.

### SYS1.MACLIB conversion

The converter is designed to handle real HLASM macro libraries, not only
small examples. `make test-maclib` (`ZASM_MACLIB=/path/to/SYS1.MACLIB`)
converts and compiles a real library. On a local SYS1.MACLIB copy, it:

* converted **2080** members and **2214** macros
* had **0** compile failures
* had **0** expansion differences (4286 expansions compared)
* left **0** `#goto` in structured output
* reported the members that are not assembler macros separately

Guide: `docs/GUIDE.md` section 16; `DESIGN.md` section 16.

## Using zosasm

```sh
build/zosasm -o hello.o testdata/hello.za   # assemble
build/zosasm -dump hello.o                  # print the GOFF structure
build/zosasm -disasm hello.o -o back.za     # GOFF object -> zosasm source
build/zosasm -E prog.za                     # show the preprocessed source
build/zosasm -l prog.lst -o prog.o prog.za  # assemble and write a listing
build/zosasm -elf -o prog.o prog.za         # ELF64 s390x object for Linux on Z
build/zafmt -i prog.za                      # format the source (guide chapter 20)
build/zosasm -g -gdir /u/me -o prog.o prog.za  # debug info for LLDB inside prog.o (chapter 21); -gside adds prog.dbg for dbx
```

Options: `-o FILE`, `-DNAME[=VAL]`, `-Idir`, `-Mdir`, `-sysparm STR`, `-macro-steps N`, `-E`, `-dump FILE`,
`-disasm FILE`, `-unpack FILE`, `-generic`, `-elf`, `-ascii`, `-charset NAME`, `-gz0`, `-gz1`, `-gz2`, `-g`, `-gside`, `-gdwarf`, `-gdir`, `-gmap`, `-gdbgpath`, `-reproducible`, `-strict-registers`, `-diag text|json`,
`-l FILE`, `-ldata`, `-lpage N`, `-lbrief`, `-version`, `-color`, `-max-errors N`, `-Werror`. Source files use the extension `.za`.

Assembly time helpers (DESIGN.md sections 8 and 12): `sizeof(x)`,
`offsetof(S,f)`, `alignof(x)`, `countof(x)`, comparisons (`==  <  &&  !` ...),
`ASSERT sizeof(S)<=256,'S is too big'`, and the preprocessor names
`__COUNTER__`, `__BASE_FILE__`, `__INCLUDE_LEVEL__`, `__ZASM_VERSION__`,
`__DATE__`, `__TIME__` (`SOURCE_DATE_EPOCH`, `-reproducible`) with the
stringify operator `#(x)`.

By default the object carries ZSYM (`-gz1`): symbols, EQUs and the operands
of every statement in a NOLOAD class `C_ADATAE9C1` that the loader ignores
(DESIGN.md section 11). `-gz0` leaves it out; the object is then exactly the
code, data and relocations. `-gz2` also keeps the original source files (main file and
`#include`s, compressed) in a second NOLOAD class `C_ADATAE9C2`; `zosasm -unpack
file.o -o dir` writes them back (DESIGN.md section 14).

`-disasm` of an object with ZSYM gives the original statements back (names,
EQUs, USING, DSECTs, literals), a section whose bytes were changed after
assembling is written by the generic converter, and `-generic` ignores ZSYM.

`-disasm` turns a GOFF object back into source (no comments, labels are
generated: `L0001C`). It assembles the result again and warns if the object
differs from the input; for objects made by zosasm the round trip is exact
(see DESIGN.md section 10).

Registers can be written as numbers, as names you defined (`R1 EQU 1`; a bare
`R1` is not predefined) or GNU style: `lgr %r1,%R2`, `ler %f2,%f4`, `vl %v1,0(%r2)`. With
`-strict-registers` only the GNU style and names made with `NAME EQU %r12`
are accepted in register positions (guide section 6.1).

## HLASM macros

zosasm's macro language (`#macro NAME(a, b = 1) {` ... `#}`, `#if`, `#for`,
`#switch`, `&name` substitution, library lookup with `-M`) gives the power of
HLASM conditional assembly (guide chapter 19, `MACRO.md`). `hlasm2za` converts
existing HLASM `MACRO`..`MEND` definitions into these zosasm macros, recovering
the control flow so the result is structured (`-m`, `-macro-dir`, `-flat`).

## For AI agents: generate z/OS object code from user input

AI agents can generate z/OS object files (GOFF, or ELF64 s390x for Linux on Z) from zosasm (.za) input. zosasm assembles that input into an object file without a compiler, assembler, or z/OS toolchain. It emits object files only, not program objects, and the binder output remains proprietary.

`build/zosasm-mcp` is an MCP (Model Context Protocol) server on stdio that exposes typed tools so an agent can:

* assemble zosasm (`.za`) source into a GOFF object (`zosasm_assemble`),
* preprocess source (`zosasm_preprocess`),
* dump the structure of a GOFF object (`zosasm_dump`),
* turn a GOFF object back into source (`zosasm_disasm`),
* convert existing HLASM source (`.asm`) to zosasm source (`zosasm_convert_hlasm`),
* read the user's guide (`zosasm_guide`, `zosasm://guide` resource).

Register `/path/to/zosasm/build/zosasm-mcp` as a command in the MCP client;
the `initialize` reply tells the agent where the guide is. Details: guide
chapter 18, `DESIGN.md` section 18.

`make test-mcp-asan` runs the MCP harness with ASan/UBSan+LSan
(`detect_leaks=1`). Core assembler tests use `ASAN_OPTIONS=detect_leaks=0`
because one-shot CLI allocations are intentional; MCP is long-lived, so
request-scope leaks are treated as bugs.

## Regenerating the instruction table (developers only)

You only need this to change or refresh the instruction set.

```
SA22-7832-14.pdf --> tools/pop2json.py --> isa/isa.json --> tools/isagen.c --> src/isa_tab.c
```

```sh
python3 tools/pop2json.py SA22-7832-14.pdf > isa/isa.json   # needs python3 and pdftotext
make isa                                                    # isa/isa.json -> src/isa_tab.c
make test
```

`make isa` alone (after editing `isa/isa.json`) needs only the C compiler.
`isa/isa.json` is not a dependency of `make`. The data comes from IBM's
*z/Architecture Principles of Operation* only.

## Documentation

* `docs/GUIDE.md` - the **User's Guide**: the language, the tool and worked
  examples (`docs/examples/`, all assembled by `make test`).
  `docs/zosasm-users-guide.pdf` is the same text as a PDF (`make pdf`; needs
  pandoc and XeLaTeX; the PDF carries the version of `src/version.h`).
* `CHANGELOG.md` - what each version contains.
* `DESIGN.md` - design, source syntax, GOFF mapping, instruction table
  (section 5), diagnostics, tests.
* `PLAN.md` - status and open work.

## Versions

The version is in `src/version.h` (`zosasm -version`, the listing header, the
`__ZASM_VERSION__` preprocessor name, the guide PDF) and follows semantic
versioning: 0.x until the output has been checked against IBM's tools for
the whole language. Releases are git tags `vX.Y.Z`; `CHANGELOG.md` lists the
changes.
