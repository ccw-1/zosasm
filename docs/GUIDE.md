# zosasm User's Guide

`zosasm` is an assembler that runs on any machine with a C compiler and
writes **z/OS GOFF object files**. The language is close to IBM's High Level
Assembler (HLASM), with a few deliberate differences: no card columns, a C
style preprocessor and a C like macro language instead of HLASM macros,
symbols without length limit, and a set of assembly-time helpers (`sizeof`, `ASSERT`, ...). This guide teaches
the language and the tool; the *why* of each decision is in `DESIGN.md`.

**What to expect.** The object files follow IBM's GOFF documentation, and the
instruction table is generated from the *z/Architecture Principles of
Operation*. An oracle run on z/OS (HLASM 6.0 and the binder, 2026-11)
compared instruction operand rules, DC/DS rules and the objects of 26 probe
programs, and linked mixes of IBM and zosasm objects; section 14 says what
was covered. Where this guide says that something is "not verified against
IBM", treat the exact bytes as an interpretation of the documentation until a
z/OS test has been done. Section 14 lists what is affected.

**Conventions.** `monospace` is source text or a command. Square brackets
mean "optional". Example programs are in `docs/examples/`; every program
printed here is that file, and the test suite assembles each of them.

## Contents

1. Getting started
2. The source language
3. The preprocessor
4. Sections, classes and the object structure
5. Data: DC, DS, EQU, ORG, CNOP and literals
6. Instructions and registers
7. USING, DSECT and qualified names
8. External dummy sections
9. Built-in functions and ASSERT
10. Floating point, UTF-16, CCW and address differences
11. Diagnostics and the listing
12. Looking inside an object: `-dump`, ZSYM, `-disasm`, `-unpack`
13. Command reference
14. Differences from HLASM and known limits
15. Recipes: a subroutine, a table, build variants, source round trip, AMODE 31 and 64
16. Converting HLASM source: `hlasm2za`
17. Linux on Z: ELF output (`-elf`) and the ASCII character set
18. For AI agents: the MCP server `zosasm-mcp`
19. Macros: the C-like macro language
20. Formatting source: `zafmt`
21. Debug information for dbx and LLDB: `-g`

### Feature index

| I want to ... | see | example |
|---|---|---|
| assemble a first program, see what comes out | 1 | `hello.za` |
| write names, numbers, expressions, quoted symbols | 2 | `symbols.za` |
| use `#include`, `#define`, `#if`, `-D`, stringify | 3, 15.3 | `prep.za`, `config.za` |
| write macros with loops, `#if`, `#switch`, keyword arguments, libraries | 19 | `cmacros.za` |
| control sections, classes, entry points, externals | 4 | `sections.za` |
| define data: `DC`, `DS`, `EQU`, `ORG`, literals | 5 | `data.za` |
| write instructions, name registers, require `%r1` notation | 6, 6.1 | `symbols.za`, `strict.za` |
| map storage with DSECTs, `USING` (labeled, dependent, ranges) | 7 | `using.za` |
| use external dummy sections (`DXD`, `CXD`, `Q`-type) | 8 | `xd.za` |
| measure with `sizeof`, `offsetof`, `countof`; check with `ASSERT` | 9 | `builtins.za` |
| floating point, UTF-16, `CCW`, `A(X-Y)` | 10 | `floats.za` |
| understand an error or warning | 11.1 | |
| get an assembly listing, control it with `PRINT`, `TITLE`, `EJECT` | 11.2 | |
| dump, disassemble, keep and unpack sources | 12, 15.4 | |
| every command line option | 13 | |
| know what differs from HLASM, what is unverified | 14 | |
| a subroutine, a table search, build variants | 15 | `linkage.za`, `table.za`, `config.za` |
| one program for AMODE 64 and AMODE 31 (`__AMODE__`) | 15.5, 3 | `showargs.za` |
| convert existing HLASM source (columns, registers, `COPY`) | 16 | `hlasm-demo.asm`, `hlasm-demo.za` |
| format `.za` source (columns, indentation of macros) | 20 | `linkage.za`, `cmacros.za` |
| debug on z/OS with dbx or LLDB (`-g`: DWARF in the object; `-gside`: also the `.dbg` file) | 21 | |
| write objects for Linux on Z (ELF, ASCII text) | 17 | `linux-hello.za` |
| let an AI agent assemble, inspect and read this guide (MCP) | 18 | |

---

## 1. Getting started

### 1.1 Building

```sh
make            # builds build/zosasm
make test       # unit tests and command line tests (optional)
```

Any C99 compiler works (`make CC=clang`). Nothing else is needed: no Python,
no generated code at build time.

### 1.2 Your first program

<!-- example: hello.za -->
```za
* A small routine with standard linkage: it saves the caller's registers,
* uses register 12 as its base and adds one to a fullword whose address
* is the first entry of the parameter list (register 1).
HELLO    CSECT
HELLO    AMODE 31
HELLO    RMODE ANY
         STM   14,12,12(13)       save the caller's registers
         LR    12,15              entry point address -> base register
         USING HELLO,12           tell the assembler about it
         L     2,0(,1)            first parameter is the address of a fullword
         L     3,0(,2)            fetch the fullword
         A     3,ONE              add one
         ST    3,0(,2)            store it back
         LM    14,12,12(13)       restore the registers
         SR    15,15              return code 0
         BR    14                 return
ONE      DC    F'1'
         END   HELLO
```

Save it as `hello.za` and assemble it:

```sh
build/zosasm -o hello.o hello.za
```

If the source has no errors, `hello.o` is written. With errors, nothing is
written and each error is printed with the source line (see section 11). The
default output name is the input name with `.o`.

Things to notice in the example:

* The name field starts in column 1; the operation (`CSECT`, `STM`) is
  separated from it by blanks. There are **no column rules**: the operands
  may start anywhere, and a line can be as long as you like.
* The first blank outside quotes ends the operand field; what follows is a
  remark. Lines starting with `*` are comments.
* `USING HELLO,12` does not load register 12; it tells the assembler that
  the program will have done so (the `LR 12,15`). The assembler then turns
  `A 3,ONE` into displacement and base register for you.
* `END HELLO` ends the module and names the entry point.

### 1.3 What did we get?

```sh
build/zosasm -dump hello.o
```

prints the GOFF records (excerpt):

```
HDR arch=1
ESD 0  id=1 parent=0 ... "HELLO"            the section (SD)
ESD 1  id=2 parent=1 off=0 len=36 ... "B_TEXT"   its default class (ED)
ESD 2  id=3 parent=2 ... "HELLO"            the entry label (LD) at offset 0
ESD 1  id=4 parent=1 ... "C_ADATAE9C1"      symbol information (section 12)
TXT esd=2 off=0 len=36 90 EC D0 0C 18 CF 58 20 ...   the machine code
END kind=1 amode=2 esd=2 off=0 name="" count=8
```

and `build/zosasm -disasm hello.o` turns the object back into source (section
12). To get a program you can run, the object still has to go through the
z/OS binder; zosasm does not link.

---

## 2. The source language

### 2.1 Statements

One statement per line:

```
[name]  operation  [operands]  [remark]
```

* `name` starts in column 1. An optional `:` after it is accepted
  (`loop:`). A line that starts with a blank has no name.
* A line starting with `*` or `;` in column 1, or a blank line, is a comment.
* The operand field ends at the first blank that is not inside quotes
  `'...'` or parentheses. The rest of the line is a remark.
* **Continuation.** If the operand field ends with a comma, the next line
  continues it; its leading blanks are skipped.

```
loop:    LA    1,4(,2)            a colon after the label is accepted
         DC    F'1',F'2',         ends with a comma ...
               F'3'               ... so this line continues it
```

* Mnemonics, directives, register names and attribute letters are not case
  sensitive. **Symbols are case sensitive** (`Total` and `TOTAL` differ).
* Source text is UTF-8 (or Latin-1). Character constants are converted to
  EBCDIC IBM-1047 unless you ask for ASCII (`CA'...'`) or UTF-16 (`CU'...'`).

A directive with an *optional* operand (`ORG`, for example) takes the first
word after it as the operand, so a remark there is an error: put the remark
on the line before. `CSECT`, `DSECT` and the like ignore a stray operand
silently, and `-disasm` then shows the first word of the remark as an operand:
also put remarks for them on the line before. For statements that have no operand at all (`LTORG`,
`BR 14`) a remark may follow.

### 2.2 Symbols and quoted symbols

A plain symbol uses the characters `A-Z a-z 0-9 @ # $ _` and does not start
with a digit. There is **no length limit**.

Any other name is written between double quotes. Inside the quotes every
character is allowed; `\"` is a quote, `\\` a backslash and `\xHH` the
character U+00HH (so every EBCDIC byte can be put in an external name).

```
"a.b"     DC    A(*)               a label with a dot
"ext sym" DS    0H                 a name with a blank
          DC    A("a.b","ext sym") used in an expression
```

A quoted symbol is allowed wherever a symbol is: name field, operands,
`ENTRY`, `EXTRN`, `WXTRN`, `V()`, `END`, `L'"x"`. `"ABC"` and `ABC` are the
same symbol. This replaces HLASM's `ALIAS`: the name is used exactly as
written as the name in the object, so `"C++::f"` is an ordinary external
name. The preprocessor does not touch text inside double quotes.

### 2.3 Expressions

An expression is built from:

| term | meaning |
|---|---|
| `123` | decimal number |
| `X'FF'`, `B'101'`, `C'A'` | hexadecimal, binary, character (1 to 4 characters, EBCDIC) |
| `SYMBOL`, `W1.FIELD` | a symbol, or a symbol seen through the labeled USING `W1` (section 7) |
| `*` | the current location |
| `L'SYM`, `T'SYM`, `S'SYM`, `I'SYM`, `D'SYM`, `O'SYM` | attributes of a symbol: length, type, scale, integer, defined, operation code |
| `sizeof(x)`, `offsetof(S,f)`, `alignof(x)`, `countof(x)` | section 9 |

with the operators `+ - * /`, unary `-`, `+` and `!`, parentheses, the
comparisons `== != < <= > >=` and the logical `&&` and `||` (the result is
1 or 0). Precedence, lowest first: `||`, `&&`, comparisons, `+ -`, `* /`.

An expression is *absolute* (a number) or *relocatable* (a number plus the
address of a section). The difference of two symbols of the same section is
absolute; the sum of two relocatable symbols (`A+B`) cannot be relocated and is
rejected (`ZA2025`). For symbols in
different sections or external symbols, only `A`-type constants can hold the
difference (section 10).

Example (`docs/examples/symbols.za`):

<!-- example: symbols.za -->
```za
* Registers, EQU, expressions and quoted symbols.
SYMS     CSECT
R1       EQU   1                  your own names for registers: a bare R1 is
R2       EQU   2                  not predefined, it must be defined like this
BASE     EQU   12
         USING SYMS,BASE
         LGR   R1,R2              names you defined,
         LGR   1,2                numbers and
         LGR   %r1,%R2            %r<n> (any case: %R2) all mean the same
         LA    R1,TABLE+4*2       expression: byte offset 8
         LA    R2,END_TABLE-TABLE the table length (a relocatable difference)
         LHI   %r3,X'FF'+B'101'   hex and binary self-defining terms
         LHI   %r4,C'A'           EBCDIC code of A (193)
TABLE    DC    10F'0'
END_TABLE DS   0F
"a.b"    DC    A(*)               any name can be quoted
"ext sym" DS   0H
         DC    A("a.b","ext sym")
LEN      EQU   L'TABLE            length attribute of TABLE: 4
         LA    1,LEN
         END
```

Note that a bare `R1` is an ordinary symbol, as in HLASM before `REGEQU`: it
is not predefined, and using it without defining it is an error (`ZA1011`,
with a hint). A register is written `%r1` (any case, `%R1`), as a plain number,
or as a name your program defined with `EQU`. And that `L'TABLE` is the length
of one element, 4.

---

## 3. The preprocessor

HLASM's macro language is not part of zosasm; chapter 19 describes its C like
replacement. First, a C style preprocessor runs on the whole file. A `#` in column 1 starts a directive only if it
is followed by one of the keywords below; otherwise it is an ordinary label
(`#COUNT` is a fine symbol).

| directive | meaning |
|---|---|
| `#include "file"`, `#include <file>` | insert a file (with quotes: next to the including file first, then in the `-I` directories; with angle brackets: only the `-I` directories) |
| `#define NAME text`, `#define NAME(a,b) text`, `#undef NAME` | object-like and function-like macros; `##` pastes |
| `#if`, `#ifdef`, `#ifndef`, `#elif`, `#else`, `#endif` | conditional text; `#if` takes integers, `defined(X)`, `! && \|\| == != < > <= >= + - * / % ~` and parentheses |
| `#error text`, `#warning text` | stop or warn |
| `#pragma ...` | listing control: `title`, `eject`, `space`, `print` (section 11.2) |

Rules that differ from C:

* Macros are **not** expanded inside quoted strings or when a name is
  directly followed by `'` (`L'X`, `C'..'`). To put a macro argument into a
  string use the stringify operator `#(x)`, which turns the argument into
  quoted text: with `#define STR(x) C ## #(x)`, `STR(hello)` becomes `C'hello'`
  (`##` pastes the `C` to the quote). It is `#(x)` and not C's `#x` because
  `#` is a symbol character in assembler.
* Only the name, operation and operand fields are expanded. Remarks are not.
* In a `#define` body, `\n` outside quotes starts a new line, so one macro
  can stand for several statements.
* The built-in names cannot be redefined:

| name | value |
|---|---|
| `__FILE__`, `__LINE__` | file name (as opened) and line number |
| `__BASE_FILE__`, `__INCLUDE_LEVEL__` | the file named on the command line; 0 in it, 1 in its includes... |
| `__COUNTER__` | 0, 1, 2 ... one step per use |
| `__ZASM_VERSION__` | `major*10000 + minor*100 + patch` (0.2.0 is `200`) |
| `__GOFF__` or `__ELF__` | defined (as 1) by the object format: `__ELF__` with `-elf`, else `__GOFF__` |
| `__EBCDIC__` or `__ASCII__` | defined (as 1) by the character set (section 17): `__ASCII__` with `-ascii`, `-charset ascii` or `-elf` |
| `__AMODE__`, `__RMODE__` | 0 until an `AMODE` or `RMODE` statement with 24, 31 or 64 is read, then that number (the last one read; `RMODE ANY` counts as 31, `AMODE ANY` and `MIN` change nothing) |
| `__DATE__`, `__TIME__` | `2026-11-15` and `20:21:05` in UTC; see `SOURCE_DATE_EPOCH` below |

`__DATE__` and `__TIME__` use the environment variable `SOURCE_DATE_EPOCH` when
it is set. With `-reproducible` and no `SOURCE_DATE_EPOCH` they are an error,
so a build cannot silently change from one run to the next.

Use `-E` to see the result of the preprocessor. Example:

<!-- example: macros.inc -->
```
#ifndef MACROS_INC
#define MACROS_INC
* macros.inc - shared definitions
#define PAGE      4096
#define SAVE(n)   DC    18F ## #(n)
#define STR(x)    C ## #(x)
#define ZERO(r)   XR    r,r
#endif
```

<!-- example: prep.za -->
```za
* The preprocessor: #include, #define, #if, built-in names and stringify.
#include "macros.inc"
#include "macros.inc"            second include is harmless (guard)
#define DEBUG 1
PREP     CSECT
         USING PREP,12
         ZERO(5)                  expands to  XR 5,5
SAVEAREA SAVE(0)                  expands to  DC 18F'0'
         LHI   1,PAGE             expands to  LHI 1,4096
#if DEBUG && (PAGE > 1000)
         DC    STR(debug build)   expands to  DC C'debug build'
#else
         DC    STR(release build)
#endif
         DC    STR(__FILE__)      the file name as text
LINE     DC    A(__LINE__)        the source line number
N1       DC    A(__COUNTER__)     0, then 1, 2 ... one per use
N2       DC    A(__COUNTER__)
LEVEL    DC    A(__INCLUDE_LEVEL__) 0 in the main file
         END
```

`build/zosasm -E prep.za` shows what the assembler will read:

```
* The preprocessor: #include, #define, #if, built-in names and stringify.
* macros.inc - shared definitions
PREP     CSECT
         USING PREP,12
         XR    5,5                  expands to  XR 5,5
SAVEAREA DC    18F'0'                  expands to  DC 18F'0'
         LHI   1,4096             expands to  LHI 1,4096
         DC    C'debug build'   expands to  DC C'debug build'
         DC    C'prep.za'      the file name as text
LINE     DC    A(16)        the source line number
...
```

(The second `#include` did nothing because of the guard `#ifndef MACROS_INC`.
`__FILE__` shows the path as you typed it on the command line.)

