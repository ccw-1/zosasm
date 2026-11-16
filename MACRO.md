# zosasm macro language - design (not implemented)

Status: **design for discussion**. Nothing here exists in the code yet. The
numbers of the decisions (D1, D2, ...) are the points where the user's
choice is needed; the rest follows from them.

## 1. Why, and what it is not

zosasm has a C style preprocessor (`#define`, `#include`, `#if`; DESIGN.md
8, guide 3). It substitutes tokens. It cannot loop, keep a counter, take
a string apart or choose which statements to generate from the *value* of
an argument. Existing HLASM macro libraries do all of that, so converting
them (`hlasm2za` today only comments `MACRO`..`MEND` out) needs a language
with the same power.

The goal is **not** to copy the HLASM macro language. It is a small
C-like language that

* defines *statement macros* (a new "instruction": `LABEL  NAME  operands`),
* computes with integers, strings and lists at assembly time,
* decides, by `#if`/`#while`/`#switch`, which assembler statements to emit,
* can express anything an HLASM macro does, so that `hlasm2za` can convert
  every macro (in the worst case with `#goto`, section 8).

It sits in the preprocessor stage (`src/pp.c`) and produces the same lines
that the assembler reads today. Object output is unchanged.

## 2. Overview by example

An HLASM macro and its form in the new language:

```
         MACRO
&NAME    MOVEA &TO,&FROM,&LEN=,&FILL=NO
         AIF   ('&LEN' NE '').HAVE
&NAME    MVC   &TO.(L'&FROM),&FROM
         AGO   .FILL
.HAVE    ANOP
&NAME    MVC   &TO.(&LEN),&FROM
.FILL    AIF   ('&FILL' EQ 'NO').EXIT
         MVI   &TO,C' '
.EXIT    MEND
```

```
#macro MOVEA(to, from, len = "", fill = "NO")
#if len != ""
&__LABEL__ MVC   &to(&len),&from
#else
&__LABEL__ MVC   &to(L'&from),&from
#endif
#if fill != "NO"
           MVI   &to,C' '
#endif
#endmacro
```

Calls look like any statement, with the operand syntax of zosasm:

```
         MOVEA  AREA,SRC                     positional
LBL      MOVEA  AREA,SRC,len=8,fill=YES      keywords
         MOVEA  to=AREA,from=SRC             keywords may name positionals too
```

Two kinds of line exist inside a macro body:

* a line whose first non-blank character is `#` followed by a keyword is
  **macro code**: it runs when the macro is expanded and uses names without
  any marker (like C);
* every other line is a **model statement**: a source line that is emitted
  after substituting `&name` (section 6).

## 3. The marker: `#` for macro code, `&` for substitution

`#` is already the preprocessor marker (`#include`, `#define`, ...), and the
existing rule stays: a directive is a `#` followed by a keyword from a fixed
list; any other `#...` is an ordinary symbol such as `#COUNT`. The new
keywords extend that list. Inside a macro body (and only there) macro code
may be indented; at the top level `#` must still be in column 1.

New keywords: `macro endmacro let set global if elif else endif while
endwhile for endfor foreach switch case default endswitch break continue
return goto label emit note` (`error` and `warning` exist already).
Because the list is closed, a user symbol called `#LET` in column 1 is the
only thing that stops working; `hlasm2za` can warn about such names.

**D1 - the substitution marker.** Model statements need a way to insert a
value, also *inside quoted strings* (`C'&text'`, `DC A(&n)`). Candidates:

| marker | for | against |
|---|---|---|
| `&name`, `&{expr}` (recommended) | same marker as HLASM, so the model lines of converted macros stay almost as they were; `&` has no other meaning in zosasm source; works inside strings; `&&` = a literal `&` | none really, but it is the HLASM style the user said we need not follow |
| `$name` | no clash with `&` | `$` is a symbol character: `$ABC` is a legal label |
| `#{name}` | one marker for everything | noisy; macro code and model text look alike |
| bare names (C `#define` style) | no marker | every parameter name becomes a keyword of the model text (`n`, `to`, `len` are also symbols and operands); too dangerous in assembler text |

