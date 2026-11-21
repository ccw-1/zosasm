# zosasm macro language - design and status

Status: **M1 implemented** (2026-11-16: language, library lookup `-M`,
`-macro-steps`, `-E`, listing, `tests/test_macro.c`, guide chapter 19);
**C1 (`hlasm2za` conversion of MACRO..MEND) implemented** (2026-11-16: `src/mconv.c`, `src/mflow.c`, `src/mpost.c`, `-m`, `-macro-dir`, `-flat`, `-keep-goto`, `tests/test_mconv.c`, `make test-maclib`; structure recovery by ranges, not the full flow graph of section 10, `else if`, `#switch`, and a dispatch loop for the rest; COPY, AREAD, AINSERT, OPSYN, PUNCH and nested macros convert; DESIGN.md 16.1). Nothing was
compared with HLASM (none available). Refinements made while coding M1:

* `+` adds when one side is an int and the other a numeric string (so
  `#off += size` with `size = "8"` adds); otherwise it joins strings.
* A keyword argument `name=value` is one only if `name` is a parameter of
  the macro, else the argument is positional; any named parameter can be
  given by keyword.
* A mixed int/string `==`/`!=` compares numerically when the string is a
  number, else the values differ.
* An unknown `&name` in a model statement is ZA4110 (not ZA4111).
* The remark of a model statement is copied as written (no substitution).
* A macro is closed by `#}`; `.zmac` is the extension of converted macros
  (`.mac` is found too) and `MNOTE *,` becomes `#note`.

Original design: decided with the user: `&` substitutes
into emitted text (D1), indexing is 0-based everywhere (D2), the macro
language is C-like with braces and free indentation and only has to keep
the *semantics* of HLASM conditional assembly, not its syntax; `hlasm2za`
recovers structure from the sequence-symbol branching.

## 0. What SYS1.MACLIB shows (survey of `maclib_utf8/`)

`/home/knoppix/go-dev/maclib_utf8/` holds 2080 members of SYS1.MACLIB; 1958
define a macro (1984 definitions; the rest are data-area copy members). The
per-feature counts were taken on the first 1656 of them (those whose `MACRO`
statement is in the first 400 lines), so they are slightly low. The table counts
statements (a regular-expression survey over continued, comment-free lines, so the numbers are approximate and a few include words in strings); "macros" is the number of macros that use
the feature. IBM's text is copyrighted: it is used here only to measure, no
macro text is copied into this repository, and the acceptance test of
section 12 reads the directory in place.