Command line: `-DNAME` or `-DNAME=value` defines a macro (value 1 without
`=`), `-Idir` adds an include directory.

---

## 4. Sections, classes and the object structure

A GOFF object has this shape:

```
module
  section (SD)                  CSECT, RSECT, START, COM
    class (ED)                  B_TEXT unless you choose another with CATTR
      part (PR)                 optional, from CATTR ...,PART(name)
    label (LD)                  ENTRY labels and the section name itself
  external reference (ER)       EXTRN, WXTRN
```

How the source maps to the object:

| source | object |
|---|---|
| `X CSECT`, `X RSECT`, `X START` | section `X` with the class `B_TEXT` and a label `X` at offset 0; `RSECT` marks read-only code |
| `X AMODE 31`, `X RMODE ANY` | addressing and residence mode of the section (`24 31 64 ANY`; AMODE also `MIN`) |
| `ENTRY A,B` | the labels become entry points that other modules can name |
| `EXTRN A`, `WXTRN B` | external references; `WXTRN` is weak: it may stay unresolved |
| `LOCTR` | another location counter of the same class; the counters are joined in the order of first use |
| `NAME CATTR attrs` | start a new class `NAME`; subsequent statements go into it |
| `X XATTR attrs` | attributes of an external symbol, entry label or part |
| `X COM` | common section: no data, the binder takes the largest size |
| `DSECT` | **no object**; a dummy section only gives names and offsets |
| `END [entry]` | end of the module; the operand is the entry point (a label or an external name) |

`CATTR` attributes: `ALIGN(n)` (power of two, 0 to 12), `FILL(n)`,
`PRIORITY(n)`, `RMODE(24|31|64|ANY)`, `EXECUTABLE`, `NOTEXECUTABLE`, `READONLY`,
`NOLOAD`, `DEFLOAD`, `MERGE`, `RENT`, `REUS`, `NONREUS` (also written `NOTREUS`),
`MOVABLE` (no effect on the object), `REMOVABLE`, `PART(name)`. A class made by
`CATTR` has the defaults of IBM's assembler: `ALIGN(3)`, `RMODE(24)`,
`NONREUS`, no AMODE (it does not take the AMODE of the section); `PART`
classes are merged. Two zosasm extensions state "not specified" for objects
of other tools (the disassembler writes them): `RMODE(NONE)` and
`TASKING(NONE)`.

`XATTR` attributes: `LINKAGE(OS|XPLINK)`, `SCOPE(SECTION|MODULE|LIBRARY|EXPORT|IMPORT)`,
`REFERENCE(CODE|DATA[,DIRECT|INDIRECT])`, `PSECT(name)`.

<!-- example: sections.za -->
```za
* Sections, classes, entry points and external references.
MAIN     CSECT
MAIN     AMODE 31
MAIN     RMODE ANY
         ENTRY ALTENT             a second entry point (an LD)
         EXTRN PRINTF             a strong external reference (an ER)
         WXTRN OPTION             a weak one: may stay unresolved
         USING MAIN,12
         L     15,=V(PRINTF)
         BASR  14,15
ALTENT   DS    0H
         BR    14
         LTORG                    literals go here, inside the range of USING
PRINTF   XATTR LINKAGE(XPLINK),SCOPE(LIBRARY) describe the ER

* LOCTR: one section, several location counters, joined in order
DATA     LOCTR
COUNT    DC    F'0'
MAIN     LOCTR                    back to the first counter
         L     1,COUNT

* A second class in the same section, with attributes
D_INFO   CATTR ALIGN(3),NOLOAD
INFO     DS    CL64

* A read-only section and a common section
CONST    RSECT
CONST    AMODE 64
         DC    CL8'CONSTANT'
SHARED   COM
         DS    CL100
         END   MAIN
```

Look at the result with `build/zosasm -dump sections.o`. Notes:

* The `LTORG` in the middle is needed because the literal `=V(PRINTF)` must be
  within reach of the `USING`. Without it, literals are placed at `END` at
  the end of the section you are in at that moment, which in this program
  would be in the class `D_INFO`, out of reach of the base register.
* `DATA LOCTR` and `MAIN LOCTR` are two location counters of one class; the
  code after the second `MAIN LOCTR` continues the first counter, and
  `COUNT` ends up after it in the object.
* `D_INFO CATTR ...` creates a *new class*; `INFO DS CL64` is storage in it.

A `CSECT` may be unnamed. `DSECT` and `LOCTR` need a name, and so do `CATTR`
(the class name) and `XATTR` (the symbol it describes): `ZA3004`, `ZA3006`,
`ZA3007`, `ZA3021`. A statement that generates data or code before any
section has started is an error (`ZA3001`, "no active section").

---

## 5. Data: DC, DS, EQU, ORG, CNOP and literals

### 5.1 DC and DS

```
[name]  DC  [dup] type [ext] [Ln] [Sn] [En] 'value' [,more values]
[name]  DS  [dup] type [ext] [Ln]
```

`dup` is a duplication factor (`4XL2'00'`: four times). `Ln` is the length in
bytes. A name gets the length attribute of the *first* value (`L'name`) and
the type (`T'name`). `DS 0F` generates nothing and aligns; it is the way to
align or to name a place.

| type | constant | default length | notes |
|---|---|---|---|
| `C` | characters | as written | `CA` ASCII, `CE` EBCDIC (default), `CU` UTF-16BE |
| `X` | hexadecimal | as written | digits only |
| `B` | binary | as written | |
| `F`, `H` | fixed point | 4, 2 | decimal, fraction, `E` exponent, `U` prefix for unsigned; `FD`: 8 bytes |
| `A`, `Y` | address | 4, 2 | an expression; relocated when it names a section |
| `V` | external address | 4 | a name that may be external |
| `S`, `SY` | base register and displacement | 2, 3 | `S(address)` or `S(disp(base))`; section 5.1.1 |
| `Q`, `J`, `R` | pseudo register offset, length, PSECT address | 4 | section 8; `QY` is 3 bytes (20 bit long displacement) |
| `G` | graphic (double byte) characters | as written | hexadecimal digits, section 5.1.1 |
| `P`, `Z` | packed, zoned decimal | as written | |
| `E`, `D`, `L` | hexadecimal float short, long, extended | 4, 8, 16 | `EB DB LB` binary, `ED DD LD` decimal, `LQ` quadword aligned |

A bit length `L.n` is also possible (section 5.1.1).

Each constant is aligned to its natural boundary unless it has an explicit
length: `F` to 4, `D` to 8, `H` to 2, and so on.

<!-- example: data.za -->
```za
* Constants, storage and literals.
DATA     CSECT
         USING DATA,12
* integers, addresses, characters
I1       DC    F'-5'              fullword
I2       DC    H'300,-2'          two halfwords
I3       DC    FD'1234567890123'  doubleword
ADDR     DC    A(I1)              4-byte address (relocated)
ADDR3    DC    AL3(I1)            3-byte address
SHORT    DC    Y(I2-I1)           2-byte difference: absolute
TEXT     DC    CL8'Hello'         padded with blanks (EBCDIC)
ATEXT    DC    CA'ASCII'          ASCII text
HEX      DC    X'0102FF'          hexadecimal
BITS     DC    B'10100101'
PACKED   DC    P'-12345'          packed decimal
ZONED    DC    Z'123'
WIDE     DC    CU'Grüße'          UTF-16 big endian
* duplication factor, length modifier, alignment
ARR      DC    4XL2'00'           four times a 2-byte zero
         DS    0D                 align to a doubleword
BUF      DS    CL80               80 bytes, not initialised
         DS    2F                 two fullwords
* scale and exponent modifiers
FIX      DC    FS8'35.92'         35.92 * 2**8, rounded
FLT      DC    EE3'3.414'         3.414E3 in hexadecimal float
* literals: the value is stored in a pool and the address is used
         L     1,=F'100'
         MVC   BUF(8),=CL8'LITERAL'
         LTORG                    the pool is written here
* ORG, EQU, CNOP
         ORG   BUF+4              move the location counter back
MARK     DS    X
* ORG without operand: back to the highest location used so far
         ORG
LEN      EQU   *-DATA             length so far, absolute
FLAG     EQU   X'80',1,C'F'       value, length attribute, type
         CNOP  2,8                align to a halfword in a doubleword
         END
```

Points worth knowing:

* `DC Y(I2-I1)` is an absolute difference (same section) and needs no
  relocation; `DC A(I1)` is relocated by the binder.
* `FS8'35.92'` means 35.92 * 2^8 = 9195.52 rounded to 9196; `EE3'3.414'` is
  3.414 * 10^3 as a hexadecimal float. Both are in section 10.
* `ORG BUF+4` moves the location counter *back*; `ORG` without operand moves
  it to the highest address used so far. Overwriting earlier bytes degrades
  the symbolic information of that section (section 12) but is allowed.
* `EQU` takes up to three operands: value, length attribute and type
  attribute (a number 0 to 255 or a character).
* `CNOP byte,boundary` aligns to `byte` modulo `boundary` (2,8 = a halfword
  position inside a doubleword) with no-op instructions.

#### 5.1.1 Bit lengths, `S`/`SY`, `QY` and `G`

*Bit lengths.* `L.n` gives the length in bits (`FL.12'-1'` is `FFF0`). The
fields of one `DC` follow each other bit by bit (`FL.12'-1,1000'` is
`FFF3E8`), the last byte is filled with zero bits, and the length attribute
is the number of bytes of the first field. `C`, `E`, `D` and `L` constants
fill the field from the left, all other types take the low-order bits of the
value (`XL.12'ABCD'` is `BCD0`). A byte length operand after a bit length
operand starts on a byte boundary. Not allowed: `CU`, `G`, `S`, `V`, `Q`,
`J`, `R`, binary and decimal floating point, and relocatable `A` or `Y`
values (error `ZA2069`, `ZA2070`).

*`S` and `SY`.* The constant holds a base register and a displacement,
4 + 12 bits for `S` (2 bytes) and 4 + 20 bits for `SY` (3 bytes; the
displacement is stored low 12 bits first, then the high 8 bits, as in
instructions). `S(address)` takes both from the active `USING`s exactly like
a storage operand (`S(1024)` with no USING is base 0, displacement 1024);
`S(disp(base))` gives both explicitly (`S(512(12))` is `C200`). Alignment is a
halfword. In a literal, `*` is the address of the literal pool entry (HLASM
uses the place where the literal is used).

*`QY`.* Like `Q`, but 3 bytes, halfword aligned, and the binder stores the
offset as a 20 bit signed long displacement (RLD reference type 9 with a
2 byte + 4 bit field at bit offset 4; not verified against IBM).

*`G`.* Graphic constants hold double-byte characters. zosasm has no DBCS code
page tables, so each character is written as **four hexadecimal digits**
(`G'42C1 42C2'`), with optional blanks and the shift code marks `<` and `>`
around them, which are ignored (the shift codes are not part of the
constant). `GL6'42C1'` pads with the DBCS blank `4040`, `GL2'42C142C2'`
truncates on the right, and the length must be a multiple of 2. `T'name` of a
`G` constant is `@`.

#### 5.1.2 Rules compared with IBM HLASM

These rules were compared with IBM HLASM (`as`, release 6.0) by assembling the
probes in `testdata/oracle/` on z/OS with `tools/oracle.sh` (section 14).

* A length modifier of 0 is an error (`ZA2016`) for every type, in `DC` and
  `DS`. The largest lengths are `C`, `X` 256 (`DS` 65535, `DXD` too), `B` 256,
  `P` and `Z` 16, `F`, `H`, `FD` 8, `A` 4, `AD` 8, `V` 3 or 4, `S` 2, `SY` 3,
  `Y` 2; `E`, `D` and `L` take 1 to 8, 1 to 8 and 1 to 16 bytes (`EH`, `DH`,
  `LH`, `EB`, `DB`, `LB` at least 2; `ED`, `DD`, `LD` exactly 4, 8, 16).
* The modifiers must be in the order `S`, `E`, `L`, ...: `FE2S1'1'` is an error.
* `F` and `H` values outside the signed range are an error (`ZA2023`) unless
  written with the `U` prefix (`F'U4294967295'`). A value of an absolute `A`
  constant is cut to the low bytes without a message (`AL1(256)` is `00`).
  `Y` accepts -32768 to 65535.
* A relocatable `A` or `Y` constant of 2 bytes is a warning (`ZA2086`), of 1
  byte an error.
* `P` allows 31 digits, `Z` 16 (`ZA2028`). When the length modifier is
  shorter than the digits, the leading digits are dropped silently
  (`PL2'12345'` is `345C`).
* Blanks inside the nominal value of all numeric constants are ignored:
  `X'12 34'` is `1234`, `F'1 2'` is `12`.
* An empty nominal value is an error except for `CL2''` (blanks).
* Floating point with a length other than the natural one gives a value with
  the exponent width of the type and 8*n bits, rounded (`EL1` is an error in
  hexadecimal float, "precision lost").
* Instructions: register pairs, lengths, immediate windows (`ZA2081` warns for
  values that HLASM accepts with a warning) and masks follow the table in
  `isa/oracle_rules.json` (`ZA2080` to `ZA2084`).

### 5.2 Literals

`=F'100'` (or any DC operand after `=`) in an instruction operand stores the
constant in a literal pool and uses its address. The pool is written at the
next `LTORG` or, failing that, at `END`. Identical literals are shared; the
pool is doubleword aligned. Like HLASM it is ordered by the **length** of
the literal: lengths that are a multiple of 16 first, then 8, 4, 2 and the
rest, in order of appearance inside each group (`=XL4'1'` is in the group 4,
`=CL3'A'` in the last). A `*` in a literal is the address of the instruction
that uses it (`L 1,=A(*)`), and such a literal is never shared. A literal
 must
be reachable from a `USING`, so put an `LTORG` close to the code that uses it
if the program is longer than 4096 bytes.

---

## 6. Instructions and registers

Practically every z/Architecture instruction of the *Principles of Operation*
(SA22-7832-14) is known, including the extended mnemonics of its Appendix J
(`BE`, `JNE`, ...); a few extended mnemonics such as `NOTR` and `VNOT` are not
generated. Operands follow IBM's notation: `D(X,B)`, `D(B)`, `D(L,B)`, `D(,B)`.

**Registers** can be written in three ways, all equal:

```
         LGR   1,2                numbers
         LGR   R1,R2              names you defined (R1 EQU 1); a bare R1 is NOT predefined
         LGR   %r1,%R2            %r<n>, upper or lower case: %r general, %f floating point,
                                  %a access, %c control, %v vector
```

The `%` form says unambiguously that the operand is a register, and needs no
`EQU` or `REGEQU`-style macro; use it where a number would be unclear
(`LA %r1,0` against `LA 1,0`).

The kind of register is checked: `%f1` where a general register is needed is
an error (`ZA2055`). Register pairs (even-odd) are *not* checked.

### 6.1 Strict registers

With **`-strict-registers`** a register position accepts only the `%` form, or
a name made with `NAME EQU %r<n>` (a *register name*). A plain number, or any
other symbol, is an error (`ZA2067`) that tells you what to write:

```
error[ZA2067]: "14" is not written as a register
  --> hello.za:7:16
  |
7 |          STM   14,12,12(13)       save the caller's registers
  |                ^^
  help: write %r14
```

This catches the classic slips (`LA 1,0` against `L 1,0`, a swapped register
and immediate) because the instruction table knows which operands are
registers. Everything that is not a register stays a number: displacements,
lengths, immediates, masks (`BCR 15,%r14`) and shift counts (`SLL %r3,2`).
A register name can be used only where a register is expected; in an
expression it is an error under `-strict-registers` (`ZA2068`), and the kind
is checked (`FP EQU %f2` cannot be used as a general register, `ZA2055`).
`USING` and `DROP` take registers in the same way.

<!-- example: strict.za -->
```za
* Strict registers: assemble with -strict-registers.
* A register is written %r1 (also %R1; %f0 %a0 %c0 %v0 for the other kinds)
* or is a name made with EQU %r<n>. Numbers stay numbers: displacements,
* lengths, immediates, masks and shift counts.
STRICT   CSECT
BASE     EQU   %r12               register names
WORK     EQU   %r2
FP       EQU   %f2
         STM   %r14,%r12,12(%r13)
         LR    BASE,%r15
         USING STRICT,BASE
         LA    WORK,AREA          base and displacement come from USING
         L     %r3,4(WORK,%r1)    D(index,base)
         SLL   %r3,2              2 is a shift count, not a register
         LHI   %r4,5              5 is an immediate
         BCR   15,%r14            15 is a mask
         LER   FP,%f4             floating point registers have their own letter
         MVC   0(8,WORK),0(%r1)   8 is a length
         DROP  BASE
         LM    %r14,%r12,12(%r13)
         BR    %r14
AREA     DS    CL64
         END
```

