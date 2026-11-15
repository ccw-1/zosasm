# zosasm User's Guide

`zosasm` is an assembler that runs on any machine with a C compiler and
writes **z/OS GOFF object files**. The language is close to IBM's High Level
Assembler (HLASM), with a few deliberate differences: no card columns, a C
style preprocessor instead of macros, symbols without length limit, and a
set of assembly-time helpers (`sizeof`, `ASSERT`, ...). This guide teaches
the language and the tool; the *why* of each decision is in `DESIGN.md`.

**What to expect.** The object files follow IBM's GOFF documentation, and the
instruction table is generated from the *z/Architecture Principles of
Operation*. At the time of writing, no object made by zosasm has been
compared with one made by IBM's assembler or run through the z/OS binder
(no z/OS system was available). Where this guide says that something is
"not verified against IBM", treat the exact bytes as an interpretation of the
documentation until a z/OS test has been done. Section 14 lists what is
affected.

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
11. Diagnostics
12. Looking inside an object: `-dump`, ZSYM, `-disasm`, `-unpack`
13. Command reference
14. Differences from HLASM and known limits
15. Recipes: a subroutine, a table, build variants, source round trip

### Feature index

| I want to ... | see | example |
|---|---|---|
| assemble a first program, see what comes out | 1 | `hello.za` |
| write names, numbers, expressions, quoted symbols | 2 | `symbols.za` |
| use `#include`, `#define`, `#if`, `-D`, stringify | 3, 15.3 | `prep.za`, `config.za` |
| control sections, classes, entry points, externals | 4 | `sections.za` |
| define data: `DC`, `DS`, `EQU`, `ORG`, literals | 5 | `data.za` |
| write instructions, name registers | 6 | `symbols.za` |
| map storage with DSECTs, `USING` (labeled, dependent, ranges) | 7 | `using.za` |
| use external dummy sections (`DXD`, `CXD`, `Q`-type) | 8 | `xd.za` |
| measure with `sizeof`, `offsetof`, `countof`; check with `ASSERT` | 9 | `builtins.za` |
| floating point, UTF-16, `CCW`, `A(X-Y)` | 10 | `floats.za` |
| understand an error or warning | 11 | |
| dump, disassemble, keep and unpack sources | 12, 15.4 | |
| every command line option | 13 | |
| know what differs from HLASM, what is unverified | 14 | |
| a subroutine, a table search, build variants | 15 | `linkage.za`, `table.za`, `config.za` |

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

HLASM's macro language is not part of zosasm. Instead a C style preprocessor
runs first, on the whole file. A `#` in column 1 starts a directive only if it
is followed by one of the keywords below; otherwise it is an ordinary label
(`#COUNT` is a fine symbol).

| directive | meaning |
|---|---|
| `#include "file"`, `#include <file>` | insert a file (with quotes: next to the including file first, then in the `-I` directories; with angle brackets: only the `-I` directories) |
| `#define NAME text`, `#define NAME(a,b) text`, `#undef NAME` | object-like and function-like macros; `##` pastes |
| `#if`, `#ifdef`, `#ifndef`, `#elif`, `#else`, `#endif` | conditional text; `#if` takes integers, `defined(X)`, `! && \|\| == != < > <= >= + - * / % ~` and parentheses |
| `#error text`, `#warning text` | stop or warn |
| `#pragma ...` | accepted (`title`, `eject`, `space`, `print`); no effect yet |

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
| `__ZASM_VERSION__` | `major*10000 + minor*100 + patch` (0.1.0 is `100`) |
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
`NOLOAD`, `DEFLOAD`, `MERGE`, `RENT`, `REUS`, `NONREUS`, `PART(name)`.

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
PRINTF   XATTR LINKAGE(XPLINK),SCOPE(LIBRARY)   describe the ER

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
| `Q`, `J`, `R` | pseudo register offset, length, PSECT address | 4 | section 8 |
| `P`, `Z` | packed, zoned decimal | as written | |
| `E`, `D`, `L` | hexadecimal float short, long, extended | 4, 8, 16 | `EB DB LB` binary, `ED DD LD` decimal, `LQ` quadword aligned |

Modifiers and extensions that are not supported and give an error that says
so: `G` constants, `S` and `SY` constants, `QY`, bit lengths (`L.n`).

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

### 5.2 Literals

`=F'100'` (or any DC operand after `=`) in an instruction operand stores the
constant in a literal pool and uses its address. The pool is written at the
next `LTORG` or, failing that, at `END`. Identical literals are shared; the
pool is doubleword aligned and sorted by alignment 8, 4, 2, 1. A literal must
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
* `QD`, `JD`, `RD` and `QL3`... give other sizes; `QY` is not supported.

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
         LHI   2,offsetof(PARM,P_TABLE)  16
         LHI   3,countof(P_TABLE)        4
         LHI   4,alignof(PARM)           8
         LHI   5,sizeof(P_TABLE)>=16     comparisons give 1 or 0

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
RND      DC    EB'0.1R5',EB'0.1R4'      toward zero, nearest even
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

## 11. Diagnostics

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

Code ranges:

| codes | area |
|---|---|
| `ZA0xxx` | syntax of lines, strings, quoted symbols, attributes |
| `ZA1xxx` | symbols and expressions; `ZA104x` sizes; `ZA105x` ASSERT |
| `ZA2xxx` | instructions, operands, DC/DS and constants; `ZA206x` XD and CCW |
| `ZA3xxx` | sections, GOFF directives, USING (`ZA306x`) |
| `ZA4xxx` | the preprocessor |
| `ZA9xxx` | not supported yet / internal |

Codes do not change from one version to the next, so you can search for them
and use them in scripts.

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
```

| option | meaning |
|---|---|
| `-o FILE` | output file (default: input with `.o`; for `-disasm` stdout; for `-unpack` the directory, default `.`) |
| `-DNAME[=VAL]` | define a preprocessor macro (value 1 if omitted) |
| `-Idir` | add an include directory |
| `-E` | print the preprocessed source and stop |
| `-gz0`, `-gz1`, `-gz2` | symbolic information level (default `-gz1`) |
| `-generic` | with `-disasm`: ignore ZSYM |
| `-reproducible` | fail on anything that makes the object differ between runs |
| `-version` | print the version |
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
| macros, `AIF`, `SETC`, `COPY` | C style preprocessor; `COPY` is an error that says `#include` |
| `ALIAS`, 63 character names | quoted symbols, no limit |
| `MNOTE` | `ASSERT`, `#error`, `#warning` |
| `*PROCESS`, `ACONTROL`, `PUSH`/`POP` | `#pragma`, options on the command line |
| `*-LABEL` length tricks | `sizeof`, `offsetof`, `alignof`, `countof` |
| listing | none yet (`TITLE`, `EJECT`, `SPACE`, `PRINT`, `PUNCH`, `ICTL`, `ISEQ` are accepted and ignored) |
| object deck / OBJ output | GOFF only |

**Not implemented.** `ACONTROL`, `ADATA`, `AINSERT`, `CEJECT`, `EXITCTL`,
`MNOTE`, `OPSYN`, `PUSH`, `POP`, `REPRO`; DC types `G`, `S`, `SY`, `QY`; bit
length modifiers (`L.n`); `CU` code page options (UTF-16LE/UTF-8); register
pair checks (an odd register where a pair is needed assembles and fails when
run).

**Not verified against IBM.** No z/OS system has been available. The
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