Recommendation: `&`, with two changes from HLASM: the name ends at the first
character that is not a letter, digit or `_`, and `&{expr}` inserts the
value of any macro expression. `&X.` (the HLASM end marker) is not accepted;
`A&{n}B` replaces `A&N.B`. In **macro code** (the `#` lines) names are bare:
`#if len != ""`, no `&`.

## 4. Values, names and scope

**Types.** `int` (64 bit signed), `str` (text) and `list` (ordered values).
There is no declaration syntax; the type is that of the value.

* Arguments arrive as text. A text of the form `(a,b,(c,d))` is also a list:
  `count(x)` is 3, `x[0]` is `a`, `x[2][1]` is `d`. Indexing starts at **0**
  (D2). Everything else is a plain string. `int(x)` reads a self-defining
  term (`10`, `X'1F'`, `B'101'`, `-4`).
* Strings are written `"..."` (the assembler's `'` is not used, so a
  macro string can contain assembler quotes). Escapes: `\"`, `\\`, `\n`,
  `\xHH`.
* `+` joins strings when either side is a string (`"L" + n`), else adds.

**D2 - indexing base.** HLASM counts from 1 (`&SYSLIST(1)`, substrings
`'abc'(1,2)`). A C-like language counts from 0. Recommendation: 0 everywhere;
`hlasm2za` converts constants (`&P(1)` -> `p[0]`) and writes `p[i - 1]` for
variable indexes. The alternative is a language where every conversion is
literal but every C programmer is tripped up.

**Names.** Macro-time names (macros, parameters, variables, functions) are
**case insensitive**, like mnemonics (zosasm symbols stay case sensitive).
This keeps converted HLASM (upper case) and hand written code (`len`)
interchangeable. String *values* keep their case.

**Variables.**

```
#let   n = 0              declare in this expansion (error if it exists)
#set   n = n + 1          assign (error if it does not exist)
#set   t[3] = "x"         list element; the list grows, gap filled with ""
#global count = 0         shared by all macros and open code; the first
                          declaration gives the initial value, later ones
                          (in other macros) only name it
```

Scope is the whole expansion (not the block). Parameters are variables.
Open code (outside a macro) has its own variables with the same statements,
which gives the `LCLA`/`SETA` of conditional assembly in the main source
(DESIGN: no separate mechanism).

**Built-in variables** (read only, in the spelling of the existing
`__FILE__` family):

| name | value | HLASM |
|---|---|---|
| `__LABEL__` | the name field of the call (`""` if none) | `&NAME` |
| `__NDX__` | `"0001"`, `"0002"`...: unique per expansion | `&SYSNDX` |
| `__SECT__` | name of the current control section | `&SYSECT` |
| `__MACRO__`, `__LEVEL__` | macro being expanded, nesting depth (1 = called from open code) | `&SYSMAC`, `&SYSNEST` |
| `__ARGS__` | list of the positional arguments as written | `&SYSLIST` |
| `__ARGC__` | their number | `N'&SYSLIST` |
| `__FILE__`, `__LINE__` ... | as now; inside a macro: where the *call* is | |
| `__GOFF__`/`__ELF__`, `__ASCII__`/`__EBCDIC__` | as now | `&SYSOPT_*` |

## 5. Definition and call

```
#macro NAME(p1, p2 = default, p3, rest...)
   ... body ...
#endmacro
```

* The macro name is an operation code, folded like a mnemonic. A macro
  hides an instruction or directive of the same name (warning ZA4106 once).
  A later `#macro` of the same name replaces it (warning).
* Parameters are listed in order. `= default` (any macro expression, may use
  earlier parameters) makes a parameter optional. A parameter without a
  default that is omitted is `""`; so the type of a missing argument is
  always testable (`#if p3 == ""`).
* `rest...` collects the remaining positional arguments as a list
  (HLASM `&SYSLIST(n)` for n > fixed count). Without it, too many arguments
  are an error (ZA4102).
* **Call syntax is the ordinary statement syntax**: the operands are split
  at top level commas by the reader's operand scanner, so `C'A,B'`, `(1,2)`
  and `D(X,B)` are each one argument. An argument is the exact text written.
  `name=value` (an identifier, `=`, and not the literal `=F'1'`) is a keyword
  argument for the parameter of that name; the rest are positional, in order
  of the parameters not already given by keyword. An empty argument
  (`A,,C`) means "use the default".
* An argument that must be an integer is converted by the macro with `int(x)`;
  there are no parameter types. (HLASM has none either; the converter needs
  none.)
* Macros may call macros (also themselves); depth limit 64 (ZA4120). The
  total number of macro statements executed per expansion is limited
  (`-macro-steps N`, default 1,000,000, ZA4121), so a loop that never ends
  is an error and not a hang.

`#define` stays for token substitution (constants, one-line helpers used
inside operands). A macro is a **statement**, found in the operation field;
`#define` names are found anywhere and expanded first. They do not interfere:
a macro body sees the call's arguments after `#define` expansion.

**Library macros (later).** An unknown operation code `FOO` is looked up as
`foo.zmac` in the `-I` directories (the HLASM library convention: one macro
per member). The file contains one `#macro`. `hlasm2za` writes one file per
converted macro with `-macro-dir DIR`.

## 6. Model statements and `#emit`

```
&__LABEL__ MVC &to(&len),&from              &name  and  &{expr}
           DC  C'&text'                     also inside strings
           DC  C'AT&&T'                     && is one &
           DC  F'&{n * 4}'                  any macro expression
L&{__NDX__} DS 0H                           label made from a built-in
```

* A name must exist (parameter, variable, built-in); `&xyz` with no `xyz` is
  an error (ZA4110), not literal text.
* A value inserts as decimal (int), as written (string) or `(a,b,c)` (list).
* The resulting text is **rescanned as source**: a line made by `&op` may
  be another macro call. A line that expands to nothing but blanks is
  dropped. A line with an empty label field simply starts with blanks.
* Each emitted line keeps the file and line of the model statement and the
  note "in expansion of macro X at file:line" (the diagnostics already have
  this). In the listing the lines appear when `#pragma print gen` is on
  (`PRINT GEN`; the default shows only the call, as HLASM does).
* `#emit expr` emits the text of any expression as a line (`\n` splits it).
  It replaces what HLASM macros do with `AINSERT` and computed statements:

  ```
  #foreach r in regs
  #emit "         ST    " + r + "," + (off + 4 * i) + "(13)"
  #endfor
  ```

## 7. Macro code

```
#if expr / #elif expr / #else / #endif
#switch expr / #case v1, v2 / ... / #default / #endswitch     (no fall through)
#while expr / #endwhile        #break  #continue
#for i = 0 .. n - 1 [step 2] / #endfor                         inclusive range
#foreach x in list / #endfor
#return                         leave this macro                (MEXIT)
#note "text"                    message, severity info          (MNOTE *,)
#warning "text" / #error "text" message, assembly goes on / fails (MNOTE 4 / 8+)
#emit expr                      section 6
#goto name / #label name        section 8
```

`#if` here is the same directive as in the preprocessor: one evaluator.
Expressions: integers, strings, lists; `+ - * / %`, `<< >> & | ^ ~`
(bitwise), `&& || !`, `== != < <= > >=`, `?:`, parentheses, indexing `x[i]`,
function calls. Comparison of two strings is character-wise and case
sensitive (HLASM `EQ`); of a string and an int the string is converted with
`int()` first. A condition is false when it is 0 or `""`.
In open code `#if` keeps the C rule (an unknown identifier is 0, so that
`#if DEBUG` works as it does today); inside a macro an unknown identifier is
an error (ZA4111).

`#error`/`#warning` take a string expression here (`#error "bad register "
+ r`); with a plain unquoted text, outside macros, they keep their present
form (ZA4050/4051). Severities: `#note` info, `#warning` ZA warning,
`#error` error; `hlasm2za` maps `MNOTE n,` (0 note, 1-4 warning, 5+ error).

**Functions** (names case insensitive; HLASM equivalent in brackets):

| function | result |
|---|---|
| `len(s)` | number of characters (`K'`) |
| `count(x)` | number of list items; 1 for a plain string, 0 for `""` (`N'`) |
| `substr(s, start, n)` | `n` characters from `start` (0 based) (`'..'(i,n)`) |
| `index(s, t)` | position of `t` in `s`, or -1 (`INDEX`, `FIND`) |
| `upper(s)`, `lower(s)`, `trim(s)` | (`UPPER`, `LOWER`) |
| `repeat(s, n)` | `n` copies (duplication factor) |
| `split(s, sep)`, `join(list, sep)` | string and list |
| `list(a, b, ...)` | make a list |
| `int(x)`, `str(n)`, `hex(n, digits)`, `bin(n, digits)` | conversions (`X2C`, `D2X`, `C2X`...) |
| `min`, `max`, `abs` | |
| `empty(x)`, `defined(name)` | `x == ""`; a variable, parameter, `#define` or macro is defined |
| `attr(x, "T")` | an HLASM attribute of the **text** of `x`: `T` type, `L` length, `S` scale, `I` integer, `D` defined, `O` operation code, `K`, `N` as above |

`attr()` is where macro code needs the assembler's symbol table; see
section 9.

## 8. Control flow without structure: `#goto`

Any HLASM macro can be converted if the language has an unconditional jump,
because `AIF`/`AGO`/`ANOP` and sequence symbols are exactly that:

```
#label top
#if i > n
#goto done
#endif
 ...
#goto top
#label done
```

`hlasm2za` first tries to recognise the common patterns (a forward `AIF`
skipping lines becomes `#if`, a backward `AIF` to a label above becomes
`#while`, `AGO` over an `ANOP` pair becomes `#else`) and falls back to
`#goto`/`#label` otherwise, so that the conversion **never fails** on
control flow. Rules: labels are local to a macro or to open code, a jump may
leave any block and enter none (it may not jump into a `#while`, `#for`,
`#foreach`, `#switch` or `#if` body; ZA4113). The computed `AGO (&I).A,.B,.C`
becomes a `#switch`.

Implementation note: the body is compiled once into a flat array of
statements with resolved jump targets (`#if` = conditional jump to the next
`#elif`/`#else`/`#endif`, `#while` = jump back), so `#goto`, `#break` and
`#continue` are plain jumps and there is no recursion in the interpreter.

## 9. When the macro needs the assembler's symbols

Most macros decide from their *arguments*. Some ask about symbols the
assembler has already seen: `AIF (D'&X)`, `L'&X`, `T'&X`, or use `&SYSECT`.
The preprocessor runs before the assembler (`pp_run` produces all lines,
then pass 1, then pass 2), so these have to be provided in two steps:

* **M1 (text time).** `attr(x, ...)` answers only from the argument text:
  self-defining terms (`T` = `N` or `H`/`F`..., `L` the byte length of the
  constant), register names, `D` false for every symbol, `T` = `U` (HLASM
  "undefined"). `__SECT__` is the last `CSECT`/`DSECT`/... name found by a
  light scan. A macro that depends on the answer gets a warning (ZA4130)
  "attribute of a symbol is not known at macro time".
* **M2 (pass 1 interleaved).** The expansion happens inside assembler pass
  1, line by line, like HLASM: a statement whose operation code is a macro
  is expanded when pass 1 reaches it, with the symbol table as it is at that
  point (forward references are undefined, as in HLASM), and the generated
  lines are inserted into the line array that pass 2 reads. Open-code
  `#if`/`#while` that use `attr()` are executed the same way. This
  needs `pp.c` to become a pull interface (`pp_next_line`) and the
  assembler's pass 1 loop to ask it for lines; that is the larger change
  and is done after M1.

## 10. Conversion by `hlasm2za` (planned)

| HLASM | zosasm |
|---|---|
| `MACRO` / prototype / `MEND` | `#macro NAME(params)` / `#endmacro`; `&NAME` -> `__LABEL__`; `&K=v` -> `k = "v"`; `&P,&Q` -> `p, q` |
| `LCLA/LCLB/LCLC &X`, `GBLx` | `#let x = 0` / `""`, `#global x = 0` (declared where first assigned if the local scope is simple) |
| `&X SETA expr`, `SETB`, `SETC` | `#set x = expr` (`SETB` -> 0/1, `SETC` strings) |
| `AIF (cond).L`, `AGO .L`, `ANOP`, `.L` | `#if`/`#while`/`#else` when recognised, else `#goto`/`#label` |
| `MEXIT` | `#return` |
| `MNOTE sev,'text'` | `#note`, `#warning`, `#error` |
| `AIF ('&X' EQ '')` | `#if x == ""` |
| `&X`, `&X.`, `&X.Y` in model text | `&x`, `&x`, `&{x}Y` |
| `&&` | `&&` |
| `&P(n)`, `&SYSLIST(n)`, `N'&P`, `K'&P` | `p[n-1]`, `__ARGS__[n-1]`, `count(p)`, `len(p)` |
| `T'&P`, `L'&P` ... in `AIF` | `attr(p, "T")`, `attr(p, "L")` (macro time); `L'&P` inside a model statement stays (the assembler does it) |
| `&SYSNDX`, `&SYSECT`, `&SYSLIST`, `&SYSMAC` | `__NDX__`, `__SECT__`, `__ARGS__`, `__MACRO__` |
| `&(&A)` created names, `&A(&I)` subscripted SET symbols | `#set` on a list; created names are not convertible (reported) |
| `AINSERT`, `AREAD`, macro generating macros, `OPSYN` | reported, left commented |
| `COPY` inside a macro | `#include` at definition time (as HLASM) |

Arithmetic in `SETA` is 32 bit in HLASM and 64 bit here; the difference
shows only on overflow (documented; no conversion needed).

## 11. Diagnostics (ZA41xx, new block)

| code | meaning |
|---|---|
| ZA4100 | macro syntax: definition, unclosed `#macro`/`#if`/`#while`, nested `#macro` |
| ZA4101 | unknown or duplicate parameter / keyword in a call |
| ZA4102 | too many or too few arguments |
| ZA4103 | argument text is unbalanced (quote, parenthesis) |
| ZA4106 | macro hides an instruction (warning) |
| ZA4110 | `&name` with no such variable |
| ZA4111 | unknown identifier in a macro expression |
| ZA4112 | type error (list index out of range, `int()` of text, divide by 0) |
| ZA4113 | `#goto` to an unknown label or into a block |
| ZA4120 | macro nesting too deep |
| ZA4121 | step limit reached |
| ZA4130 | attribute of a symbol is not available at macro time (M1) |

Each message points at the macro body line and carries "in expansion of
macro X at file:line", the call being the outer position.

## 12. Plan and tests

1. **M1: language core** (`src/macro.c`, `macro.h`; hooks in `pp.c`):
   expression parser and evaluator, `#macro` collection, call matching
   (reusing `split_operands`), model-statement substitution, the compiled
   statement array, built-ins, `attr()` over text, diagnostics, open-code
   `#let/#set/#while`. Tests: `tests/test_macro.c` golden expansions per
   construct, error cases, step limit, recursion, keyword/default/variadic
   calls, `#goto`, nested macros, a fuzz loop over the expression parser.
2. **C1: `hlasm2za` conversion** of `MACRO`..`MEND` (patterns, then the
   `#goto` fallback), `-macro-dir`, warnings for what cannot be converted.
   Tests: converted macros expand to what HLASM would produce
   (compared by hand-checked expected output; **no HLASM available to
   check**, so the expectations are interpretations of the Language
   Reference).
3. **M2: interleaved expansion** with the symbol table (section 9); needed before the conversion can be exact for macros that test symbols.
4. Guide chapter, `-E` shows expansions, listing `PRINT GEN`, library
   lookup.

Open points for the user: D1 (`&` marker), D2 (0-based indexing), case
insensitive names (section 4), whether `#note` should exist or `MNOTE *`
becomes a `#warning`, and above all **which real macros must convert** -
a sample (SAVE/RETURN, WTO, a PL/I or LE prologue macro, a table
generator) decides which of M2, C1, the library lookup and the `&SYS...`
variables come first.