Without the option nothing changes: numbers and defined names are accepted,
and register names made with `EQU %r<n>` work as numbers too, so a source
written strictly assembles either way, to the same object. `-strict-registers`
is off by default.

`zosasm -strict-registers -disasm file.o` writes the source in this notation:
the register operands of every instruction, and of `USING` and `DROP`, become
`%r12`, `%f2`... even when the object was assembled from numbers or from
names made with `R12 EQU 12` (names made with `EQU %r12` are kept; masks,
lengths and immediates stay numbers). The text it writes is then assembled
with `-strict-registers` as well to check it; if that fails (for example a
register operand that is an expression) the generic converter is used, which
always writes `%r12`. Without the option `-disasm` gives back the statements
as they were written.

**Addresses.** An operand like `A 3,ONE` (no base given) is turned into
displacement and base register using the active `USING` statements; if no
USING covers the address you get `ZA2030` with a hint. Writing the base
yourself (`A 3,32(,12)`) always works.

**Immediate operands** accept anything that fits the field, signed or
unsigned (`AHI 1,40000` assembles). **Relative branches** (`J`, `BRAS`, `BRCL`)
to a label in the same class are resolved by the assembler; to a label in
another class or to an external symbol they get a relocation.

Instruction names are looked up case-insensitively. A typo gets a suggestion:

```
error[ZA2001]: unknown operation "LGRR"
  --> /tmp/e.za:2:2
  |
2 |  LGRR 1,2
  |  ^^^^ not an instruction or directive
  help: did you mean "LGR"?
```

---

## 7. USING, DSECT and qualified names

A **DSECT** is a template: it defines names and offsets but generates nothing.
Use it to describe a record, a control block or a parameter list. A `USING`
tells the assembler which register points to which address.

```
         USING base,reg1,reg2,...     ordinary: reg1 holds base, reg2 holds base+4096, ...
W1       USING base,reg               labeled: use the fields as W1.NAME
         USING base,address           dependent: base is at address (which another USING covers)
         USING (base,end,lower,upper),reg    with limits
         DROP  reg|label|address ...  end them (DROP alone ends all)
```

When several USINGs cover an address, the smallest non-negative displacement
wins. A name without qualifier uses only ordinary (unlabeled) USINGs;
`W1.FIELD` uses only the USING `W1`. An ordinary USING ends when a later one
uses the same register or at `DROP reg`; a labeled one ends at `DROP label`
or when the label is used again.

<!-- example: using.za -->
```za
* USING: ordinary, labeled, dependent and with a range.
USEDEMO  CSECT
         STM   14,12,12(13)
         LR    12,15
         USING USEDEMO,12         ordinary: register 12 holds USEDEMO

* two records mapped by the same DSECT, each in its own register
         LA    2,REC1
         LA    3,REC2
OLD      USING REC,2              labeled: fields are written OLD.NAME
NEW      USING REC,3
         MVC   NEW.NAME,OLD.NAME  copy the name from one record to the other
         L     0,OLD.COUNT
         A     0,NEW.COUNT
         ST    0,NEW.COUNT
         DROP  OLD,NEW            labeled USINGs end with DROP label

* a header followed by a body: dependent USING needs no register of its own
         LA    4,MSG
         USING HDR,4
         USING BODY,HDR+HDRLEN    the body starts after the header
         CLC   HDR_TYPE,=C'AB'
         MVC   BODY_TEXT,=CL16'payload'
         DROP  4

* a range: the first 256 bytes only
         USING (USEDEMO,USEDEMO+256),12
         LM    14,12,12(13)
         BR    14
         LTORG

REC1     DC    CL10'Alice',F'1'
REC2     DC    CL10'Bob',F'2'
MSG      DC    CL2'AB',CL16' '

* a dummy section: only symbol values
REC      DSECT
NAME     DS    CL10
         DS    CL2
COUNT    DS    F
RECLEN   EQU   *-REC

HDR      DSECT
HDR_TYPE DS    CL2
         DS    H
HDRLEN   EQU   *-HDR              length of the header: 4
BODY     DSECT
BODY_TEXT DS   CL16
         END
```

What the example shows:

* `OLD USING REC,2` and `NEW USING REC,3` map the same DSECT over two
  records. `NEW.NAME` and `OLD.NAME` are the same field in the two views.
* `USING BODY,HDR+HDRLEN` is a *dependent* USING: no register is named. The
  address `HDR+HDRLEN` lies in the area covered by `USING HDR,4`, so the base
  register is 4 and the displacement is added. `DROP 4` ends both.
* `USING (USEDEMO,USEDEMO+256),12` restricts the range to 256 bytes; the
  default is 4096 for the 12-bit displacement.
* `L'HDR`, the length attribute of the *name of a DSECT*, is 1, not the size of
  the DSECT. Use an `EQU *-HDR` (as with `HDRLEN` and `RECLEN`) or `sizeof`
  (section 9) for the size.

`USING 0,3` makes `LA 7,5` assemble as `LA 7,5(,3)`: a USING on an
absolute base 0 gives a register to numbers.

---

## 8. External dummy sections

Reentrant programs (LE, PL/I) need static storage that exists once per
thread. The binder collects it in the *pseudo register area*; each user names
the part it needs. In source:

```
WORK     DXD   CL100,F        external dummy section: 104 bytes, word aligned
         DC    Q(WORK)        offset of WORK in the pseudo register area
         DC    J(WORK)        length of WORK
         CXD                  fullword: total size of all pseudo registers
         DC    R(ENT)         address of the PSECT of an external symbol
```

* `DXD name operands` takes the operands of `DS` and stores nothing. The
  name may be used only in `Q`, `J` and `XATTR`.
* A `DSECT` named in `Q` or `J` becomes an external dummy section too.
* `QD`, `JD`, `RD` and `QL3`... give other sizes; `QY` is the 3 byte long displacement form (section 5.1.1).

<!-- example: xd.za -->
```za
* External dummy sections: pseudo registers that the binder allocates once
* per program and merges by name across modules.
XDDEMO   CSECT
         USING XDDEMO,12
WORK     DXD   CL100,F            100 bytes of workspace, word aligned
STATE    DXD   2F
* the offset of WORK in the pseudo register area, filled by the binder
OFFWORK  DC    Q(WORK)
OFFSTATE DC    Q(STATE)
* the length of WORK
LENWORK  DC    J(WORK)
* total length of all pseudo registers of the program
TOTAL    CXD
* a DSECT used with Q or J becomes an external dummy section too
SAVE     DSECT
SAVEREGS DS    18F
SAVELEN  EQU   *-SAVE
XDDEMO   CSECT
OFFSAVE  DC    Q(SAVE)
* address of the PSECT of an external function
         EXTRN FUNC
         DC    R(FUNC)
         END
```

#### 14.1 Assembler controls

| statement | what zosasm does |
|---|---|
| `ACONTROL AFPR`, `NOAFPR` | `NOAFPR`: only floating-point registers 0, 2, 4 and 6 are accepted (`ZA2071`); `AFPR` (the start state) allows all 16 |
| `ACONTROL TYPECHECK(REGISTER)`, `NOREGISTER`, `NOTYPECHECK` | switch the strict register checks of section 6.1 on and off in the middle of the source |
| `ACONTROL CA(819)`, `CE(1047)` (or `LOCAL`) | accepted; zosasm writes `CA` constants in ISO 8859-1 and `CE` in IBM-1047 only (`ZA2073` for other code pages) |
| `ACONTROL COMPAT(..)`, `FLAG(..)`, `CU(..)`, `ILMA`, `LIBMAC`, `MACHINE(..)`, `OPTABLE(..)`, `RA2` | the selections are checked (`ZA2072` for an unknown one) and have no effect |
| `PUSH`/`POP` `PRINT`, `USING`, `ACONTROL`, `,NOPRINT` | save and restore the listing print state, the active `USING`s and the `ACONTROL` state, each on its own stack; `POP` without `PUSH` is an error (`ZA2075`), a wrong operand `ZA2074` |
| `CEJECT [n]` | listing only (section 11.2) |
| `EXITCTL type,v1[,v2..]` | checked (`ZA2076` for an unknown exit type) and ignored: zosasm has no user exits |
| `name OPSYN op` | `name` becomes another name of the instruction, directive or macro `op`; `name OPSYN` (no operand) deletes `name`; the last definition wins; `ZA2079` for an unknown `op` or a missing name. The macro language has no conditional assembly instructions to rename, so only operation codes of statements count; an alias of a macro is kept by the preprocessor, one of an instruction or directive by the assembler |
| `PUNCH 'text'` | warning `ZA2078`, the text is ignored: a GOFF object has no card image records |
| `REPRO` | warning `ZA2077`; the record that follows is skipped: a GOFF object has no card image records |

**Not verified against IBM.** The records that the binder reads for these
constants (offset, length and R-type relocations) are written as the GOFF
documentation describes them, but the exact combination for the Q constant
differs from one reading of the table and could not be compared with an
object from IBM's assembler. Test it with the binder before relying on it.

---

## 9. Built-in functions and ASSERT

These replace the old `*-LABEL` tricks and the `MNOTE` / conditional assembly
checks. They work at assembly time; nothing is generated for them.

| call | value |
|---|---|
| `sizeof(label)` | bytes of the first operand of its `DC`/`DS` (`L'` times the count): `W DS 4CL10` is 40 |
| `sizeof(section)` | bytes of the first class of a `DSECT` or `CSECT` |
| `sizeof(dxd)` | the length of an external dummy section |
| `offsetof(S,f)` | offset of the field `f` in the section `S` |
| `alignof(x)` | alignment in bytes (a section: the strictest used in it) |
| `countof(label)` | duplication factor times number of values: `4CL10` is 4, `F'1,2,3'` is 3 |

The names are lower case and only a call when followed directly by `(`. The
arguments are symbol names.

**When is a value known?** A label's size, count and alignment are known
right after its statement. The size of a section is known at the end of the
first pass:

* a **DSECT** can be measured at once if it is closed, and the assembler
  checks at the end that it did not grow afterwards (`ZA1044`); so
  `LEN EQU sizeof(S)` and `DS CL(sizeof(S))` work after the DSECT.
* a **CSECT** can be measured in constants and instruction operands (second
  pass), not where the value would decide the size of storage or an `EQU`
  (`ZA1042`).

`ASSERT condition[,'message']` stops the assembly with an error when the
condition is zero. It is checked in the second pass, so sizes and forward
references work:

<!-- example: builtins.za -->
```za
* sizeof, offsetof, alignof, countof and ASSERT: sizes without *-LABEL tricks.
BUILTIN  CSECT
         USING BUILTIN,12

PARM     DSECT
P_FLAGS  DS    X
P_NAME   DS    CL15
P_TABLE  DS    4F
P_END    DS    0D
PARMSIZE EQU   sizeof(PARM)       a closed DSECT may be measured at once (32)

BUILTIN  CSECT
         LA    1,sizeof(PARM)     in an operand
         LHI   2,offsetof(PARM,P_TABLE) 16
         LHI   3,countof(P_TABLE)       4
         LHI   4,alignof(PARM)    8
         LHI   5,sizeof(P_TABLE)>=16    comparisons give 1 or 0

AREA     DS    CL(sizeof(PARM))   storage as big as the DSECT
MAP      DC    A(sizeof(BUILTIN)) a CSECT's own size, known in pass 2

* assembly time checks
         ASSERT sizeof(PARM)<=256,'PARM must fit in 256 bytes'
         ASSERT offsetof(PARM,P_TABLE)==16 && alignof(PARM)==8,'layout changed'
         END
```

```
error[ZA1052]: assertion failed: layout changed
  help: the condition was: offsetof(PARM,P_TABLE)==16 && alignof(PARM)==8
```

The message is a quoted text with `''` for a quote; without a message the
condition text is used.

---

## 10. Floating point, UTF-16, CCW and address differences

<!-- example: floats.za -->
```za
* Floating point, UTF-16, channel command words and address differences.
FLOATS   CSECT
         USING FLOATS,12
* hexadecimal float (the default), binary (IEEE) and decimal float
HX       DC    E'1.5',D'-0.1',L'3.14159265358979323846'
BI       DC    EB'1.5',DB'0.1',LB'1'
DEC      DC    ED'1.5',DD'0.1',LD'1'
* a rounding mode after the value, and special values
RND      DC    EB'0.1R5',EB'0.1R4' toward zero, nearest even
SPEC     DC    DB'(INF)',DB'-(INF)',DB'(QNAN)',EB'(MAX)'
* scale and exponent modifiers apply to fixed point and hexadecimal float
SC       DC    FS8'35.92',HS6'-25.46',EE3'3.414'
* UTF-16 text
U16      DC    CU'Grüße 😀'
* channel command word: command, data address, flags, count
         CNOP  0,8
CCW1     CCW   X'02',BUFF,X'40',80
BUFF     DS    CL80
* a difference of two relocatable symbols needs the binder: RLD SUB
         EXTRN FIRST,SECOND
DIFF     DC    A(SECOND-FIRST)
OFF      DC    A(FIRST-*)
         END
```

**Floating point.** The type letter `E`, `D`, `L` is the length (short 4,
long 8, extended 16 bytes), the extension letter the format:

| write | format |
|---|---|
| `E'1.5'`, `EH'1.5'` | IBM hexadecimal float |
| `EB'1.5'` | IEEE 754 binary |
| `ED'1.5'` | IEEE 754-2008 decimal (DPD encoding) |
| `LQ'1'` | hexadecimal extended, aligned to a quadword |

After the value, `R` and a number select the *rounding mode*:

| format | modes |
|---|---|
| hexadecimal, binary | `R1` nearest, ties away (default for hexadecimal); `R4` nearest even (default for binary); `R5` toward zero; `R6` toward +infinity; `R7` toward -infinity |
| decimal | `R8` nearest even (default); `R9` toward zero; `R10` up; `R11` down; `R12` nearest ties away; `R13` nearest ties toward zero; `R14` away from zero; `R15` round for re-round |

Special values, for binary and decimal: `(INF)`, `-(INF)`, `(QNAN)`, `(SNAN)`,
`(NAN)`, `(MAX)`, `(MIN)`, `(DMIN)`. A value too large or too small is a
warning (`ZA2066`) with the result the rounding mode gives; for hexadecimal
float an overflow is an error.

**Scale and exponent.** `Sn` on a fixed point constant multiplies the value
by 2^n and rounds half away from zero; on a hexadecimal float it shifts the
fraction right by n hexadecimal digits (the same number, unnormalized). `En`
multiplies by 10^n before the conversion. `FS8'35.92'` is 9196 (35.92 * 256 =
9195.52, rounded) and `EE3'3.414'` is the float 3414.0. On binary floating
point `S` is ignored with a warning; on decimal float it is an error.

**CU.** `CU'text'` is UTF-16 big endian; characters above U+FFFF become
surrogate pairs. The length modifier counts bytes (even); shorter text is
padded with U+0020.

**CCW, CCW0, CCW1.** `CCW cmd,address,flags,count` generates an 8-byte
channel command word (doubleword aligned). `CCW` and `CCW0` use format 0
(24-bit address, relocated like `AL3`), `CCW1` format 1 (31-bit address,
relocated like `A`).

**Address differences.** `DC A(SECOND-FIRST)` where the two symbols are in
different sections or external needs the binder to compute the number: the
assembler writes two relocation items on the same field, one that adds and
one that subtracts. A single subtraction `A(-X)` works too, and up to five
terms with coefficient +1 or -1 are allowed. A difference of two labels of the
same section is a plain number with no relocation at all.

**Not verified against IBM.** All of this is implemented from the HLASM
Language Reference and checked against its examples and against published IEEE
bit patterns, not against IBM's assembler. In particular the encoding of the
subtract relocations should be verified with the binder.

---

## 11. Diagnostics and the listing

### 11.1 Messages

Every message has a severity, a **stable code**, the source position and
usually a help line:

```
error[ZA1011]: undefined symbol "UNDEF"
  --> /tmp/e.za:3:7
  |
3 |  LA 1,UNDEF
  |       ^^^^^
```

* `error` stops the object from being written; `warning` does not (unless
  `-Werror`). The assembler goes on after an error to find more, up to
  `-max-errors N` (default 50, 0 = no limit).
* Lines produced by a preprocessor macro get a note that says which macro
  and where it was defined.