| feature | uses | macros | consequence |
|---|---|---|---|
| `AIF` / `AGO` / `ANOP` | 43079 / 12605 / 28678 | 1251 / 1111 / 1253 | the code is flat branches, forward *and backward* (`AGO .POSITIO` re-enters a block above), with sequence symbols like `.ALVL1_A`, and is hard to read. The converter must **recover structure** (if/else, loops, early return, switch; section 10); `#goto` stays only as the last resort. |
| `SETA` / `SETB` / `SETC` | 9934 / 6645 / 20090 | 361 / 380 / 790 | string building dominates |
| `LCLx` / `GBLx` | 5.8k / 2.4k | 1000 / 1145 | many `GBLA &SYSALVL`-style globals that one macro (`SYSSTATE`) sets and the others read: `#global` must keep the first initial value |
| arrays `LCLB &B(5)`, `&B(1) SETB` | 1872 | 418 | lists with indexed `#set` |
| `T'&x` (compared with `'N'` 19236x, `'O'` 3040x, `'U'` 9x) | 22322 | 494 | type of an **argument**; only N (self defining), O (omitted), U |
| `K'&x`, `N'&x` | 76398, 29303 | 500, 605 | length and item count |
| `L'&x` in `AIF` | 17 | 11 | |
| `D'`, `I'`, `S'`, `O'` | **0** | 0 | symbol-table queries at macro time are **not used at all**: milestone M2 (section 9) is not needed for this library |
| substring `'&x'(i,n)` | 56846 | 670 | hottest string operation; `INDEX` 3633 (455 macros) |
| `AND`/`OR`/`NOT` | 39776 | 1050 | |
| `ACTR` | 2162 | 358 | maps to the step limit, not converted |
| `MNOTE` | 5018 | 419 | severities 0-12 plus `MNOTE *,` comments |
| `MEXIT` | 2530 | 383 | |
| `&SYSLIST`, `&SYSNDX`, `&SYSECT`, `&SYSSTYP` (-> `__SECTYPE__`), `&SYSMAC` | 6305, 2563, 46, 39, 6 | 479, 501, 29, 19, 1 | the only real assembler-supplied variables; all other `&SYS...` names seen (`&SYSASCE`, `&SYSSPLV`, `&SYSAM64`, `&SYSALVL`, `&SYSNAME`...) are **ordinary globals set by IBM macros** |
| built-in functions | `BYTE` 78, `UPPER` 27, `DCVAL` 8, `SIGNED` 3 | few | small set needed |
| `&X.` end marker | 54005 | 567 | the converter writes `&x` or `&{x}Y` |
| `AREAD`, `OPSYN`, `PUNCH`, `COPY` | 10, 84, 67, 14 | 1, 2, 1, 5 | converted (`aread()`, kept as model statements, `COPY` inlined from `-copy-dir`) |
| computed `AGO (&I)`, created names `&(...)`, `AINSERT` | 0 | 0 | `AINSERT` -> `#insert`; the others are not needed |
| keyword parameters in the prototype | 1255 | 1254 | positionals and keywords are mixed in almost every macro |

What the code looks like (not quoted; own summary):

* a macro of 100-300 lines is typical (median 281 incl. comments, the largest
  member has 21193 lines), almost all of it conditional assembly around a few
  model lines;
* **the operation field is often a variable** (`&STM  &REG(1),&REG(2),...`
  where `&STM` was set to `'STM'` or `'STMG'`), so expanded text is rescanned;
* arguments are indexed as sublists (`&REG(1)`, `N'&REG`), and substituted
  inside strings and next to dots (`&A.(0,13)`, `CL&A'&E'`);
* macros call other macros (`SYSSTATE`, `IHBERMAC` for messages,
  `IHBINNRR`): **library lookup is required, not optional** (section 7);
* a member may contain non macro text after `MEND` (the PL/S version of the
  macro) and column 73-80 change flags (`@L2C`); the converter stops at
  `MEND` and drops the columns.

Consequences for the plan: M1 plus library lookup plus `hlasm2za` conversion
cover this library; the symbol-table interleaving (M2) is deferred and may
never be needed; control-flow recovery in the converter and a fast interpreter are central (a single
`WTO` or `GETMAIN` expansion runs hundreds of statements).

## 1. Goals

* **Same semantics as HLASM conditional assembly, not the same syntax.** What
  a macro can compute and emit is kept; how it is written is C-like and
  readable: braces, `if/else`, `while`, `for`, `switch`, `return`, named
  variables, free indentation.
* **A converted macro must read like a hand written one.** `hlasm2za` is a
  decompiler for sequence-symbol branching (section 10), not a transliterator.
* One language for macros and open code; `#` marks code lines (as the
  preprocessor already does), `&` substitutes into emitted text (D1).
* A macro is a **statement** (it sits in the operation field), found by name,
  in the source or in a library directory.
* Fast and bounded: bodies are compiled once, the interpreter has no
  recursion for control flow, and runaway loops end with an error.

It lives in the preprocessor stage (`src/pp.c`) and produces the lines the
assembler reads today; object output does not change.

## 2. A first look

A small macro (own example):

```
// STOREREGS (first,last) [,base=13] [,off=20] [,size=4]
// stores registers first..last into a save area
#macro STOREREGS(regs, base = 13, off = 20, size = 4) {
    #if (count(regs) != 2) {
        #error "STOREREGS needs a register range such as (2,5)"
        #return
    #}
    #let first = int(regs[0])
    #let last  = int(regs[1])

    #for (let r = first; r <= last; r++) {
        #let disp = off + (r - first) * size
        #if (size == 8) {
            STG   &r,&disp(,&base)
        #} else {
            ST    &r,&disp(,&base)
        #}
    #}
#}
```

