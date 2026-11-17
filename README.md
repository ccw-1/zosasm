# zosasm

A cross assembler for an HLASM-like language that writes z/OS GOFF object
files. Portable C99; it needs only a C compiler and `make`.

## Building

```sh
make          # build/zosasm, build/hlasm2za, build/zafmt and build/zosasm-mcp
make test     # unit tests and command line tests
make clean
```

Any C99 compiler works (`make CC=clang`). No generator, python or other tool
is run at build time: the instruction table (`src/isa_tab.c`) and the EBCDIC
tables (`src/ebcdic_tab.c`) are generated files that are checked in.

## Using it

```sh
build/zosasm -o hello.o testdata/hello.za   # assemble
build/zosasm -dump hello.o                  # print the GOFF structure
build/zosasm -disasm hello.o -o back.za     # GOFF object -> zosasm source
build/zosasm -E prog.za                     # show the preprocessed source
build/zosasm -l prog.lst -o prog.o prog.za  # assemble and write a listing
build/hlasm2za old.asm                      # HLASM source -> zosasm source (old.za)
build/zosasm -elf -o prog.o prog.za         # ELF64 s390x object for Linux on Z
build/zafmt -i prog.za                      # format the source (guide chapter 20)
```

Options: `-o FILE`, `-DNAME[=VAL]`, `-Idir`, `-E`, `-dump FILE`,
`-disasm FILE`, `-unpack FILE`, `-generic`, `-elf`, `-ascii`, `-charset NAME`, `-gz0`, `-gz1`, `-gz2`, `-reproducible`, `-strict-registers`,
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

Macros: a C-like macro language (`#macro NAME(a, b = 1) {` ... `#}`, `#if`,
`#for`, `#switch`, `&name` substitution, library lookup with `-M`) gives the
power of HLASM conditional assembly (guide chapter 19, `MACRO.md`).

`hlasm2za FILE.asm` converts HLASM source (card columns, upper case, `COPY`,
registers as numbers or `EQU` names) to zosasm source, by default with
`%r1` registers so that the result assembles with `-strict-registers`.
`MACRO`..`MEND` definitions become zosasm macros with the control flow
recovered (`-m` for a library member, `-macro-dir` for a library, `-flat`
for `#goto`); open code conditional assembly becomes a synthetic macro
`OPENCODE` (guide section 16, DESIGN.md section 16).

`zosasm -elf FILE.za` writes a relocatable ELF64 object for Linux on Z (big
endian, `EM_S390`) instead of GOFF, with ASCII character constants; `-ascii`
and `-charset ascii|ebcdic` choose the character set separately (guide
section 17, DESIGN.md section 17).

## For AI agents

`build/zosasm-mcp` is an MCP (Model Context Protocol) server on stdio. An
agent can assemble, preprocess, dump, disassemble and convert HLASM source
through typed tools and read the user's guide (`docs/GUIDE.md`) through the
`zosasm_guide` tool or the `zosasm://guide` resource. Register
`/path/to/zosasm/build/zosasm-mcp` as a command in the MCP client; the
`initialize` reply tells the agent where the guide is. Details: guide
chapter 18, `DESIGN.md` section 18.

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