* `-color` colours the output.
* `-diag json` writes all messages as **one JSON document** on standard error
  instead, for tools:
  `{"diagnostics":[{"severity":"error","code":"ZA1011","message":"...",`
  `"file":"e.za","line":2,"column":7,"length":5,"label":"...",`
  `"notes":[{"file":...,"line":...,"column":...,"length":...,"message":"..."}],`
  `"help":["..."]},...],"errors":1,"warnings":0}`. `severity` is `note`,
  `warning`, `error` or `fatal`; `label` is left out when there is none;
  line and column are 1-based (column counts bytes); the messages are sorted
  by file, line and column. The document is written also when there is
  nothing to report (`"diagnostics":[]`).

Code ranges:

| codes | area |
|---|---|
| `ZA0xxx` | syntax of lines, strings, quoted symbols, attributes |
| `ZA1xxx` | symbols and expressions; `ZA104x` sizes; `ZA105x` ASSERT |
| `ZA2xxx` | instructions, operands, DC/DS and constants; `ZA206x` XD and CCW |
| `ZA3xxx` | sections, GOFF directives, USING and DWCFI (`ZA306x`) |
| `ZA4xxx` | the preprocessor; `ZA405x` and `ZA41xx` the macro language (chapter 19) |
| `ZA9xxx` | not supported yet / internal |

Codes do not change from one version to the next, so you can search for them
and use them in scripts.

### 11.2 The listing: `-l`

`-l FILE` writes an assembly listing to FILE (`-l -` is standard output). It
is written also when the assembly has errors; then it is the best way to see
them in context. Options: `-ldata` (all object bytes), `-lpage N` (pages of N
lines), `-lbrief` (no tables at the end). The listing has no date or time, so
the same source gives the same listing.

<!-- listing: hello -->
```
zosasm 0.2.0  docs/examples/hello.za

Loc     Object               Line   Source
                                1   * A small routine with standard linkage: it saves the caller's registers,
                                2   * uses register 12 as its base and adds one to a fullword whose address
                                3   * is the first entry of the parameter list (register 1).
                                4   HELLO    CSECT
                                5   HELLO    AMODE 31
                                6   HELLO    RMODE ANY
000000  90ECD00C                7            STM   14,12,12(13)       save the caller's registers
000004  18CF                    8            LR    12,15              entry point address -> base register
                                9            USING HELLO,12           tell the assembler about it
000006  58201000               10            L     2,0(,1)            first parameter is the address of a fullword
00000A  58302000               11            L     3,0(,2)            fetch the fullword
00000E  5A30C020               12            A     3,ONE              add one
000012  50302000               13            ST    3,0(,2)            store it back
000016  98ECD00C               14            LM    14,12,12(13)       restore the registers
00001A  1BFF                   15            SR    15,15              return code 0
00001C  07FE                   16            BR    14                 return
000020  00000001               17   ONE      DC    F'1'
                               18            END   HELLO

18 source lines, 15 statements; 0 errors, 0 warnings
```

**Columns.** `Loc` is the offset in the class (the same number the symbol
table and a debugger use; 6 hex digits, 8 when a class is longer than 16 MB),
`Object` the bytes of the statement in groups of 4, then the line number and
the source. Notes:

* Alignment bytes are not shown: the location of `ONE` above is 20, the two
  bytes before it are padding. Relocatable fields (`A(X)`, `V(X)`, a branch
  target in a different section) show zeros, because the value is only known
  to the binder or loader.
* Only the first 8 bytes are shown, and `+` marks that there are more. With
  `-ldata` (or `PRINT DATA`) the rest follows on further rows.
* `DS`, `ORG` and dummy sections show a location but no bytes. `EQU` shows its
  value in the `Loc` column when it is a plain number.
* The literal pool of `LTORG` (or of `END`) is listed under that statement,
  one row per literal with the literal as written as the source.
* A line number with a character before it (`1` or `A`...) is a line of an
  included file; the table `Source files` explains the characters.

**Marks** before the source text:

| mark | meaning |
|---|---|
| (blank) | an ordinary source line |
| `#` | a preprocessor directive (`#include`, `#define`, `#if`, `#pragma`...) |
| `-` | a line skipped by `#if`/`#ifdef`/`#else`, or by an `#include` guard |
| `+` | the result of preprocessor expansion: the line as written is listed first, then each line it produced, with the object bytes |

**Diagnostics** are printed after the line they belong to, as `** CODE
severity: message` with the notes and help lines, so a listing shows what went
wrong at that line, with carets (`^^^^`) under the source text first.
Messages without a line (for example "no control section") come after the last
line. (No carets for lines that macro expansion produced: their columns
belong to the expanded text, which the text format of the messages shows.)

**Print control.** `TITLE 'text'` sets the second line of the page header (and
starts a new page with `-lpage`), `EJECT` starts a new page, `SPACE n` leaves
n blank lines, and `PRINT` takes a list of `ON`, `OFF`, `GEN`, `NOGEN`,
`DATA`, `NODATA`:

| operand | effect |
|---|---|
| `OFF`, `ON` | stop and resume the rows (a line with a diagnostic is shown anyway) |
| `NOGEN`, `GEN` | hide or show the `+` rows of preprocessor expansion |
| `DATA`, `NODATA` | all object bytes, or back to what `-ldata` says |

`CEJECT n` starts a new page when fewer than n lines are left on the page
(no operand: like `EJECT`; 0: unless at the top; the number must be written
in decimal), and is not listed itself. `PUSH PRINT` saves the print state
(`PRINT` operands and `DATA`) and `POP PRINT` restores it; with `,NOPRINT` the
`PUSH` or `POP` statement is not listed.

The same controls exist as preprocessor lines, which can be used inside `#if`: `#pragma title "text"`, `#pragma eject`, `#pragma ceject 10`, `#pragma
space 2`, `#pragma print off` (also `on`, `data`, `nodata`, `gen`, `nogen`).
Without `-lpage` there are no pages, and the header is written once.

**Tables** after the source (not with `-lbrief`): the source files, the
sections with their classes, lengths and modes, the USING table (line, label,
register, base, range, and the line that dropped it), and the symbol cross
reference: for every symbol its kind, length, value, defining line and all the
lines that use it. For a file other than the main one a reference is
written `1:12`-style with the file character in front.

<!-- listing: tables -->
```
Source files
Id  File
    docs/examples/using.za  (main)

Sections
Section  Kind    Class       Length  AMODE  RMODE
USEDEMO  CSECT   B_TEXT      00007A  -      -
REC      DSECT   B_TEXT      000010  -      -
HDR      DSECT   B_TEXT      000004  -      -
BODY     DSECT   B_TEXT      000010  -      -

USING table
Line      Label       Reg  Base                    Offset    Range              Dropped
5                     r12  USEDEMO                 0                            27
10        OLD         r2   REC                     0                            16
11        NEW         r3   REC                     0                            16
20                    r4   HDR                     0                            24
21                    r4   BODY                    0         (dependent)        24
27                    r12  USEDEMO                 0

Symbol cross reference
Symbol                    Kind     Length  Value     Section    Defined  References
BODY                      section  0                 BODY       47       21
BODY_TEXT                 label    16      0         BODY       48       23
COUNT                     label    4       C         REC        40       13 14 15
HDR                       section  0                 HDR        43       20 21
HDRLEN                    equ      1       4                    46       21
HDR_TYPE                  label    2       0         HDR        44       22
MSG                       label    2       68        USEDEMO    34       19
NAME                      label    10      0         REC        38       12
REC                       section  0                 REC        37       10 11
REC1                      label    10      4A        USEDEMO    32       8
REC2                      label    10      58        USEDEMO    33       9
RECLEN                    equ      1       10                   41
USEDEMO                   section  0                 USEDEMO    2        5 27

49 source lines, 38 statements; 0 errors, 0 warnings
```

The cross reference counts uses in operands, in `USING`, `ENTRY`, `END` and
`XATTR`; a use inside a literal (`=A(X)`) is not counted.

The location column shows class offsets, so two `CSECT`s may show the same
numbers. This is not HLASM's listing format (no statement numbers, no ESD ids,
no relocation dictionary), and no program should read it.

---

## 12. Looking inside an object

### 12.1 `-dump`

`zosasm -dump file.o` prints the GOFF records: `ESD` items (section, class,
part, label, external reference), `TXT` (the bytes), `RLD` (relocations),
`END`. After that, if the object has it, the symbolic information (below).

### 12.2 ZSYM: symbolic information in the object

The GOFF records contain addresses and bytes, not names, EQUs, DSECTs or the
operands you wrote. By default zosasm adds them in a *class that is not
loaded* (`C_ADATAE9C1`, `NOLOAD`), so the program is byte for byte the same
whether or not it is there. The class holds, per statement, its file and
line, where it put its bytes, a checksum of them and the operands as written,
with symbols as references (so a symbol name is stored once).

| option | content |
|---|---|
| `-gz0` | nothing: only code, data and relocations |
| `-gz1` (default) | symbols and statements |
| `-gz2` | also the original source files (below) |

The block is protected by checksums; a reader never trusts a damaged block.

### 12.3 `-disasm`: object to source

```sh
zosasm -disasm file.o [-o out.za]
```

For an object with ZSYM you get your own statements back: names, `EQU`s,
`USING`s, `DSECT`s and literals (`=F'5'`), but no comments (the original text
is in `-gz2`). With `hello.o`:

```
HELLO    CSECT
HELLO    AMODE 31
HELLO    RMODE ANY
         STM   14,12,12(13)
         LR    12,15
         USING HELLO,12
         L     2,0(,1)
         ...
ONE      DC    F'1'
         END   HELLO
```

Objects without ZSYM, objects from other producers, and objects that were
changed after assembling (`-generic` forces this form) are converted
**generically**: the instructions are decoded, labels are generated
(`L00044`), data becomes `DC X'...'` or `DC C'...'`. The same object, `-generic`:

```
HELLO    CSECT
         AMODE 31
         RMODE 31
         STM   %r14,%r12,12(%r13)
         LR    %r12,%r15
         L     %r2,0(%r0,%r1)
         ...
         DC    X'000000000001'
         END   HELLO
```

After converting, zosasm assembles the text again and compares it with the
object; if the source does not give the same object, it says so (and with
ZSYM it first falls back to the generic text). A section whose bytes were
patched after assembling is written generically by itself, the rest of the
module still gets its names.

### 12.4 `-gz2` and `-unpack`: the original sources

With `-gz2` the object also carries the files the assembler read, byte for
byte, in a second not-loaded class (`C_ADATAE9C2`): the main file, every
`#include`, and the `-D` options. They are compressed (LZ4 block format) when
that makes them smaller.

```sh
zosasm -gz2 -DDEBUG=1 -o prog.o prog.za
zosasm -dump prog.o                 # lists the files with sizes and checksums
zosasm -unpack prog.o -o src        # writes them below ./src, same relative names
```

The files are written with their relative names (a hostile object cannot
write outside the directory: `/` and `..` components are neutralised), and
assembling the main file there with the same options gives the same object.
Several modules bound together keep their sources in separate blocks; the
later blocks are unpacked into `src/2/`, `src/3/` and so on.

### 12.5 Reproducible builds

`-reproducible` makes the assembly fail on anything that could change between
two runs (today: `__DATE__` and `__TIME__` without `SOURCE_DATE_EPOCH`).

---

## 13. Command reference

```
zosasm [options] source.za
zosasm -dump file.o
zosasm [-generic] [-o out.za] -disasm file.o
zosasm [-o dir] -unpack file.o
hlasm2za [options] input.asm        (section 16)
zafmt [options] [file ...]          (chapter 20)
```

| option | meaning |
|---|---|
| `-o FILE` | output file (default: input with `.o`; for `-disasm` stdout; for `-unpack` the directory, default `.`) |
| `-DNAME[=VAL]` | define a preprocessor macro (value 1 if omitted) |
| `-Idir` | add an include directory |
| `-Mdir` | add a directory of library macros (`name.zmac`, `name.mac`, also `NAME.zmac`; chapter 19) |
| `-macro-steps N` | stop a macro expansion after N statements (default 1000000) |
| `-sysparm STR` | the text of `sys.parm` in macros (HLASM `&SYSPARM`) |
| `-E` | print the preprocessed source and stop |
| `-elf` | write a relocatable ELF64 object for Linux on Z instead of GOFF; implies `-ascii`, ignores `-gz` (section 17) |
| `-ascii`, `-charset ascii\|ebcdic` | character set of `C'..'` constants and of the type attribute (default `ebcdic`; `ascii` with `-elf`) |
| `-gz0`, `-gz1`, `-gz2` | symbolic information level (default `-gz1`) |
| `-g` | put the DWARF for dbx and LLDB into the object (class `C_ADATAE9C3`, NOLOAD) and append the debug link module (chapter 21) |
| `-gside` | also write `FILE.dbg` with the same DWARF (for dbx); implies `-g` |
| `-gdwarf FILE` | write the `.dbg` file to FILE (default: the object name with `.dbg`); implies `-gside` |
| `-gdir DIR` | the z/OS directory that corresponds to the current directory; implies `-g` |
| `-gmap FROM=TO` | a path prefix of this machine and its z/OS counterpart, repeatable; implies `-g` |
| `-gdbgpath PATH` | where the `.dbg` will be on z/OS, at most 200 bytes; implies `-g` |
| `-generic` | with `-disasm`: ignore ZSYM |
| `-reproducible` | fail on anything that makes the object differ between runs |
| `-strict-registers` | registers only as `%r1` or an `EQU %r1` name (section 6.1); with `-disasm` the source is written that way |
| `-l FILE` | write a listing to FILE (`-` = standard output), also after errors (section 11.2) |
| `-` (as the source) | read the program from standard input (as `hlasm2za` and `zafmt` do) |
| `-ldata` | listing: all object bytes of a statement |
| `-lpage N` | listing: N lines per page (default 0: one page) |
| `-lbrief` | listing: no tables |
| `-version` | print the version |
| `-diag text\|json` | diagnostics as text (default) or as one JSON document (section 11.1) |
| `-color` | colour the diagnostics |
| `-max-errors N` | stop after N errors (default 50; 0 = no limit) |
| `-Werror` | warnings are errors |
| `-h`, `-help` | usage |

Options can also be written with two dashes (`--dump`). The exit status is 0
on success, 1 when assembling failed or a file could not be read or written,
2 for a bad command line. Source files use the extension `.za`.

Environment: `SOURCE_DATE_EPOCH` (seconds since 1970) fixes `__DATE__` and
`__TIME__`.

---

## 14. Differences from HLASM and known limits

**Different on purpose.**

| HLASM | zosasm |
|---|---|
| card columns, continuation column | none; a trailing comma continues |
| names fold to upper case | symbols are case sensitive |
| macros, `AIF`, `SETC`, `COPY` | C style preprocessor and the macro language of chapter 19; `COPY` is an error that says `#include` |
| `ALIAS`, 63 character names | quoted symbols, no limit |
| `MNOTE` | `ASSERT`, `#error`, `#warning` |
| `*PROCESS` | `#pragma`, options on the command line |
| `ACONTROL`, `PUSH`, `POP`, `CEJECT`, `EXITCTL`, `REPRO` | only the useful parts (section 14.1) |
| `*-LABEL` length tricks | `sizeof`, `offsetof`, `alignof`, `countof` |
| listing | `-l` (section 11.2), a different layout; `TITLE`, `EJECT`, `SPACE`, `PRINT` work in it; `PUNCH`, `ICTL`, `ISEQ` are accepted and ignored |
| object deck / OBJ output | GOFF only |

`hlasm2za` (section 16) converts HLASM source to the zosasm form: it cuts the
card columns, folds names to upper case, writes registers as `%r1`, and
converts `MACRO`..`MEND` definitions to zosasm macros (section 16.4), and
converts conditional assembly in open code (section 16.5), and reports what it
cannot convert (macro calls of system macros that are not in a library, a few
macro statements).

**Not implemented.** `ADATA`, `MNOTE`; `G`
constants with real DBCS text (hexadecimal only); `CU` code page options
(UTF-16LE/UTF-8); other code pages for `CA`/`CE`.

**Limits.** A section cannot hold more than 2147483647 bytes (2 GB - 1):
GOFF stores class lengths and text offsets in 32 bits. A `DS`/`DC` that
would need more storage is an error (`ZA2087`), an `ORG` beyond the limit is
an error (`ZA3053`), and a section grown past the limit by several
statements is an error too (`ZA3054`) - the assembly says so instead of
running out of memory.

**Verified against IBM.** An oracle run (2026-11) on z/OS with HLASM 6.0 and
the binder compared: the operand rules of all 483 instructions that have
register pairs, length, immediate or mask limits (`isa/oracle_rules.json`),
the DC/DS rules of section 5.1.2 (bytes and errors), hexadecimal and binary
floating point constants of any length, and the GOFF flags of sections, entry
points and A-constants to local labels and external symbols (also linked and
run). `tools/oracle.sh testdata/oracle/X.asm` repeats such a run (it needs
`ssh` access to a z/OS host with `as`; see the script header) and
`tools/oracle_cmp.py` compares the listing. Known differences that remain:
HLASM does the arithmetic of expressions in 32 bits (zosasm in 64),
HLASM writes some extra empty classes (`B_IDRL`, an empty `B_PRV`) that zosasm
does not, and HLASM writes the END record count as 0 and the architecture
level of the HDR as 0 (zosasm writes the real count and level 1).