```
         STOREREGS (2,4),size=8        expands to  STG 2,20(,13)
                                                   STG 3,28(,13)
                                                   STG 4,36(,13)
```

The rules in one paragraph. A line whose first non-blank character is `#`
is **macro code**: C-like statements, names without any marker, blocks in
braces (`#if (...) {` ... `#}`), indented as you like. Every other line is a
**model statement**: assembler text that is emitted, with `&name` replaced
by the value of the variable. Labels in model statements end with a colon
(`LOOP&sys.ndx:  DS 0H`) so that the text can be indented. `//` starts a
comment that is never emitted.

A second example shows how arguments steer the emitted code (own example,
the shape is common in system macros):

```
// FILLZ  area,len [,pad=NO]   clear storage, optionally pad with blanks
#macro FILLZ(area, len, pad = "NO") {
    #if (len == "") {
        #error "FILLZ: len is required"
        #return
    #}
    #switch (attr(len, "T")) {
    #case "N":                       // a number: MVC/XC reach 256 bytes
        #if (int(len) <= 256) {
            XC    &area(&len),&area
        #} else {
            #let rest = int(len)
            #let off = 0
            #while (rest > 256) {
                XC    &area+&off(256),&area+&off
                #off += 256
                #rest -= 256
            #}
            XC    &area+&off(&rest),&area+&off
        #}
    #default:                        // a symbol or expression: length known at run time
        LA    1,&area
        LA    0,&len
        XR    15,15
        MVCL  0,14
    #}
    #if (pad == "YES") {
        MVI   &area,C' '
    #}
#}
```

(Parentheses after a name are plain text: `&area(&len)` is the value of
`area` followed by `(`, the length, and `)`; a list element is written with
brackets, `&regs[0]`.)

## 3. Layout and markers

**Macro code lines.** Inside a macro body, and inside any `{ ... }` block
opened by a `#` line, a line whose first non-blank character is `#` is macro
code. At the top level of a file the existing rule stays: `#` in column 1,
followed by a directive keyword (`#include`, `#define`, ...) or by a macro
statement keyword; any other `#NAME` is a label such as `#COUNT`.
The statement keywords are `macro let global if else while for switch case
default break continue return goto label emit note warning error`; an
assignment starts with a variable name (`#n += 1`). Variables named like a
directive or a keyword cannot exist (`#define`, `#if`...).

**Model statements.** Any other non-blank line in a body is a model statement:

```
    [label:]  operation  operands  [remark]
```

