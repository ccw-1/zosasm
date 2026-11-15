# zosasm

A cross assembler for an HLASM-like language that writes z/OS GOFF object
files. Portable C99; it needs only a C compiler and `make`.

## Building

```sh
make          # build/zosasm
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
build/zosasm -E prog.za                     # show the preprocessed source
```

Options: `-o FILE`, `-DNAME[=VAL]`, `-Idir`, `-E`, `-dump FILE`, `-color`,
`-max-errors N`, `-Werror`. Source files use the extension `.za`.

Registers can be written as numbers, as names (`R1`, equated or predefined)
or GNU style: `lgr %r1,%r2`, `ler %f2,%f4`, `vl %v1,0(%r2)`.

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

* `DESIGN.md` - design, source syntax, GOFF mapping, instruction table
  (section 5), diagnostics, tests.
* `PLAN.md` - status and open work.