A second oracle run compared the objects themselves (`tools/oracle_obj.sh`:
`as -mgoff`, fetch the object, `zosasm -dump` of both, `tools/oracle_obj.py`
compares ESD, text and RLD items structurally). What it changed: the CATTR
defaults above, the padding of `CNOP` (`0700`, `47000700`), no common flag and
no text for a `COM` class, `Q`/`QY` constants refer to the class of the pseudo
register, `B_PRV` and its pseudo registers have the attributes of HLASM, a
pseudo register (`DXD`, or a DSECT used in `Q`/`J`) belongs to the section
started last (not the one resumed), `R(label)` of a local entry refers to an
ER of the same name, `RMODE 31|ANY` alone gives `AMODE 31`, `RMODE 64` gives
`AMODE 64`, `AMODE 64` gives `RMODE 31`, a `CSECT` statement resumes the last
location counter of `B_TEXT`, and the location counters (`LOCTR`) of a class
follow each other without padding, their items aligning to the final address
(the assembly is repeated until the piece starts settle). Every `EXTRN` and
`WXTRN` makes an external reference (ER), even when it is never used, and its
parent is the section current at the statement (also when only an earlier
section uses it). `tools/oracle_amb.sh NAME` runs the z/OS `amblist` on both
objects as a second, independent reader; apart from the extra `B_IDRL` class,
the IDRL record (which shifts the ESD ids), the END count and the HDR level,
the decoded listings agree.

**Not verified against IBM.** The
following encodings were made from the documentation and from tests that
check the bytes by hand, but were not compared with IBM's assembler or run
through the binder: the GOFF records for `Q`, `J`, `R` and `CXD`; the
subtract relocation of `A(X-Y)`; floating point values in decimal float, the
rounding modes and the special values; the `CCW` family; the exact attributes
of the ZSYM classes. The test suite round trips everything through zosasm's
own reader. Expect small differences when the first object is checked on
z/OS; they are all in one place each (`reloc_xd` in `emit.c`, `float.c`,
`dc.c`).

**Where to read more.** `DESIGN.md` (decisions, GOFF mapping, ZSYM format,
disassembler), `ZSYM.md` (the symbolic information in detail),
`HLASM-REVIEW.md` (every HLASM feature and what was decided), `PLAN.md`
(status), and `tests/` (small programs that show every feature).

---

## 15. Recipes

Complete programs for common jobs. Like the others, each one is a file in
`docs/examples/` that the test suite assembles with `-Werror`.

### 15.1 A subroutine with standard linkage

The usual shape of an OS linkage routine: save the caller's registers, chain
the save areas, address the parameter list through a DSECT, call another
routine and return its return code in register 15.

<!-- example: linkage.za -->
```za
* A complete subroutine: standard save area chain, a parameter list mapped
* by a DSECT, a call to another routine and the return code.
*   parameter 1: address of the input fullword
*   parameter 2: address of the output fullword (receives input * 2)
DOUBLE   CSECT
DOUBLE   AMODE 31
DOUBLE   RMODE ANY
         EXTRN CHECK              a routine in another module
         STM   14,12,12(13)       caller's registers into the caller's area
         LR    12,15
         USING DOUBLE,12
         ST    13,SAVE+4          back chain: our area -> caller's area
         LA    2,SAVE
         ST    2,8(,13)           forward chain: caller's area -> ours
         LR    13,2

         LR    10,1               keep the parameter list address
         USING PL,10
         L     3,PL_IN            address of the input
         L     4,0(,3)            the input value
         LR    5,4
         AR    5,4                times two
         L     6,PL_OUT
         ST    5,0(,6)            store it

         LA    1,PL_IN            pass the same list on to CHECK
         L     15,=V(CHECK)
         BASR  14,15              R15 = its return code
         DROP  10

         L     13,SAVE+4          unchain
         L     14,12(,13)
         LM    0,12,20(13)        R15 stays: it is our return code
         BR    14
SAVE     DC    18F'0'             not reentrant: a static save area
         LTORG

* the parameter list
PL       DSECT
PL_IN    DS    A
PL_OUT   DS    A
PL_LEN   EQU   *-PL
         ASSERT PL_LEN==8,'parameter list changed'
         END   DOUBLE
```

Points to notice: `PL_IN` and `PL_OUT` are plain names because `USING PL,10`
is active; `LA 1,PL_IN` therefore means "the address 0(,10)", which is the
list itself. The `ASSERT` at the end stops the build if somebody changes the
list without changing the callers. `=V(CHECK)` makes a literal that holds an
external address; the `LTORG` puts it inside the range of register 12.

### 15.2 A table of records

A DSECT describes one entry, the table is data, and the number of entries is
computed by the assembler instead of being counted by hand.

<!-- example: table.za -->
```za
* A table of records mapped by a DSECT, searched in a loop.
* On return R2 points to the matching entry, or R2 = 0 when there is none.
* one entry
ENT      DSECT
E_KEY    DS    CL4
E_VALUE  DS    F
ENTLEN   EQU   *-ENT

FIND     CSECT
         USING FIND,12
         USING ENT,2              R2 addresses one entry
         LA    2,TAB              first entry
         LHI   3,NENT             number of entries
LOOP     CLC   E_KEY,=C'BETA'     the key to look for
         BE    DONE
         LA    2,ENTLEN(,2)       next entry
         BCT   3,LOOP
         XR    2,2                not found
DONE     BR    14
         LTORG

TAB      DC    CL4'ALPH',F'1'
         DC    CL4'BETA',F'2'
         DC    CL4'GAMM',F'3'
TABEND   DS    0F
NENT     EQU   (TABEND-TAB)/ENTLEN
         ASSERT NENT==3
         ASSERT NENT*ENTLEN==TABEND-TAB,'table is not a whole number of entries'
         END
```

`TABEND-TAB` is a difference of two labels in the same section, so it is
absolute and can be divided; add a row to the table and `NENT` follows. There
is no `%` (remainder) operator (section 2.3), so the second `ASSERT` checks the
division by multiplying back.

### 15.3 One source, several builds

The preprocessor and `-D` replace conditional assembly. Here `LEVEL` decides
which instructions are assembled and `#error` rejects a wrong value.

<!-- example: config.za -->
```za
* One source, several builds: -DLEVEL=n chooses what is assembled.
* The default is level 1.
#ifndef LEVEL
#define LEVEL 1
#endif
#if LEVEL < 0 || LEVEL > 2
#error LEVEL must be 0, 1 or 2
#endif
CONFIG   CSECT
         USING CONFIG,12
         L     1,=A(LEVEL)
#if LEVEL >= 1
         L     2,COUNTER          counting is on from level 1
         AHI   2,1
         ST    2,COUNTER
#endif
#if LEVEL >= 2
         CLC   COUNTER,=F'1000'   a sanity check at level 2
         BNH   OK
         DC    H'0'               a zero halfword is an invalid instruction: abend
OK       DS    0H
#endif
         BR    14
COUNTER  DC    F'0'
         LTORG
         END
```

```sh
zosasm -DLEVEL=0 -o lean.o config.za      # 20 bytes of code
zosasm -DLEVEL=2 -o checked.o config.za   # 48 bytes of code
zosasm -DLEVEL=3 config.za                # error: LEVEL must be 0, 1 or 2
zosasm -E -DLEVEL=2 config.za             # show what the preprocessor made
```

With `-gz2` (section 12.4) the `-D` options are stored in the object together
with the source, so the object says how it was built.

### 15.4 From source to object and back

Everything the tool does, in the order you would use it:

```sh
zosasm -gz2 -Werror -DLEVEL=2 -o config.o config.za   # assemble, keep the sources
zosasm -dump config.o                                  # records, symbols, sources
zosasm -disasm config.o                                # your statements, comments gone
zosasm -unpack config.o -o src                         # the original files again
cd src
zosasm -gz2 -Werror -DLEVEL=2 -o ../again.o config.za  # same options, same name
cmp ../config.o ../again.o                             # no output: identical
```

The object records the file name as it was given, so assemble the unpacked
file under the same relative name (here: from inside `src`); then the object
is byte for byte the same. Add `-reproducible` when the source uses
`__DATE__` or `__TIME__`.

### 15.5 One program for AMODE 64 and AMODE 31

`__AMODE__` and `__RMODE__` (section 3) hold the value of the last `AMODE` or
`RMODE` statement read: 0 before any, then 24, 31 or 64. The `AMODE` statement
itself can be chosen with `-D`, and the code that depends on the mode follows
`__AMODE__`, so the two cannot disagree. This z/OS program prints its
arguments, one per line, after a banner line with the mode it was built for:

<!-- example: showargs.za -->
```za
* Print all argv items cleanly
*
* Built twice:  zosasm -DTEST31=0   AMODE 64 (the default)
*               zosasm -DTEST31=1   AMODE 31
* The AMODE statement is chosen by TEST31; everything that depends on the
* addressing mode is chosen by the built-in __AMODE__, which holds the value
* of the AMODE statement read so far (0 before any AMODE statement).
#define STR(x) C ## #(x)
HELLOW   CSECT
#if defined(TEST31) && TEST31
HELLOW   AMODE 31
#else
HELLOW   AMODE 64
#endif
HELLOW   RMODE ANY
#if __AMODE__ == 64
         STMG  %r14,%r12,16(%r13)
#elif __AMODE__ == 31
         STM   %r14,%r12,12(%r13)
#else
#error "AMODE must be 31 or 64 before the code"
#endif
         LARL  %r12,HELLOW
         USING HELLOW,%r12

         STG   %r1,SAVED_R1
         LARL  %r2,SAVEAREA
         STG   %r13,128(,%r2)
         STG   %r2,136(,%r13)
         LGR   %r13,%r2

* Locate BPX4WRT
         LHI   %r0,544
         A     %r0,16
         LLGTR %r1,%r0
         LHI   %r0,24
         A     %r0,0(,%r1)
         LLGTR %r1,%r0
         LHI   %r0,220
         A     %r0,0(,%r1)
         LLGTR %r1,%r0
         L     %r0,0(,%r1)
         LLGTR %r0,%r0
         STG   %r0,BPX_ENTRY

* Initialize PARMLIST. BPX1WRT and BPX4WRT are one entry point; the service
* looks at the AMODE of its caller to read the parameter list:
*   AMODE 64: doubleword slots, the buffer slot points to a doubleword that
*             holds the buffer address (P_BUF, filled in by DO_WRITE)
*   AMODE 31: fullword slots, the buffer slot points to a fullword that
*             holds the buffer address (P_BUF), the last slot has its high
*             bit on
         LARL  %r8,PWA
         MVC   P_FD,=F'1'
         XC    P_ALET(8),P_ALET
#if __AMODE__ == 64
         LA    %r0,0(,%r8)
         STG   %r0,PARMLIST+0
         LA    %r0,16(,%r8)
         STG   %r0,PARMLIST+8
         LA    %r0,32(,%r8)
         STG   %r0,PARMLIST+16
         LA    %r0,96(,%r8)
         STG   %r0,PARMLIST+24
         LA    %r0,4(,%r8)
         STG   %r0,PARMLIST+32
         LA    %r0,8(,%r8)
         STG   %r0,PARMLIST+40
         LA    %r0,12(,%r8)
         STG   %r0,PARMLIST+48
#else
         LA    %r0,0(,%r8)
         ST    %r0,PARMLIST+0
         LA    %r0,16(,%r8)
         ST    %r0,PARMLIST+4
         LA    %r0,32(,%r8)
         ST    %r0,PARMLIST+8
         LA    %r0,96(,%r8)
         ST    %r0,PARMLIST+12
         LA    %r0,4(,%r8)
         ST    %r0,PARMLIST+16
         LA    %r0,8(,%r8)
         ST    %r0,PARMLIST+20
         LA    %r0,12(,%r8)
         O     %r0,=X'80000000'
         ST    %r0,PARMLIST+24
#endif

* Banner: the addressing mode this build runs in
         LARL  %r1,BANNER
         LGHI  %r2,BANNER_L
         BRAS  %r14,DO_WRITE
         LARL  %r1,NL_CHAR
         LGHI  %r2,1
         BRAS  %r14,DO_WRITE

* R5 = SAVED_R1
         LG    %r5,SAVED_R1
#if __AMODE__ == 31
         LLGTR %r5,%r5
#endif
         LTGR  %r5,%r5
         JZ    EXIT

* 0(R1) -> pointer to argc, 8(R1) (AMODE 31: 4(R1)) -> pointer to the argv
* array: the pointers are doublewords in AMODE 64 and fullwords in AMODE 31
#if __AMODE__ == 64
         LG    %r8,0(,%r5)
         LTGR  %r8,%r8
         JZ    EXIT
         LGF   %r10,0(,%r8)       R10 = argc (32-bit int)
         LTGR  %r10,%r10
         JLE   EXIT

         LG    %r11,8(,%r5)
         LTGR  %r11,%r11
         JZ    EXIT
#else
         L     %r8,0(,%r5)
         LLGTR %r8,%r8
         LTGR  %r8,%r8
         JZ    EXIT
         LGF   %r10,0(,%r8)       R10 = argc (32-bit int)
         LTGR  %r10,%r10
         JLE   EXIT

         L     %r11,4(,%r5)
         LLGTR %r11,%r11
         LTGR  %r11,%r11
         JZ    EXIT
#endif

* Loop R6 = 0..argc-1
         XGR   %r6,%r6

ARGLOOP  DS    0H
         CGR   %r6,%r10
         JNL   EXIT

* Fetch argv[R6]
         LGR   %r7,%r6
#if __AMODE__ == 64
         SLLG  %r7,%r7,3
         LG    %r8,0(%r7,%r11)    R8 = pointer to argument descriptor/string
#else
         SLLG  %r7,%r7,2
         L     %r8,0(%r7,%r11)    R8 = pointer to argument descriptor/string
         LLGTR %r8,%r8
#endif
         LTGR  %r8,%r8
         JZ    NEXTARG

* String data starts at offset 4 of the descriptor.
* Determine string length by scanning for null terminator (0x00).
         LA    %r1,4(,%r8)        R1 = start of string characters
         LGR   %r2,%r1            R2 = scanner
SCAN_NULL DS   0H
         CLI   0(%r2),X'00'
         BE    SCAN_DONE
         AGHI  %r2,1
         J     SCAN_NULL

SCAN_DONE DS   0H
         SGR   %r2,%r1            R2 = string length (bytes before \0)
         LTGR  %r2,%r2
         JZ    WRITENL
         BRAS  %r14,DO_WRITE

WRITENL  DS    0H
* Write ASCII newline (0x0A)
         LARL  %r1,NL_CHAR
         LGHI  %r2,1
         BRAS  %r14,DO_WRITE

NEXTARG  DS    0H
         AGHI  %r6,1
         J     ARGLOOP

EXIT     DS    0H
         LG    %r13,SAVEAREA+128
#if __AMODE__ == 64
         LMG   %r14,%r12,16(%r13)
#else
         LM    %r14,%r12,12(%r13)
#endif
         LGHI  %r15,0
         BR    %r14

* write R2 bytes at R1; the kernel service runs in the AMODE of this program
DO_WRITE DS    0H
         STMG  %r0,%r15,WR_REGS
#if __AMODE__ == 64
         STG   %r1,P_BUF
#else
         ST    %r1,P_BUF
#endif
         ST    %r2,P_LEN
         LARL  %r1,PARMLIST
         LG    %r15,BPX_ENTRY
         BASR  %r14,%r15
         LMG   %r0,%r15,WR_REGS
         BR    %r14

         DS    0D
SAVED_R1 DC    AD(0)
BPX_ENTRY DC   AD(0)
         DS    0H
NL_CHAR  DC    X'15'              EBCDIC newline (0x15) -> converts to 0x0A on terminal
         DC    X'00'
BANNER   DC    STR(AMODE __AMODE__)   C'AMODE 64' or C'AMODE 31'
BANNER_L EQU   *-BANNER
         DS    0H
         DS    0D
SAVEAREA DS    18FD
WR_REGS  DS    16FD

PWA      DS    0D
P_FD     DC    F'1'
P_RV     DC    F'0'
P_RC     DC    F'0'
P_RN     DC    F'0'
P_BUF    DC    AD(0)
P_R15    DC    AD(0)
P_ALET   DC    AD(0)
PARMLIST DC    7AD(0)
P_LEN    DC    F'0'
         DS    0D
         LTORG
         END   HELLOW
```