* Leading blanks are free (the body is indentable). A label is written with
  a trailing colon; no colon, no label. A non-blank character in column 1 of a
  body line that is not `#`, `//` or `*` is the error ZA4104 ("indent the
  statement or write `label:`"): it is almost certainly a pasted assembler
  line.
* The colon is not part of the label; the emitted line is laid out as
  `label` in column 1, the operation in column 10, the operands after it
  (the remark is kept). A label that expands to nothing is no label.
* A statement continues on the next line when it ends in a comma (as in the
  assembler); continuation lines may be indented.
* `*` as first non-blank character: an assembler comment line, emitted
  (shows in the listing with `print gen`). `//` as first non-blank character,
  or after macro code: a macro comment, never emitted. `/* ... */` is not
  used.
* Macro code continues while parentheses, brackets or a string are open, or
  after a trailing `\`.
* Blank lines are dropped. Tabs count to 8 columns.

**Substitution (D1: `&`).**

| written | meaning |
|---|---|
| `&name` | the value of the variable; the name ends at the first character that is not a letter, digit or `_` |
| `&name[i]`, `&name[i][j]` | element of a list (`i` is an integer literal or a name); `[` has no other meaning in assembler text |
| `&{expr}` | the value of any macro expression: `L&{n+1}`, `&{upper(op)}` |
| `&sys.label` | built-in (section 7); `sys.` names contain a dot |
| `&&` | one literal `&` |

Substitution happens everywhere in a model statement, **also inside quoted
strings** (`C'&text'`), and a name that is not a variable is the error
ZA4110, never literal text. A value inserts as decimal (int), as written
(string), or `(a,b,c)` (list). A reference to an HLASM style `&name.` end marker is not accepted; use
`&{name}x` or put a non-name character after the name.

## 4. Values, names, scope

* **Types:** `int` (64 bit signed; the literals `0`, `-4`, `0x1F`, `X'1F'`,
  `B'101'`, `true`, `false`), `str` (`"text"`, escapes `\"` `\\` `\n` `\xHH`,
  also usable for assembler quotes: `"C'A'"`), `list` (ordered values).
* **Arguments** arrive as text. A text of the form `(a,b,(c,d))` is also a
  list: `count(x)` is 3, `x[0]` is `a`, `x[2][1]` is `d`. Indexing is **0
  based** everywhere (D2). Anything else is a plain string. `int(x)` reads a
  self-defining term (`10`, `X'1F'`, `B'101'`, `C'A'`, `-4`).
* **Truth:** `0`, `""` and the empty list are false; everything else is true.
  Comparison and logic give `1` or `0`.
* **Names** (variables, parameters, macros, functions) are case
  insensitive, like mnemonics; string values keep their case. Assembler
  symbols in model text keep their case rules (they are case sensitive).
* **Scope:** `#let x = v` declares a variable in the **block** where it
  stands (macro body, `if`, loop body...); declaring it twice in the same block
  is an error, hiding an outer one is a warning (ZA4107). A `for` header
  declaration lives in the loop. Parameters live in the macro's outer block.
* **Globals:** `#global name = init` (any macro or open code). The first
  declaration anywhere creates it with `init`; later declarations in other
  macros only name it and ignore their `init`. This is exactly how system
  macros share state (`&SYSALVL` set by one macro, read by the others).
* **Open code:** the same statements work outside macros (file level
  variables), which replaces the conditional assembly of the main source.

## 5. Expressions

C precedence: `[]` `()` calls, unary `- ! ~`, `* / %`, `+ -`, `<< >>`,
`< <= > >=`, `== !=`, `&`, `^`, `|`, `&&`, `||`, `?:`. `&&`/`||` short circuit
and give 0 or 1. Assignment operators `= += -= *= /= %= <<= >>= &= |= ^=`
and `++`/`--` are statements, not expressions.

* `+` joins strings when either side is a string (`"L" + n`), else adds.
  `/` truncates toward zero.
* **Comparison of two strings follows HLASM** (Language Reference, character
  relations): character by character in the EBCDIC order, and **a shorter
  string is always the lesser**; `==`/`!=` are exact. So `"9" < "15"` is true
  and `"B" < "AA"` is true. The library relies on this (`'&C' LE '15'`).
  A string against an int is converted with `int()` first (error if it is
  not a number).
* `x ? a : b`, parentheses, and `in`: `x in list` (true if equal to an
  element).

**Functions** (HLASM equivalent in brackets):

| function | result |
|---|---|
| `len(s)` | characters (`K'`) |
| `count(x)` | list items; 1 for a plain string, 0 for `""` (`N'`) |
| `substr(s, start[, n])` | `n` characters from `start` (0 based); without `n` to the end (`'..'(i,*)`). `start` outside the string or `n` beyond the end is an error (ZA4112), as in HLASM |
| `index(s, t)` | 0 based position of the first `t` in `s`, or -1 (`INDEX`) |
| `upper(s)`, `lower(s)`, `trim(s)`, `repeat(s, n)` | (`UPPER`, `LOWER`, duplication factor) |
| `double(s)`, `quote(s)` | double the `'` and `&` of s; `'s'` with `'` doubled (`DOUBLE`) |
| `byte(n)`, `chars(s)` | one character of code n (`BYTE`); the list of characters |
| `split(s, sep)`, `join(list, sep)`, `list(a, b, ...)` | |
| `int(x)`, `str(n)`, `hex(n, digits)`, `bin(n, digits)` | conversions (`D2X`, `X2D`...) |
| `min`, `max`, `abs` | |
| `empty(x)`, `defined(name)` | `x == ""`; a variable, parameter, `#define` or macro of that name exists |
| `attr(x, "T")` | HLASM attribute of the text `x`: `T` type (`N` self-defining, `O` omitted, `U` anything else), `L` length of a constant, `D` defined (see section 9) |

## 6. Statements

```
#let x = expr                       declare
#x = expr   #x += 1   #x++          assign (also #x[i] = v: a list grows, the gap is "")
#global x = expr

#if (cond) {                        #while (cond) {
    ...                                 ...
#} else if (cond) {                 #}
    ...
#} else {                           #for (let i = 0; i < n; i++) {   ...  #}
    ...                             #for (let r : regs) {            ...  #}
#}                                  #for (let ch : chars(s)) {       ...  #}

#switch (expr) {                    #break   #continue   #return
#case "A", "B":                     #emit expr           emit the text of expr as a line ("\n" splits)
    ...                             #note expr           message, severity info
#case "C":                          #warning expr        message, assembly goes on
    ...                             #error expr          message, assembly fails
#default:                           #goto name  /  #label name     last resort (section 10)
    ...
#}
```

* `if`, `while`, `for`, `switch` take their condition in parentheses and
  open a block with `{` at the end of the line; `#}` closes it; `else` and
  `else if` follow the `}` on the same line (`#} else {`).
* `switch` compares with `==` (strings and ints), has no fall through and no
  need for `break` (a `#break` leaves the switch early); `#case a, b:` lists
  several values; `#default:` is optional.
* `#return` leaves the macro (HLASM `MEXIT`); at the end of the body the macro
  ends by itself (`MEND`).
* `#error`/`#warning`/`#note` take an expression; the text is reported with the
  position of the macro body line and "in expansion of macro X at file:line".
  HLASM `MNOTE` maps: severity 0 and `*` to `#note`, 1-4 to `#warning`,
  5 and more to `#error`.
* `#emit` produces a line of **computed** text, for the rare case that a
  model statement cannot say it (`#emit op + " " + operands`). A model
  statement is always preferred.
* Open code accepts the same statements; the old unbraced preprocessor `#if
  expr` ... `#endif` stays for conditional text (a `#if` that ends in `{` is the
  braced form). `#error text` and `#warning text` outside a macro keep their
  raw text form.
* A loop runs at most `-macro-steps N` statements per expansion in total
  (default 1,000,000, ZA4121): this stands in for HLASM's `ACTR`.

## 7. Macro definition, call, library

```
#macro [name:] NAME(p1, p2 = default, p3, rest..., kw = default) {
    ...
#}
```

* The macro name is an operation code, folded like a mnemonic. A macro
  hides an instruction or directive of the same name (warning ZA4106). A
  later `#macro` of the same name replaces the earlier one (warning).
* Parameters: `p` is positional; `p = expr` is optional with a default
  (any expression, may use earlier parameters); a missing argument without
  default is `""` (testable with `empty(p)`). `rest...` collects the
  remaining positional arguments as a list (HLASM `&SYSLIST(n)` beyond the
  named ones); parameters after it, or after a bare `*`, can only be given by
  keyword (HLASM keyword parameters). Without `rest...` too many positional
  arguments are an error (ZA4102).
* The optional `name:` before the macro name names the **name field** of the
  call: `#macro lbl: SAVE(regs)` ... `&lbl:  DS 0H`. It is also always
  available as `sys.label`.
* **Call syntax is the ordinary statement syntax**: operands split at top
  level commas by the reader's operand scanner, so `C'A,B'`, `(1,2)` and
  `D(X,B)` are each one argument, and an argument is the exact text written
  (after `#define` expansion). `name=value` is a keyword argument; an empty
  argument (`A,,C`) means "use the default". Calls may continue over
  several lines with a trailing comma.
* **Built-ins** (read only): `sys.label` (name field of the call, `""` if
  none; `&NAME`), `sys.ndx` (`"0001"`, `"0002"`..., unique per expansion;
  `&SYSNDX`), `sys.sect` and `sys.sectype` (current control section and its
  kind; `&SYSECT`, `&SYSSTYP`), `sys.macro`, `sys.level` (`&SYSMAC`,
  nesting), `sys.args` (all positional arguments as a list; `&SYSLIST`),
  `sys.argc`; `__FILE__`, `__LINE__` (the call), `__GOFF__`/`__ELF__`,
  `__ASCII__`/`__EBCDIC__`, `__AMODE__`/`__RMODE__` (0, 24, 31 or 64: the
  last `AMODE`/`RMODE` statement read so far) as in the preprocessor.
* **Library macros.** An unknown operation code `FOO` is looked up in the
  macro directories (`-M DIR`, repeatable; the `-I` directories last) as
  `foo.zmac`, then `foo.mac`. The file may hold several `#macro`s; those
  named `FOO` define the call, all are defined. A file is read once per run.
  This is HLASM's library macro search and makes macros that call macros
  (`SAVE` calls `SYSSTATE`) work without `#include`. `hlasm2za -macro-dir
  DIR` converts a directory of members to `.zmac` files.
* Nesting: a macro may call macros and itself (depth limit 64, ZA4120).
  `#macro` inside a macro body is not allowed.
* `#define` stays for token substitution inside operands. A macro is found
  in the operation field only; `#define` names are expanded first.

## 8. Emitted text

* A model statement becomes one source line after substitution; the result
  is **rescanned as source**, so a variable operation field (`&op &a,&b`)
  can name an instruction, a directive or another macro. A line that is only
  blanks is dropped.
* The line keeps the file and line of the model statement; diagnostics add
  "in expansion of macro X at file:line" (the call is the outer position).
  The listing shows the generated lines with `#pragma print gen` (HLASM
  `PRINT GEN`); the default shows the call line only.
* `-E` prints the expansion: every call line followed by the lines it made,
  indented as emitted.

## 9. Kept from HLASM, and the symbol table

Where the semantics matter for the 1958 macros of SYS1.MACLIB (section 0):

* conditions and comparisons behave as section 5 says; `SETB` values are
  0/1 in arithmetic and `"1"`/`"0"` in strings (`str(b)` gives `"1"`);
* `SETA` is 32 bit in HLASM and 64 bit here (differs only on overflow);
* string length limit 4064 as in HLASM is not enforced;
* sublists: `&REG(1)` in HLASM is `reg[0]` (element 0), `N'&REG` is
  `count(reg)`, `K'&REG` is `len(reg)`; an argument that is not a list has
  count 1 (as `N'`), and an omitted one count 0 and `T'` = `O`.

**Symbol table.** Nothing in SYS1.MACLIB asks the assembler for `D'`, `I'`,
`S'` or `O'`, `L'&x` of a symbol appears 17 times and `T'` is only
compared with `N`, `O`, `U`. So `attr()` answers from the argument text only
(`T`: `N` for a self-defining term, `O` for omitted, `U` otherwise; `L`
for a constant); `D`, `I`, `S` of a symbol give the warning ZA4130 and the
"undefined" answer. `sys.sect` is the last `CSECT`/`RSECT`/`DSECT`/`START`/`COM`
name found by a light scan of the lines already read.
An extension M2 (macros expanded inside assembler pass 1, with the real symbol
table) is possible later (`pp.c` becoming a pull interface) and is not
planned.

## 10. Conversion by `hlasm2za`: recovering structure

Only the **semantics** are carried over. Statement by statement:

| HLASM | zosasm |
|---|---|
| `MACRO` / prototype / `MEND` | `#macro NAME(params) {` / `#}`; `&NAME` -> `name:` binding or `sys.label`; `&K=v` -> keyword parameter with default; stops at `MEND`, drops columns 73-80; text after the last `MEND` of a library member (PL/X trailers) is not read |
| `LCLA/B/C &X(n)` | `#let x = 0` / `0` / `""`, `#let x = list()` for arrays |
| `GBLx` | `#global x = 0` / `""` (the first declaration's initial value) |
| `&X SETA e`, `SETB`, `SETC` | `#x = e` (declared with `#let` where first assigned in the block) |
| `AIF`, `AGO`, `ANOP`, `.SEQ`, `MEXIT`, `MEND` reached | structured control flow (below) |
| `AIF ('&X' EQ '')` | `#if (x == "")` ; `AND/OR/NOT` -> `&& \|\| !` |
| `'&X'(i,n)`, `K'&X`, `N'&X`, `T'&X`, `INDEX` | `substr(x, i-1, n)`, `len(x)`, `count(x)`, `attr(x,"T")`, `index(x, ...)` (0-based, `+1-1` folded) |
| `&X`, `&X.`, `&X.Y` in model text | `&x`, `&x`, `&{x}Y`; `&REG(1)` -> `&reg[0]` |
| `&NAME  DS 0H` | `&sys.label:  DS 0H` |
| model statement with label `L1  LA ...` | `L1:  LA ...` (colon) |
| `.* text` comment | `// text` |
| `MNOTE sev,'text'` | `#note`, `#warning`, `#error`; `MNOTE 4, 'text'` (blank after the comma) or an unquoted message: kept as a model statement, HLASM gives ASMA163W and no message (probed on HLASM R6.0) |
| prototype operand that is not `&name[=default]` (`IGWRMRCD ... !`) | ignored with a warning (HLASM: ASMA025S, the macro is still defined) |
| `LCLB @NAME` (no `&`) | read as `&@NAME` with a warning (HLASM accepts it silently) |
| comment line (`*`, `.*`) with a non-blank column 72 | continues over the following records (HLASM rule; `IXGCON` has a `Macro` word in such a continuation) |
| `&SYSNDX`, `&SYSECT`, `&SYSLIST(n)` | `sys.ndx`, `sys.sect`, `sys.args[n-1]` |
| `ACTR` | dropped (the step limit replaces it) |
| `AREAD`, `AINSERT`, `COPY`, `MACRO` inside a macro, `&SYSDATE`...`&SYSPARM` | `aread()`, `#insert`, inlined member, a macro of its own, `sys.*` |
| `OPSYN`, `PUNCH` | kept as model statements (the assembler honours them) |
| created names `&(...)`, `SETAF`, `SETCF`, computed `AGO` | not converted (the macro is commented out and reported) |

**Control flow.** The converter does not translate branch by branch. A
quick feasibility check on the library (a crude reading of `AIF`/`AGO`/`MEXIT`
at statement level, so only indicative): of 1958 macros, **585 (30%) have a
backward branch** (a loop), and **21 (1%) have an irreducible flow graph**
(the smallest are `IGGCSIF`, `QEDIT`, `STCKSYNC`, `IHADVA`, `TGET`); the other
99% can be written with `if`/`while`/`break`/`return` only. The converter



1. cuts the body into basic blocks at sequence symbols, `AIF`, `AGO`, `MEXIT`
   and builds the flow graph (a `.SEQ ANOP` with one predecessor is merged away);
2. removes unreachable blocks and `ACTR`;
3. copies small **exit blocks** (a message call plus `MEXIT`, such as
   `.E1  IHBERMAC 28,303 / MEXIT`) into every branch to them, so error
   exits become `#if (cond) { ...; #return }` guard clauses at the top;
4. finds loops (natural loops, a back edge to a dominating header) and writes
   `#while (cond)` when the header tests the exit first, otherwise
   `#while (true) { ... #if (!cond) { #break } ... }`; counted loops (`i`
   initialised, tested, stepped by a constant) are written as `#for`;
5. finds `if` / `else` by branch and join (the immediate post dominator is the
   end of the construct; a branch that only leaves the construct
   becomes a `#break`/`#continue`/`#return` without `else`);
6. writes a chain of `AIF` tests that jump to blocks which join again after
   them as `#if` / `#else if` / `#else`, and a chain of three or more arms that
   all compare one variable with constants (`==` or `||` of them) as `#switch`
   (`src/mpost.c`, only when no arm has a `#break`/`#continue` and no value
   repeats);
7. for what is still unstructured (several entries into a block): a
   **dispatch loop**, `#let pc = "(start)"` plus `#while (true) { #switch (pc)
   { #case "label": ... } }` (the variable is `pc`, `pc_` ... when the macro
   has such a name); `-keep-goto` keeps `#goto`/`#label` for those jumps
   instead (and writes the macro flat when a `#goto` would enter a block);
8. negates conditions the readable way (`!(a == b)` -> `a != b`, De Morgan),
   lower-cases names, keeps `.*` comments as `//`, indents by block.

Each conversion is checked mechanically: `hlasm2za -flat` writes the plain
branch version (`#goto`/`#label` for every sequence symbol) and the
tests expand the flat and the structured version of a macro for many argument
sets and require **identical output**. The quality measure is the number of
`#goto` and `pc` fallbacks over the library (goal: none for reducible
macros). The numbers of the library run are in DESIGN.md 16.1.

## 11. Diagnostics (ZA41xx, new block)

| code | meaning |
|---|---|
| ZA4100 | macro syntax: unclosed `{`, `#macro` inside a body, bad statement |
| ZA4101 | unknown or duplicate parameter / keyword in a call |
| ZA4102 | too many or too few arguments |
| ZA4103 | argument text is unbalanced (quote, parenthesis) |
| ZA4104 | model statement in column 1 of a macro body without `#`, `//`, `*` |
| ZA4106 | macro hides an instruction (warning) |
| ZA4107 | variable hides an outer one (warning) |
| ZA4110 | `&name` with no such variable |
| ZA4111 | unknown identifier in a macro expression |
| ZA4112 | type or range error (index, `substr`, `int()` of text, division by 0) |
| ZA4113 | `#goto` to an unknown label, or into a block |
| ZA4120 | macro nesting too deep |
| ZA4121 | step limit reached |
| ZA4130 | attribute of a symbol is not available at macro time (warning) |

Each message points at the macro body line and carries "in expansion of macro X
at file:line".

## 12. Plan and tests

1. **M1: the language** (`src/macro.c`, `macro.h`; hooks in `pp.c`): lexer
   and parser for macro code (line oriented, braces), compile to a flat
   statement array with jumps, evaluator with the value types and functions,
   argument splitting (reusing `split_operands`), model statement
   substitution and layout, `-M` library lookup, built-ins, `-E`
   expansion, diagnostics. Tests (`tests/test_macro.c`): every statement and
   function, scoping, globals, keyword/default/variadic calls, nested
   macros, string comparison rule, step limit, error cases, an expression
   fuzz loop.
2. **C1: `hlasm2za`** (`src/convert.c`, a new `src/flow.c`): statement
   translation, flow graph and structuring (section 10), `-flat`,
   `-macro-dir`. Tests: the flat/structured equivalence on generated argument
   sets; hand made macros with each idiom (forward skip, backward loop,
   join, shared error exit, `AIF` chain on one variable); acceptance run
   over the 1958 macros of `maclib_utf8` (local, not in the repository:
   `ZASM_MACLIB=/home/knoppix/go-dev/maclib_utf8 make test-maclib`): all
   convert, all compile, `#goto` count reported, a selected set
   (`SAVE`, `RETURN`, `ABEND`, a keyword heavy one) expanded and read by a
   human against the HLASM source (**no HLASM is available** to compare
   with).
3. Guide chapter, listing `PRINT GEN`, `-E` polish.

Decided: D1 `&` as the substitution marker; D2 0-based indexing, `-1`
adjustments by the converter. Remaining questions: file extension `.zmac`
for converted macros, and whether `MNOTE *,` (237 uses, comment-like) should
become `#note` (current plan: yes).