```sh
zosasm -DTEST31=0 -o showargs.o showargs.za   # AMODE 64 (also without -D)
zosasm -DTEST31=1 -o showargs.o showargs.za   # AMODE 31
/bin/ld -o showargs showargs.o                # on z/OS
./showargs a bb ccc                           # the banner, then ./showargs, a, bb, ccc
```

What differs between the two builds, all selected by `__AMODE__`:

* the save and restore of the registers (`STMG`/`LMG` at 16(13) or `STM`/`LM`
  at 12(13));
* the parameter list of the program: doubleword pointers and a stride of 8 in
  AMODE 64, fullword pointers and a stride of 4 in AMODE 31;
* the parameter list of the kernel call: `BPX1WRT` and `BPX4WRT` are one entry
  point and the service reads the list the way the AMODE of its caller says,
  so the AMODE 31 build builds fullword slots and the AMODE 64 build
  doubleword slots (there is no `SAM64`);
* the banner text, made with the stringify operator: `DC STR(AMODE
  __AMODE__)` becomes `DC C'AMODE 31'`.

Both builds were run on z/OS (`amode=31` and `amode=64` in the output of
`/bin/file`) and print the same arguments. A statement placed before the
`AMODE` statement would see `__AMODE__` as 0, which is why the program has a
`#error` for that case.

---

## 16. Converting HLASM source: `hlasm2za`

```sh
hlasm2za [options] input.asm        # writes input.za
```

`hlasm2za` reads HLASM source (card columns, upper case names, `COPY`) and
writes zosasm source, by default with every register in `%r1` notation, so
that the result assembles with `-strict-registers`. It is a source to source
converter that does not assemble anything and does not need the rest of the
toolchain; it understands the *form* of HLASM statements, and since C1 it converts
`MACRO` ... `MEND` definitions to zosasm macros (section 16.4).

| option | meaning |
|---|---|
| `-o FILE` | output file (default: the input with the extension `.za`; `-` = standard output) |
| `-ictl B,E,C` | source columns if the file has no `ICTL` (default `1,71,16`) |
| `-relaxed` | leave register numbers as they are (no `%r`) |
| `-w N` | break an operand list after a comma beyond column N (default 100, 0 = never) |
| `-inc-ext EXT` | extension of the files `COPY` becomes `#include` of (default `.inc`) |
| `-m` | the input is a macro library member: write the macro definitions only (default output extension `.zmac`) |
| `-macro-dir DIR` | convert every member `DIR/*.mac` like `-m`; `-o` names the output directory (default: next to the members); prints a summary |
| `-flat` | macros: `#goto`/`#label` for every sequence symbol, no structure recovery |
| `-keep-goto` | macros: where the flow cannot be structured write `#goto`/`#label` instead of a dispatch loop |
| `-copy-dir DIR` | `COPY` inside a macro reads `DIR/MEMBER.mac` (with `-macro-dir` the library directory is the default) |
| `-comment-macros` | leave macro definitions as comments (the behaviour before C1) |
| `-q` | no warnings |
| `-version`, `-h` | version, usage |

The input `-` is standard input. Warnings go to standard error as
`file:line: warning: text`; the exit status is 0 when the output was written
(warnings are not errors), 1 for a file that cannot be read or written, 2 for
a bad command line.

### 16.1 What is converted

| HLASM | zosasm |
|---|---|
| columns: names in column 1, continuation mark in column 72, sequence numbers in 73-80 | free form: the columns are cut, continued statements are joined and written again with a trailing comma where they are long |
| `ICTL begin,end,continue` | honoured for the rest of the file, then dropped (the continuation column must lie between `begin` and `end`, otherwise the `ICTL` is ignored with a warning) |
| lower case source | names, operations and operands in upper case; quoted strings stay as written; the digits of `X'..'` are upper cased |
| `* comment`, `.* comment` | `* comment` |
| remark after an operation without operands (`CSECT`, `LTORG`, `PR`...) | a `* remark` comment line before the statement (zosasm would read it as operands) |
| `YREGS` | `R0 EQU %r0` ... `R15 EQU %r15` |
| `R12 EQU 12`, `BASE EQU R12` | `R12 EQU %r12`, `BASE EQU %r12` (see 16.2) |
| register numbers: `LR 1,2`, `L 1,4(2,3)`, `USING *,12`, `DROP 12` | `LR %r1,%r2`, `L %r1,4(%r2,%r3)`, `USING *,%r12`, `DROP %r12` (floating point, access, control and vector registers get `%f`, `%a`, `%c`, `%v`) |
| `COPY name` | `#include "name.inc"` (lower case); convert the member itself with `hlasm2za -o name.inc` |
| `ISEQ` | dropped |
| `MACRO` ... `MEND` | a zosasm `#macro` (section 16.4); `-comment-macros`: one `*` line per source line |
| `SETA`, `SETB`, `SETC`, `LCLx`, `GBLx`, `AIF`, `AGO`, `ANOP` in open code | everything from the first such statement on becomes the macro `OPENCODE` and one call of it (section 16.5) |

An example. The HLASM input (72 columns, a continued operand and a continued
string):

<!-- example: hlasm-demo.asm -->
```
*        SUM OF TWO WORDS (HLASM, FIXED COLUMNS)
SUMTAB   CSECT                          THE ONLY SECTION
R1       EQU   1
R2       EQU   2
R3       EQU   3
R13      EQU   13
R14      EQU   14
R15      EQU   15
         STM   R14,R3,12(R13)
         lr    r2,r15                   lower case is folded
         using sumtab,r2
         LA    R3,TABLE
         L     R1,0(,R3)
         A     R1,4(,R3)
         ST    R1,TOTAL
         MVC   TEXT(L'TEXT),                                           X
               =C'done'
         LM    R14,R3,12(R13)
         BR    R14                      RETURN
TABLE    DC    F'10',F'20'
TOTAL    DS    F
TEXT     DC    C'a string that is split at the end of the card,        X
               and goes on here'
         LTORG
         END   SUMTAB
```

`hlasm2za hlasm-demo.asm` writes:

<!-- example: hlasm-demo.za -->
```za
*        SUM OF TWO WORDS (HLASM, FIXED COLUMNS)
* THE ONLY SECTION
SUMTAB   CSECT
R1       EQU   %r1
R2       EQU   %r2
R3       EQU   %r3
R13      EQU   %r13
R14      EQU   %r14
R15      EQU   %r15
         STM   R14,R3,12(R13)
         LR    R2,R15              lower case is folded
         USING SUMTAB,R2
         LA    R3,TABLE
         L     R1,0(,R3)
         A     R1,4(,R3)
         ST    R1,TOTAL
         MVC   TEXT(L'TEXT),=C'done'
         LM    R14,R3,12(R13)
         BR    R14                 RETURN
TABLE    DC    F'10',F'20'
TOTAL    DS    F
TEXT     DC    C'a string that is split at the end of the card,        and goes on here'
         LTORG
         END   SUMTAB
```

The result assembles with `zosasm -strict-registers`. Converting it with
`-relaxed` gives the same object (`-gz0`), which is how the converter is
tested: the strict and the relaxed text must assemble to the same bytes.

### 16.2 Registers

A number or a name in a register position of an instruction (the table tells
which operands are registers, as in 6.1), or in `USING` and `DROP`, is
rewritten like this:

* a plain number becomes `%r<n>` (`%f`, `%a`, `%c`, `%v` by the position);
* a name made with `NAME EQU <number>` or `NAME EQU <other register name>` is
  a **register name** if *every* use of it, and of the names it is an alias of,
  is as a whole register operand, always of the same kind. Its `EQU` is then
  written `NAME EQU %r<n>` (or `%f...`) and the uses keep the name;
* a name that is also used in another way (a displacement, a length,
  `LEN EQU 4` used in `LA 1,LEN`...) or in registers of two kinds stays a
  number: its `EQU` is not changed and each use in a register position
  becomes `%r<n>`;
* a name that is not defined in this file but looks like a register (`R5`,
  `FR2`, `AR1`...) is taken as that register when its kind fits the position,
  with a warning: it probably comes from `YREGS` or a `COPY` member (for such
  members convert them too; `R5 EQU 5` there becomes `R5 EQU %r5`);
* anything else (an expression, an unknown name) stays as it is, with a
  warning, and assembling with `-strict-registers` will point at it.

Literals (`=A(5)`) and everything that is not a register operand are never
touched.

### 16.3 What is not converted

These are left as they are (or commented out) and reported, so that you can
fix them by hand; zosasm then fails on them with a message that names the
line.

* **Macro definitions** are converted (section 16.4); a definition that uses
  something not converted (`COPY` without `-copy-dir`, `SETAF`/`SETCF`, a
  computed `AGO`, created `&(` names, `AREAD` with operands, a system
  variable other than the ones of section 16.4) is commented out and
  reported. Calls of **system macros** (`SAVE`, `RETURN`, `WTO`, `GETMAIN`,
  `STORAGE`, `SYSSTATE`...) are reported as "not an instruction or directive
  (a macro call?)"; zosasm has no IBM macro library of its own, so write
  what they expand to, or convert the macro library you have with
  `-macro-dir` and give the result to `zosasm -M`.
* **Conditional assembly outside a macro** is converted since 2026-11-16
  (section 16.5, not compared with HLASM beyond one program); only the
  statements of `SETAF`, `SETCF`, computed `AGO`, created `&(` names and
  `COPY` in open code are left as they are and reported.
* **`ADATA`, `ALIAS`**: not supported by zosasm (`ALIAS` is replaced by quoted
  symbols, section 2.2). `OPSYN`, `PUNCH` and `REPRO` are kept as they are:
  zosasm has `OPSYN`; `PUNCH` and `REPRO` give a warning (section 14.1).
* **`*PROCESS`** lines: the options go on the zosasm command line.
* DC and DS forms that zosasm does not have (section 14) are not checked by
  the converter; assemble the result to find them.

The converter works on the text only, so after converting run the result
through `zosasm -strict-registers -Werror`; a clean run means that nothing
was left behind.

### 16.4 Macro definitions

`hlasm2za` turns a `MACRO` ... `MEND` definition into a macro of chapter 19
(`#macro NAME(a, b = 1) {` ... `#}`). A library member is converted with
`-m`, a whole library with `-macro-dir DIR -o OUT` (every `DIR/*.mac` becomes
`OUT/NAME.zmac`; the summary gives the counts). `zosasm -M OUT` then finds the
macros by name.

What the converter does with a definition:

* **Prototype.** Positional parameters, keyword parameters with their
  defaults (`&KEY=VAL` becomes `key = "VAL"`), `&SYSLIST` (`rest...` is added
  to the parameters and `&SYSLIST(n)` is `sys.args[n-1]`). Names are lower
  case, the `&` marks of the body are kept in model statements only.
* **Variables.** `LCLA/B/C` and `GBLA/B/C` become `#let` and `#global`, put
  at the top of the macro (arrays are `fill(n, 0)` or `fill(n, "")`); `SETA`,
  `SETB`, `SETC` become assignments. A variable that is not declared is
  an empty local, with a warning.
* **Expressions.** `AIF` conditions, `K'`, `N'`, `T'`, `L'`, `D'`, `'a'(i,n)`
  substrings (1-based to 0-based), `(UPPER ...)`, `(LOWER ...)`, `(DOUBLE
  ...)`, `INDEX`, `SIGNED` and `D2A` are translated; `AND`, `OR`, `NOT`, `EQ`, `NE` and the
  rest become operators, and conditions are negated the readable way
  (`<=` becomes `>`).
* **Model statements.** `&NAME`, `&NAME.`, `&NAME(1)` become `&name`,
  `&{name}`, `&name[0]`; the name field `&LBL` becomes `&sys.label:`; `&SYSNDX`
  and `&SYSECT` become `sys.ndx` and `sys.sect`. `MNOTE` becomes `#note`,
  `#warning` or `#error` by its severity.
* **Input and output statements.** `&A SETC AREAD` becomes `aread()` and
  `AINSERT 'record',FRONT|BACK` becomes `#insert "record", front|back`
  (section 19.2). `OPSYN` and `PUNCH` stay model statements.
* **System variables.** `&SYSDATE`, `&SYSDATC`, `&SYSTIME`, `&SYSPARM` and
  `&SYSLOC` become `sys.date`, `sys.datc`, `sys.time`, `sys.parm` and
  `sys.loc`.
* **`COPY` inside a macro** reads the member `MEMBER.mac` (also `.MAC`, or the
  bare name) from the directory of `-copy-dir DIR` (with `-macro-dir` the
  library directory itself) and puts its statements in place, as the assembler
  does when it reads the definition. Without a directory, or when the member
  is not found, the macro is not converted.
* **A `MACRO` inside a macro** becomes a macro of its own that is written
  before the outer one in the same file (HLASM defines it when the outer macro
  runs; here it exists from the start, and it has no access to the variables of
  the outer macro, as in HLASM).
* **Control flow.** `AIF`, `AGO`, `MEXIT`, sequence symbols and `ANOP` are
  turned into `#if` / `#else if` / `#else`, `#while` / `#break` / `#continue`
  and `#return`. An error exit shared by many branches (a message and
  `MEXIT`) is copied to each branch as a guard clause. A chain of `AIF` tests
  that jump to blocks which all join again after them is written as one
  `#if` / `#else if` / `#else` chain, and a chain of three or more tests of
  one variable against constants becomes a `#switch` with `#case a, b:` and
  `#default:`. Flow that still cannot be shown as a structure (several
  entries into one block) is written as a **dispatch loop**: the body is cut
  into blocks at the labels that a jump reaches, `#let pc = "(start)"` names
  the block to run, and `#while (true) { #switch (pc) { #case "name": ... } }`
  runs them; a block that ends by falling into the next sets `pc` and the
  loop goes on, a jump sets `pc` and `#continue`. The macro then has no
  `#goto` at all (the variable is `pc`, or `pc_`, `pc__` ... when the macro
  already has a name `pc`). With `-keep-goto` the converter keeps the old
  form instead: `#goto name` / `#label name` for those jumps only (or the
  whole macro flat when a `#goto` would enter a block). `-flat` writes every
  sequence symbol that way.

Every conversion was checked by expanding the flat and the structured form
with many argument sets: the output must be identical (`tests/test_mconv.c`,
and the same check over a whole library with `make test-maclib`). Nothing was
compared with HLASM itself, except through one program (section 16.5); read the
result of a macro that matters against its source.


---

### 16.5 Conditional assembly in open code

*Unverified against IBM except for one program (`wr3.asm`, below).*

From the first conditional assembly statement (`SETA`, `SETB`, `SETC`,
`LCLx`, `GBLx`, `AIF`, `AGO`, `ANOP`, `ACTR`, a name starting with `.` or `&`,
or a `&variable` in any field) to the end of the source, `hlasm2za` collects
the statements into a synthetic macro `OPENCODE` with no parameters and
converts it like a macro definition (section 16.4). The result is the text
`#macro OPENCODE() { ... #}` followed by the call line `         OPENCODE`.
`&variables` that are not declared become local variables (a warning). With
`-m` (library members) nothing is wrapped.

HLASM folds the whole statement to upper case after the `&variable`
substitution, except inside quotes; zosasm symbols are case sensitive. So a
string variable that is used outside quotes in an instruction or directive is
written `&{upper(x)}`. Operands of macro calls are not folded (the called
macro does it).

Limits: registers in the wrapped code stay numeric (a `-strict-registers`
assembly flags them); `COPY` in open code is not read; statements before the
first conditional assembly statement stay in place, so labels and
sections declared there are visible to the macro as usual, but the
statements after it are all generated by the macro.

The test case is a real program that uses `&t SETA 1`, `&n SETC 'wr'`, name
pasting (`sz_#&n`, `#&n DSECT`) and the system macros `SYSSTATE`, `GETMAIN`,
`FREEMAIN`, `IHASAVER` (from a library converted with `-macro-dir`, given to
`zosasm -M`): `hlasm2za`, then `zosasm -M`, gives an object with no structural
difference to HLASM's (`tools/oracle_obj.py`); it links with `/bin/ld` and
writes the same output.

## 17. Linux on Z: ELF output (`-elf`) and the ASCII character set

`zosasm -elf prog.za` writes a **relocatable ELF64 object for Linux on Z**
(big endian, `EM_S390`, `ET_REL`) instead of a GOFF object. It is meant for the
GNU linker (`s390x-linux-gnu-ld`) or `lld`. The source language is the same;
what changes is how the result is written.

<!-- example: linux-hello.za -->
```za
* Linux on Z: write(1, MSG, MSGLEN), then exit(0).
* Assemble with   zosasm -elf linux-hello.za
* and link with   s390x-linux-gnu-ld -e START -o hello linux-hello.o
START    CSECT
         ENTRY START
         LGHI  %r2,1              arguments in r2, r3, r4; SVC n calls system call n
         LARL  %r3,MSG
         LGHI  %r4,MSGLEN
         BRAS  %r14,WRITE
         LGHI  %r2,0
         SVC   1                  exit(0)
WRITE    DS    0H
         SVC   4                  write(2,3,4)
         BR    %r14
MSG      DC    C'Hello from s390x',X'0A'
MSGLEN   EQU   *-MSG
         END   START
```

**Character set.** `-elf` implies `-ascii`: `C'..'` constants, `DC C`,
`CL`, literals, `C'x'` self-defining terms and the type attribute `T'` use
ISO-8859-1 (code points above 255 become `?`), and a character constant is
padded with blanks `0x20`. `-charset ebcdic` brings IBM-1047 back, and
`-ascii` can also be given alone to write a GOFF object with ASCII data (for
z/OS programs that run in ASCII mode). `CE'..'` is always EBCDIC and
`CA'..'` always ASCII, in `DC`, in literals (`=CA'..'`) and as self-defining
terms (`LA 1,CA'a'`, `MVI X,CA' '`), whatever the default character set; hexadecimal, packed and zoned data, floating point
and binary constants do not depend on the character set (zoned numbers keep the
`F` zone that `PACK` and `UNPK` use). Names in a GOFF object are always EBCDIC;
ELF names are the source names.

**One source for both targets.** The preprocessor defines `__ELF__` with `-elf`
and `__GOFF__` otherwise, and `__ASCII__` or `__EBCDIC__` for the character
set, so a program can carry the parts that differ (system calls against
`SVC`/`BPX` services) in one file:

```za
#ifdef __ELF__
         SVC   1                  Linux exit
#else
         LA    %r15,0             z/OS: return code 0
#endif
```

They are ordinary macros: `#ifdef`, `#if defined(...)` and `#undef` work, and
a `-D` of the same name only gives the redefinition warning.

**Mapping.**

| zosasm | ELF |
|---|---|
| each class of a `CSECT`/`RSECT` with bytes (the first class always) | one section: `.text.NAME` (`B_TEXT`, executable classes), `.rodata.NAME` (`READONLY`), `.data.NAME`, or `.bss.NAME` (`NOLOAD`, no bytes); a class other than `B_TEXT` adds `.CLASS`; an unnamed `CSECT` gives `.text` |
| class alignment (`ALIGN(n)`, default doubleword) | `sh_addralign`, at least the strictest alignment used in the section |
| `CSECT` name, `ENTRY` label | global symbol (`STT_FUNC` in executable classes, `STT_OBJECT` otherwise) |
| other labels | local `STT_NOTYPE` symbols |
| `EXTRN`, `V(...)` and referenced externals | undefined global symbols; `WXTRN` makes them weak |
| `COM` section | common symbol (`SHN_COMMON`, size and alignment) |
| `DSECT`, `EQU`, absolute symbols | not in the object |
| `A(sym)` in 4 bytes, `AD(sym)` in 8 | `R_390_32`, `R_390_64`; the addend is moved from the bytes into the RELA entry |
| `BRAS`/`BRC`/`BRASL`/`BRCL` to an external symbol | `R_390_PLT16DBL`/`R_390_PLT32DBL` with addend +2 |
| other relative branches and `LARL` to another class or an external symbol | `R_390_PC16DBL`/`R_390_PC32DBL` with addend +2 |
| `END label` | ignored: an ELF relocatable object has no entry point (use `ld -e`) |

The object also has an empty `.note.GNU-stack`, so the linker does not ask for an
executable stack. Code that stores into its own `CSECT` needs a writable
class: executable classes are read-only, so put the data in a class made with
`CATTR` (without `READONLY`).

**What cannot be written.** `Q`, `J`, `R`, `CXD`, address constants with
more than one relocatable term (`AD(A+B)`) or with a subtracted one across
sections (`AD(A-B)`), 3-byte address constants (`AL3`) and `-elf` without a
control section stop with an error that names the section. Differences of
labels in the same class need no relocation and are fine. `AMODE` and `RMODE`
have no meaning in the ELF object and are ignored, and so is `-gz`.
`A(sym)` is only 32 bits wide: on Linux on Z a 64-bit program uses `AD`.

**Not verified.** The object was checked with `readelf`, `s390x-linux-gnu-objdump`
and `s390x-linux-gnu-ld` (sections, symbols, relocations and the linked
displacements), and its relocation types and addends agree with what clang
produces for the same instructions (`brasl` gets `R_390_PLT32DBL` here, clang
uses `R_390_PC32DBL` unless `@PLT` is written). It has not been run on a Linux
on Z machine or under emulation.

**Debug information (`-elf -g`).** `zosasm -elf -g` puts the DWARF 4 into the
object: the sections `.debug_info`, `.debug_abbrev`, `.debug_line`,
`.debug_aranges`, `.debug_ranges` and `.debug_pubnames` (the last when there
are subprograms). A relocation of the DWARF against a class is a RELA entry
against the `.text.NAME` section, so `ld` relocates the addresses of the
subprograms, labels and line table. No `.dbg` file and no link module are made.
Differences from the GOFF form (section 21): the compile unit is
`DW_LANG_Mips_Assembler` and gives its code range with `DW_AT_ranges` (one
range per CSECT); the line table uses the standard `DW_LNS_set_file`; there is
no `.debug_srcfiles`. `-gside`, `-gdwarf` and `-gdbgpath` are errors with
`-elf`. Hexadecimal floats use IBM's encoding (`DW_ATE_IBM_float_hex`), which
LLDB decodes and other debuggers show as raw values. Checked with `readelf`
and by linking with `s390x-linux-gnu-ld` (`MAIN`, `SUB` and `MSG` at the
addresses the linker gives them, and the subprogram ranges in `.debug_aranges`).
Run under `qemu-s390x -g PORT` with `gdb-multiarch` (`set architecture
s390:64-bit`, `target remote`): breakpoints by source line and by label stop at
the linked addresses, `list` and `next` follow the source, a DC label prints its
bytes (`print MSG` gives `"Hi"`), and registers show the values the code set.
**Call frame information (`DWCFI`).**

*What CFI is.* A debugger stops in a function and must find the function that
called it (for `bt`, `up` and `finish`). To do that it has to know where the
caller's registers are at that moment. Call frame information (CFI) is DWARF data
in the `.debug_frame` section that gives those rules for each address of the
code. Two concepts make up the rules:

- The **CFA** (canonical frame address) is a fixed reference point for one
  frame. Here it is the stack pointer the caller had before the call, which is
  `r15` at entry. Every other rule is relative to it.
- A **register rule** says where a register of the caller is saved, as an
  offset from the CFA, or that it has the same value, or that it has no value.
  The return address (`r14`) is the register that tells the debugger where the
  caller continues.

The rules change while the function runs. In the example below the `STMG`
saves `r14` at `112(r15)` while `r15` still is the caller's stack pointer, so
the save is at CFA+112. After `LAY` lowers `r15` by 160, `r15` no longer is the
caller's stack pointer, so the CFA must be written as `r15+160` from then on.
Each `DWCFI` directive records the rule from the address where it stands.

Without CFI the debugger cannot walk past the current frame ("frame did not save
the PC"), so `bt` shows only the frame where the program stops. The assembler
cannot derive the rules by itself: it sees `STMG` and `LAY` as ordinary
instructions, and only the author knows which register holds the frame base.
Write the rules with the `DWCFI`
directive (it is not `CFI`: that is the Compare Immediate Fullword instruction)
after the instruction that changes the frame, with operands separated by commas:

| Directive | Meaning |
|-----------|---------|
| `DWCFI CFA,reg,off` | the CFA is `reg + off` (the caller's stack pointer is the CFA) |
| `DWCFI SAVE,reg,off` | register `reg` of the caller is saved at `CFA + off` |
| `DWCFI SAME,reg` | register `reg` has the same value in the caller |
| `DWCFI UNDEF,reg` | register `reg` has no value in the caller (outermost frame) |

`reg` is `%r0`-`%r15` or 0-15, `off` is a decimal number. The CIE says the CFA
is `r15 + 0` (the stack pointer at entry, which is what the caller had before
the call) and that the return address is `r14`. A fragment of a subroutine
(not a complete program, so the test suite does not assemble it):

```
MAIN     CSECT
         STMG  %r14,%r15,112(%r15)
         DWCFI SAVE,14,112
         LAY   %r15,-160(%r15)
         DWCFI CFA,15,160
         ...
         BR    %r14
```

An entry point that is not called (the program start, with `r14` = 0) ends the
backtrace: `DWCFI UNDEF,14` there, as in the `START` program of the gdb check. Rules are
given at the offset where the directive stands, and they stay until the next
directive. The directive goes into `.debug_frame` as one FDE per CSECT (ELF form
only). Bad operands give `ZA3069`; a directive before any section is open is an
error, as for any other statement. Checked under `qemu-s390x` with
`gdb-multiarch`: `bt` from a subroutine shows the caller and the start of the
caller's caller, and the caller's `$pc` and stack pointer are recovered.

Not done: CFI with `-g` in the GOFF form (there the link module has no `.debug_frame`),
and LLDB and dbx on the ELF object. Without CFI gdb cannot unwind past the current
frame ("frame did not save the PC"), so `bt` shows only the frame where the program
stops.

---

## 18. For AI agents: the MCP server `zosasm-mcp`

`build/zosasm-mcp` is a **Model Context Protocol** server. An AI agent that
speaks MCP (an IDE assistant, a command line agent, ...) can call zosasm
without knowing its command line: the server lists its tools with a typed
description of every argument and of the result, and it serves this guide so
that the agent can learn the language first. It is built by `make` together
with `zosasm` and `hlasm2za`, which it runs as separate processes.

### 18.1 Registering the server

The transport is **stdio**: the client starts the program and exchanges one
JSON-RPC message per line on standard input and output. A typical client
configuration (the file name and the exact keys depend on the client):

```json
{
  "mcpServers": {
    "zosasm": { "command": "/path/to/zosasm/build/zosasm-mcp" }
  }
}
```

The server finds `zosasm` and `hlasm2za` in its own directory and this guide
at `../docs/GUIDE.md` relative to that directory. If you install the programs
elsewhere, say where they are:

| setting | meaning |
|---|---|
| `--bin-dir DIR` or `ZOSASM_BIN_DIR` | directory of `zosasm` and `hlasm2za` |
| `--guide FILE` or `ZOSASM_GUIDE` | the user's guide (and `examples/` next to it) |
| `ZOSASM_UNRESTRICTED` | set to a non-empty value to lift the file name limit below |
| `--version`, `--help` | print and exit |

**File names are limited to the server directory (the *workspace*).** Every
file name a tool call gives — `path`, `output`, `dwarf_output`,
`output_dir`, `macro_dir`, `copy_dir`, the entries of `include_dirs` and
`macro_dirs` — must resolve to inside the directory of the server binary.
Relative names are taken relative to that directory (not the working
directory of the server), absolute names must already be inside it, and
`..` or a symlink cannot lead out of it; a name outside is an error that
says so. An operator who wants the tools to reach the whole machine can
start the server with `ZOSASM_UNRESTRICTED` set: the limit is lifted and
relative names are relative to the working directory again. Inline
`source` with `files` needs no workspace at all: the files are written to
a temporary directory the server manages itself.

When the client sends `initialize`, the reply carries *instructions* for the
agent: what the server is for, the usual order of calls, and the **absolute
path of this guide**. That is how the agent is pointed at the guide.

### 18.2 The tools

All tools return a text summary (`content`) and the same data as a JSON object
(`structuredContent`). `isError` is true when the tool failed, for example
when the assembly has errors; the text then says why. Give **exactly one** of
`source` (the text, inline) or `path` (a file on the machine that runs the
server) where a tool takes both.

**`zosasm_assemble`** assembles a program.

| argument | type | meaning |
|---|---|---|
| `source` / `path` | string | the program text, or a file name |
| `name` | string | with `source`: the name shown in diagnostics (default `input.za`) |
| `defines` | array of strings | `-D`: `"NAME"` or `"NAME=value"` |
| `include_dirs` | array of strings | `-I` directories for `#include` |
| `files` | object | extra include and macro files `{"regs.inc":"text"}`; `NAME.zmac` there is a library macro |
| `macro_dirs` | array of strings | `-M` directories of library macros (chapter 19) |
| `sysparm` | string | `-sysparm`: the value of `sys.parm` in macros |
| `macro_steps` | integer | `-macro-steps`: limit for one macro expansion |
| `format` | `goff` or `elf` | object format (default `goff`; section 17 for `elf`) |
| `charset` | `ebcdic` or `ascii` | character set of `C'..'` (`-charset`) |
| `strict_registers` | boolean | `-strict-registers` |
| `output` | string | file to write the object to; without it nothing is kept and the call only checks the program |
| `object_base64` | boolean | also return the object, base64 encoded |
| `listing`, `listing_all_bytes`, `listing_brief` | boolean | return the listing (`-l`, `-ldata`, `-lbrief`) |
| `debug_info` | 0, 1, 2 | `-gz0/1/2` (GOFF only) |
| `werror`, `reproducible` | boolean | `-Werror`, `-reproducible` |
| `max_errors` | integer | `-max-errors` |

Result: `success`, `exit_code`, `format`, `diagnostics` (the text zosasm prints
on standard error: code `ZAnnnn`, position, source line, help), `object_size`,
`output`, `object_base64` and `listing` when asked for. No object is written
when there are errors, but the listing and the diagnostics are returned.

**`zosasm_preprocess`** runs only the preprocessor (`-E`). Arguments: `source` /
`path`, `name`, `defines`, `include_dirs`, `files`, `format`, `charset`,
`strict_registers`, `macro_dirs`, `sysparm`, `macro_steps`. Result:
`preprocessed`, `diagnostics`.

**`zosasm_dump`** prints the records of a GOFF object (`-dump`). Arguments:
`path` or `object_base64`. Result: `dump`. ELF objects are not read.

**`zosasm_disasm`** turns a GOFF object back into source (`-disasm`).
Arguments: `path` or `object_base64`, `generic` (`-generic`),
`strict_registers`. Result: `source`, `diagnostics`.

**`zosasm_convert_hlasm`** converts HLASM source with `hlasm2za` (section 16).
Arguments: `source` / `path`, `name`, `relaxed`, `width` (`-w`), `ictl`
(`-ictl`), `include_extension` (`-inc-ext`) and for macros (section 16.4):

| argument | type | meaning |
|---|---|---|
| `macros` | boolean | `-m`: the input is a macro library member; only the definitions are returned |
| `flat` | boolean | `-flat`: `#goto`/`#label` for every sequence symbol |
| `keep_goto` | boolean | `-keep-goto`: `#goto` instead of a dispatch loop where the flow cannot be structured |
| `comment_macros` | boolean | `-comment-macros`: leave macro definitions as comments |
| `copy_dir` | string | `-copy-dir`: where `COPY` inside a macro reads its members |
| `macro_dir`, `output_dir` | strings | `-macro-dir`: convert every `DIR/*.mac` and write `output_dir/NAME.zmac` (instead of `source` / `path`; `output_dir` is required; this writes files) |

Result: `source` and `diagnostics` (with `macro_dir`: `output` and
`diagnostics`, the counts and what was not converted). The diagnostics list
what has to be rewritten by hand (calls of
macros that are not in the source, macro definitions that could not be
converted). Macro definitions in the source are converted to `#macro` text.
To use converted macros, give `macro_dirs` (or `files`) to
`zosasm_assemble`.

**`zosasm_guide`** reads this guide. No arguments: how to use it and the table
of contents. `section`: a chapter (`"13"`), a subsection (`"12.3"`), part of a
heading (`"listing"`) or `"all"`. `search`: the lines containing a word, with
line number and section (at most 40). Result: `guide_path`, `text`.

An agent that does not know the language should call `zosasm_guide` first
(chapters 1, 2, 6 and 13 are the shortest way in), write the source, call
`zosasm_assemble`, read `diagnostics`, and repeat. Section 11.1 explains the
messages.

### 18.3 Resources

The same material is available as MCP resources, for clients that show
resources to the user or attach them to the conversation:

| URI | content |
|---|---|
| `zosasm://guide` | this whole guide |
| `zosasm://guide/section/{section}` | one chapter or subsection (a template) |
| `zosasm://examples/NAME` | a file of `docs/examples/` |

### 18.4 A session

Client lines are shown without the `jsonrpc` member for brevity:

```text
-> {"id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18",...}}
<- {"id":1,"result":{"serverInfo":{"name":"zosasm-mcp",...},"instructions":"...GUIDE.md..."}}
-> {"method":"notifications/initialized"}
-> {"id":2,"method":"tools/call","params":{"name":"zosasm_assemble",
     "arguments":{"source":"T CSECT\n LR 1,2\n","listing":true}}}
<- {"id":2,"result":{"content":[{"type":"text","text":"zosasm succeeded ..."}],
     "structuredContent":{"success":true,"exit_code":0,"format":"goff",
       "diagnostics":"","object_size":880,"listing":"..."},"isError":false}}
```

### 18.5 Limits and security

* The server has no state: every call runs the programs in a fresh temporary
  directory that is removed afterwards.
* `path` and `output` use the file system of the machine that runs the server
  with the rights of the server's user. Run it as an unprivileged user; a
  client that must not write files simply does not give `output`.
* Texts are limited to 1 MB per result; `search` returns at most 40 lines.
* Messages are read one per line, UTF-8, JSON; batches are not supported.
* Like the rest of zosasm, objects are not run or linked, and GOFF output is
  **not verified against IBM** (section 14).

## 19. Macros: the C-like macro language

The preprocessor of chapter 3 replaces text. For anything that needs a loop,
a decision or a computed name, zosasm has **macros**. They do the work of
HLASM's conditional assembly (`SETA`, `AIF`, `AGO`, `ACTR`, `MNOTE`...) with
the same meaning but a C like syntax: blocks with braces, free indentation,
`#let`, `#if`, `#for`. `MACRO.md` has the full design; this chapter is the
working summary.

### 19.1 A first macro

<!-- example: cmacros.za -->
```
* The C-like macro language: macros, loops, a switch and open code variables.
#macro STOREREGS(regs, base = 13, off = 20, size = 8) {
    // one STG per register in the list
    #let at = off
    #for (let r : regs) {
        STG   &r,&at(,&base)
        #at += size
    #}
#}
#macro FILLZ(area, len) {
    #switch (attr(len, "T")) {
    #case "N":
        #let left = int(len)
        #let at = 0
        #while (left > 256) {
            XC    &area+&at(256),&area+&at
            #at += 256
            #left -= 256
        #}
        XC    &area+&at(&left),&area+&at
    #default:
        #error "FILLZ: the length must be a number"
    #}
#}
#let n = 3
CMAC     CSECT
         USING CMAC,12
         STOREREGS (2,3,4)
         STOREREGS (14,15),base=11,off=8
         FILLZ BUF,600
#for (let i = 0; i < n; i++) {
    DC    A(&i)
#}
BUF      DS    CL600
         END
```

* A line whose first non-blank character is `#` is **macro code**; any other
  line in a body is a **model statement**, the text that is produced. A
  model statement is `[label:] operation operands [remark]`; `&name`
  inserts a variable, `&{expr}` any expression, `&&` a literal `&`. The
  result is read again as source, so `&op &a,&b` can name an instruction or
  another macro. A name that is not a variable (`&nope`) is the error ZA4110.
* `#macro NAME(a, b = 7, rest...) {` ... `#}` defines a macro. It is a
  statement: write the name in the operation field and the arguments after
  it. Arguments are the text as written (`(2,3,4)` is a list, `C'A,B'` is one
  argument); `name=value` gives a keyword argument when `name` is a
  parameter; an empty argument (`A,,C`) takes the default; a call may
  continue on the next line after a comma. The name field of the call is
  `&sys.label`.
* `//` starts a macro comment (not emitted); `*` as first character is an
  assembler comment line that is emitted. Macro code and model statements
  cannot start in column 1 (ZA4104); a `label:` is written with a colon.
* `-E` prints the expansion; the listing marks generated lines with `+`.

### 19.2 Statements and values

```
#let x = expr         #x = expr   #x += 1   #x++   #x[i] = v     #global g = 0
#if (c) {  #} else if (d) {  #} else {  #}
#while (c) {  #}      #for (let i = 0; i < n; i++) {  #}      #for (let r : list) {  #}
#switch (e) {  #case 1, 2:  #case "A":  #default:  #}
#break  #continue  #return  #goto name  #label name
#emit text   #note text   #warning text   #error text
```

* **Values** are `int` (64 bit), `str` and `list`. An argument such as
  `(a,b,(c,d))` is a list: `count(x)` is 3, `x[0]` is `a`, `x[2][1]` is `d`
  (indexing starts at 0). Names are not case sensitive.
* `+` adds when one side is an int and the other a number, otherwise it
  joins strings (`"L" + n`). Strings compare as in HLASM: a shorter string is
  smaller (`"9" < "15"`).
* A `#let` lives in its block; declaring a name twice in a block is an error,
  hiding an outer one a warning (ZA4107). `#global` is shared by all macros
  and open code, the first declaration sets the value.
* Functions: `len count substr index upper lower trim repeat double quote byte
  chars split join list int str hex bin min max abs empty defined attr`.
  `attr(x, "T")` is `N` for a self-defining term, `O` for an omitted
  argument and `U` otherwise (the symbol table is not available).
* Built-in names: `sys.label`, `sys.ndx` (`0001`, `0002`... one per call),
  `sys.sect`, `sys.sectype`, `sys.macro`, `sys.level`, `sys.args`, `sys.argc`,
  `sys.loc` (the name of the current location counter: the last `LOCTR`, or the
  section), `sys.date` (`mm/dd/yy`), `sys.datc` (`yyyymmdd`), `sys.time`
  (`hh.mm`) and `sys.parm` (the text of `-sysparm`). The date and time are
  those of `__DATE__`: `SOURCE_DATE_EPOCH`, and an error with `-reproducible`
  when it is not set.
* Input and output of the source stream (HLASM `AREAD` and `AINSERT`):
  `aread()` returns the next record after the macro call (padded or cut to 80
  columns, `""` at the end of the input) and takes it out of the source;
  records queued by `#insert` are read first. `#insert "record"[, front|back]`
  queues a record (cut to 80 columns, `back` is the default) that is read as
  source after the outermost macro call has ended; `front` puts it before the
  records already queued. The records are rescanned like generated lines, so
  they can be a macro call, but not a `#macro` definition or a `#` directive.
* The same statements work in open code (the `#let n = 3` and `#for` of the
  example). An old style `#if expr` ... `#endif` without a `{` is still the
  preprocessor.

### 19.3 Libraries and limits

* An unknown operation is looked up as `name.zmac`, then `name.mac` (then
  the same with the name in upper case, as `hlasm2za -macro-dir` writes
  it) in the `-M` directories and then the `-I` directories. A file may define several
  macros. This is how a directory of converted HLASM macros is used
  (`hlasm2za -macro-dir`, section 16.4).
* A macro calls others and itself; the depth is limited to 64 (ZA4120) and an
  expansion to `-macro-steps N` statements, default 1,000,000 (ZA4121; the
  HLASM `ACTR`). Macro expressions nest at most 200 levels (ZA4100), and so do
  preprocessor `#if` expressions (ZA4030); a number that does not fit in 64
  bits is an error, not a wrapped value.
* `#error`, `#warning`, `#note` give ZA4050, ZA4051, ZA4052 and name the
  macro and the call. Other macro diagnostics are ZA4100 to ZA4130
  (`MACRO.md` section 11).
* **Not verified against HLASM**: no HLASM was available to compare the
  expansion of IBM's macros with this implementation.

## 20. Formatting source: `zafmt`

```
zafmt [-i | -o FILE | -check] [-op N] [-remark N] [-indent N] [-blank N] [file ...]
```

`zafmt` rewrites `.za` source into one layout. Without a file (or with `-`) it
reads standard input; the result goes to standard output, back into the file
with `-i`, or to the file named by `-o`. `-check` changes nothing, lists the
files that are not formatted and exits with 1 when there is one (for a commit
hook or a build). Only blanks and line ends change: no token is added, removed
or reordered, so the object is the same (checked by assembling the examples and
by expanding the converted macro library before and after formatting).

| rule | default | option |
|---|---|---|
| name in column 1, operation in column | 10 | `-op` |
| operands start 6 columns after the operation (one blank after a longer one; a long name pushes the operation right, the operands keep their column unless the operation reaches them) | | |
| remarks start in column | 35 | `-remark` |
| a continuation line (operands ending in a comma) starts at the operand column | | |
| at most this many blank lines in a row | 2 | `-blank` |
| indentation per block level of the macro language | 4 | `-indent` |

* Remarks of neighbouring lines whose operands reach the remark column are
  aligned with each other; the other remarks stay in the remark column.
* Text that starts in the remark column or beyond is kept as a remark, also
  after an operation without operands (`LTORG   the pool is here`). Text that
  starts closer to the operation is operands as far as zosasm is concerned,
  so it is aligned like operands (the result assembles the same).
* Comment lines (`*`, `;`, `//` in column 1) are kept. Trailing blanks go,
  tabs between fields become spaces, CR LF becomes LF, the file ends in one
  newline. The record after `REPRO` is data and is not touched.
* Lines of the macro language (`#...`) stay in column 1 outside blocks. Inside
  a braced block (`#macro`, `#if (..) {`, `#for`, `#while`, `#switch`) they are
  indented by `-indent` per level; `#}`, `#} else {` are one level out, `#case` and
  `#default` are at the level of their `#switch` and the statements below them
  one level in. The statements of the generated source in a block have no fixed
  columns: they are indented like the `#` lines, a label is written `NAME:` and
  the operation follows after one blank. `#include`, `#define`, `#if`..`#endif`
  without braces are directives and are not indented. A `#` statement that
  continues on the next line (open parenthesis or `\`) keeps its shape: the
  continuation lines move with the first line.
* A line whose quote or parenthesis is not closed is only stripped of trailing
  blanks.

Not done: changing the case of names or operations, wrapping long operand
lists, aligning `EQU` or `DC` columns across lines.

## 21. Debug information for dbx and LLDB: `-g`

`zosasm -g` puts the DWARF for dbx and the LLDB port into the object
itself: a second GOFF module (the link module) carries the DWARF as a NOLOAD
class, `C_ADATAE9C3`. Nothing is loaded at run time and no separate file is
needed after `ld` has made the program:

```sh
zosasm -g -gdir /u/me/proj -o prog.o prog.za     # writes prog.o with the DWARF inside
z-scp prog.o prog.za me@zos:/u/me/proj/          # tagged transfer, see below
ssh zos 'cd /u/me/proj && ld -o prog prog.o && lldb prog'
```

`-gside` also writes `prog.dbg` with the same DWARF, for dbx, which does not
read the class:

```sh
zosasm -gside -gdir /u/me/proj -o prog.o prog.za # prog.o and prog.dbg
```

`as -g` writes the `.dbg` only, next to the object. The embedded copy is a
zosasm addition.

`zosasm` is a cross assembler, so the paths written into the debug
information must be those of the **z/OS host**, not of the machine that
assembles:

| option | meaning |
|---|---|
| `-g` | put the DWARF into the object (class `C_ADATAE9C3`) and append the link module |
| `-gside` | also write the `.dbg` file (for dbx); implies `-g` |
| `-gdwarf FILE` | write the `.dbg` here, name of the file on this machine (default: the object name with `.dbg`); implies `-gside` |
| `-gdir DIR` | the z/OS directory that corresponds to the current directory. A source `sub/a.za` is recorded as `DIR/sub/a.za`, the `.dbg` as `DIR/prog.dbg` |
| `-gmap FROM=TO` | files outside the current directory: a path prefix on this machine and the one on z/OS (several allowed, applied in order) |
| `-gdbgpath PATH` | where the `.dbg` will be on z/OS, when it is not simply the name of the object under `-gdir` (at most 200 bytes) |

Without `-gdir` the local absolute paths are recorded; that works when the
files are mounted (or copied) under the same names. LLDB reads the DWARF from
the object first; a `.dbg` is looked up only when the object has no embedded
copy for it, at the recorded path and then in its search directories
(`target.debug-file-search-paths`). dbx looks for the `.dbg` at the recorded
path and in `dbx -I` / `_DBX_USE_PATHNAME`, so use `-gside` for dbx. The
source is looked up at the recorded path too.

**Transfer.** The `.o` (and `.dbg`, with `-gside`) is binary. The source must be tagged
`ISO8859-1` (z-scp does that); an untagged source breaks `as`, and debuggers
cannot show it.

### 21.1 What is written

* The DWARF is an ELF32 big endian relocatable file (machine 22) with DWARF 4 (the
  same bytes as `prog.dbg` with `-gside`):
  `.debug_info`, `.debug_abbrev`, `.debug_line`, `.debug_aranges`,
  `.debug_pubnames` and IBM's `.debug_srcfiles` (the file table of the
  `DW_TAG_IBM_src_file` entries), their `.rel.*` sections, and the symbols
  `.text_cu1_id<N>` (one per CSECT or RSECT; the value is the offset of the
  section in the `B_TEXT` class of the module) and `.ADATA_<32 hex>`, the
  signature that ties the file to its object.
* In the DWARF: one compile unit (`DW_LANG_IBM_Hlasm`, the producer is
  `zosasm 0.2.0`); a `DW_TAG_subprogram` per CSECT/RSECT that has instructions;
  a `DW_TAG_label` for every label in code; a `DW_TAG_variable` with
  `DW_OP_addr` and a base or array type for every `DC`/`DS` label (the type
  follows the type letter and the length: `int32`, `char[10]`, ...); the line
  table with one sequence per section, one row per instruction (and `CNOP`);
  all files that were read (`#include`, `COPY`-like) as separate file entries.
  Statements that a macro generates are attributed to the line of the call.
* DSECTs (this goes beyond `as -g`, which writes nothing for them): a named
  `DSECT` becomes a `DW_TAG_structure_type` whose members are its `DC`/`DS` labels
  (`type lookup RECD` in LLDB). Where a `USING RECD,3` (labeled or not, one base register)
  is in force, from the `USING` to the `DROP` or the end of the CSECT, the code has a
  lexical block with a variable of that structure type at the register, so
  `frame variable` shows the record, for example `(RECD) OLD = (CNT = 99, NAME = "WORLD   ")`.
  The name is the USING label or the DSECT name. A dependent `USING` is placed
  at the register of the `USING` it depends on, plus the distance; the limits of
  a `USING` range do not matter, and an absolute `USING` gives no variable.
* An `EQU` with a value is a `DW_TAG_variable` with `DW_AT_const_value`, typed
  `int64` (`p LEN`, `target variable LEN` in LLDB). Register equates such as
  `R3 EQU 3` are included: the debug information cannot tell them from other
  constants.
* In the object: a second GOFF module after the normal one, as `as -g` writes
  it: the classes `D_@@ADBG_DBG` (part `DBGNAME`: length, the signature, the
  EBCDIC path of the `.dbg`), `D_@@ADBG_MAP` (part `ENTRIES`, 32 bytes per CSECT)
  and `D_@@ADBG_LIT`, and an ER for every CSECT. The binder accepts the file
  as one input; `-dump` shows both modules, `-disasm` and `-unpack` read the
  first one.
* The DWARF ELF itself is in that module as the class `C_ADATAE9C3` (NOLOAD,
  unstructured, like the ZSYM class `C_ADATAE9C1`): each copy is preceded by
  its 8-byte big-endian length, so several modules can share the class after
  `ld`. The class is not in the first module, because the signature covers it.
* The signature is the MD5 of the first module and of the DWARF sections, so
  the `.dbg` matches only its own object.

### 21.2 Checked and not checked

Verified on z/OS 2.5 (pok56, 2026-11): the object links with `/bin/ld`; IBM
`dwarfdump -i -l` reads the `.dbg`; the LLDB port loads it through CDA and
sets breakpoints by file and line, shows the source, steps, makes a back
trace through several CSECTs, and the module of a source that includes a
second file resolves lines in both files; the module structure agrees with
the one of `as -g` (ESD attributes, text, relocations) except for the
signature, the producer and the CSECT numbering; DSECT structure types and
the variables at a `USING` were checked with `dwarfdump` and `type lookup` /
`frame variable` of the LLDB port. dbx 2.4 on that system
shows no symbols for modules made by `as -g` either, so it could not be used
as a reference.

Not the same as `as -g`:

* The DWARF is zosasm's own. The producer string differs; the line program
  header is version 3 and has a file table, where IBM's has an empty one and
  names the file with a vendor opcode; data types are integers, bytes, hexadecimal
  floats and arrays of them, and DSECT structures.
* The `.dbg` has no symbols for the `B_IDRL` and `B_PRV` classes (IBM's has
  them, with size 0).
* `-gside`, `-gdwarf` and `-gdbgpath` are errors with `-elf`: the ELF object
  holds the DWARF itself (section 17), so there is no `.dbg` file.
* DBGNAME holds at most 200 bytes of path.

