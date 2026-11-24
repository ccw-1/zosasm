# CBLOCK design: inline scalar C expressions and statements in zosasm

> Historical design plan. For current status and user documentation, see
> `docs/CBLOCK.md`. Later work added `switch`, labels, `goto`, byte and typed
> scalar pointers, opaque named pointers, memory intrinsics, and
> `__strlen`/`__memchr`.

Status: capture, integer expressions, control flow including `switch` and
`goto`, byte and typed scalar pointers, opaque named pointers, memory
intrinsics, the `__strlen`/`__memchr` scan intrinsics, and DWARF locals for
`-g` are implemented. This branch is merged and released as 0.4.0.

Implemented so far:

* `#pragma cblock` capture and the pragma validation.
* `cin`, `cout`, `cinout`, `cscratch` and `ctemp`.
* 32-bit and 64-bit integer values, literals, casts and the C-like usual
  arithmetic conversions.
* declarations, scalar locals, assignments and compound assignments;
* unary `+ - ! ~`, arithmetic, bitwise, shifts, comparisons, division and
  remainder;
* a temp-slot stack machine with a computed size check;
* `if`/`else`, `while`, `do`, `for`, `break` and `continue`;
* short-circuit `&&`/`||`, `?:`, prefix and postfix `++`/`--`;
* `switch`/`case`/`default`, labels and `goto`;
* byte pointers (`uint8_t *`, `void *`, `char *`) with assignment, comparison,
  integer byte offsets, pointer difference, `*p`, `p[i]` and `&local`;
* `__memcpy`, `__memmove` and `__memset` with constant and runtime sizes;
* `__strlen` and `__memchr` expression intrinsics;
* generated assembly lines and the `ZA5xxx` diagnostics;
* `test_cblock` and the optional qemu runtime suite (`make
  test-cblock-runtime`).

Not implemented yet: pointer-to-pointer types, `p->field`, arrays,
`SRST`-based string and byte scans, a faster chunked runtime `memset` and
`memmove`, nested CBLOCK scopes, and C source line mapping.

## 1. Goal

Make complex integer expressions and short control-flow sequences easier to
write and review, without making zosasm a general C compiler, a linker, or a
runtime library.

A CBLOCK is inline code. It is not a function, it is not called, and it does
not create an entry label. It executes at the point where it appears and
falls through to the next assembler statement. It reads and writes named
registers, uses an optional caller-provided temporary area, and compiles to
ordinary assembler statements. Those statements go through the normal
assembler passes, so labels, `USING`, listing, ZSYM and object emission work
unchanged.

The primary user is a person or agent writing assembly who wants to express
arithmetic and branching clearly. The block is a convenience between a macro
and a hand-written instruction sequence, not a separately callable routine.

## 2. Non-goals for the first version

* general pointers, arrays, structures, unions, strings, and memory access
  beyond byte addresses;
* floating point;
* function calls and recursion;
* callable CBLOCK entry points;
* external symbols, relocations or literal pools from CBLOCK code;
* a C preprocessor inside a CBLOCK;
* global variables;
* a full C type system;
* optimized code.

`switch`, `goto`, byte pointers and helper blocks were added after the first
version.

## 3. Source syntax

A CBLOCK starts with `#pragma cblock`, carries a register and temporary-area
contract in `#pragma` lines, starts its body at `#pragma cbegin`, and ends at
`#pragma cend`.

```asm
        L      %r2,X
        L      %r3,Y
#pragma cblock squares
#pragma cin     int32_t a=%r2, int32_t b=%r3
#pragma cout    int32_t c=%r4
#pragma cscratch %r0, %r1, %r5
#pragma ctemp   %r12, 64, down
#pragma cbegin
    c = a*a + b*b;
#pragma cend
        ST     %r4,Z
```

The optional name after `cblock` is for diagnostics and the listing only. It
does not define a symbol. Put a normal assembler label before the block when
the code needs a branch target.

The preprocessor captures the block before it processes comments, `#` lines
or continuations. The C lexer handles `//` and `/* */` itself. A `#pragma`
line inside a body is an error except `cend`.

The pragmas are:

| pragma | meaning |
|---|---|
| `cin type name=%rN,...` | variable read from a register; the block may also write it unless declared with `cout` |
| `cout type name=%rN,...` | variable written to a register; the block must assign it before reading it |
| `cinout type name=%rN,...` | variable read from and written to the same register |
| `cscratch %rN,...` | registers the generated code may use and clobber |
| `ctemp %rN,SIZE,DIR` | caller-provided scalar temporary area, size in bytes, `up` or `down` |
| `ctemp` omitted | the compiler may use only bound registers and `cscratch`; no spills |

`cscratch` accepts ranges, for example `%r5-%r7`. A bound register may not
appear in `cscratch`. The temp base register may not be `%r0` because `%r0`
cannot be a base register in many address forms. The temp base register is
preserved by the generated code.

The compiler reports the temp area it actually needs. It is a diagnostic if
the declared size is smaller. A block with no `ctemp` produces a diagnostic
when an expression needs a spill.

`cinout` is the common case for a register that already holds a value and
should hold the updated value afterwards. `cout` is for a fresh result. A
`cout` variable uses the register only at the point where the C code assigns
it.

## 4. Language semantics

### 4.1 Types

Only these scalar types are accepted in the first version:

* `int32_t`, `uint32_t`
* `int64_t`, `uint64_t`
* `int` and `unsigned` as aliases for the 32-bit types

Literals are decimal, hexadecimal (`0x`) or binary (`0b`), with C integer
suffixes for unsigned and 64-bit values. Explicit integer casts are allowed.

The usual arithmetic conversions apply in a documented, C-like way: promote
to 64 bits when either side is 64 bits, otherwise to 32 bits; an unsigned
operand at the same width wins. Comparisons use the converted type.

### 4.2 Deterministic integer behavior

The language defines behavior that C leaves implementation-defined or
undefined:

* signed overflow wraps two's complement, like `-fwrapv`;
* division or remainder by a constant zero is a compile-time error;
* division or remainder by a runtime zero traps at run time, as the machine
  instruction does;
* a constant shift count outside `0..width-1` is a compile-time error;
* a variable shift count is masked to the operand width;
* an unbound scalar local without an initializer is zero-initialized. This is
  an extension to C and is stated in the guide;
* a `cout` variable must be assigned before it is read. The type checker
  rejects an obvious read before assignment;
* comparisons produce `0` or `1` in the result type;
* `&&`, `||`, the ternary operator and assignments evaluate in the usual C
  order. Operands with side effects are only variables and `++`/`--` in the
  first version.

### 4.3 Statements and expressions

Statements:

* declarations with or without an initializer;
* expression statements;
* assignment and compound assignment;
* `if`, `else`, `while`, `for`, `do`, `break`, `continue`;
* nested blocks and scalar locals.

`return` is not allowed. A CBLOCK has no function boundary. The block ends at
`#pragma cend` and falls through to the next statement. `break` and
`continue` apply only to loops inside the block.

Expressions:

* unary `+ - ! ~`, prefix and postfix `++` and `--`;
* `+ - * / %`;
* `<< >>`;
* `< <= > >= == !=`;
* `& ^ |`;
* `&& ||`;
* `?:`;
* casts;
* parentheses.

The parser rejects anything outside this set with a C source location and a
help line.

### 4.4 Variables and scope

The `cin`, `cout` and `cinout` pragmas declare variables that are bound to
physical registers. Their type is written in the pragma. The body uses them
without redeclaration.

Locals declared in the body are block scoped and have a fixed slot in the
temp area, or a register in a later optimization. The first version assigns
every local a slot. A variable declared inside a loop is reinitialized on
entry to its block.

Labels generated for `if` and loops are private to the block and appear in
the listing. They use a reserved prefix based on the block name or line
number.

## 5. Register contract and generated code

The block has a contract with the surrounding code, not a call ABI:

* `cin` variables contain their register value at block entry;
* `cout` and `cinout` variables contain their final value at block exit;
* the block preserves every register not in `cscratch` and not bound by
  `cout`/`cinout`;
* bound registers are never used as scratch;
* no save area is created, no stack pointer is changed, and no call is made;
* the temporary area is caller-owned and the block only loads and stores
  fixed slots in it;
* the block may use the condition code and leaves it undefined at the end.

Illustrative output for the multiplication example:

```asm
        * cblock squares
        MSR   %r2,%r2
        MSR   %r3,%r3
        AR    %r4,%r2
        AR    %r4,%r3
        * end cblock squares
```

In practice the generator evaluates `a*a`, then `b*b`, and combines the
results with the available scratch registers. The exact sequence depends on
the expression tree and the register pool.

The listing shows the original C line as a generated note and then the
instructions it produced:

```text
        CBLOCK squares, C source at demo.asm:12
12      c = a*a + b*b;
        MSR       %r5,%r2
        MSR       %r6,%r3
        AR        %r5,%r6
        LR        %r4,%r5
        * cblock temp used: 0 of 64 bytes
```

`-disasm` shows the generated assembly, not the C source. ZSYM stores the
generated statements like any other statements. `-g` initially has no C
variable types; line mapping to C lines is a later phase.

If a `cout` variable is assigned several times, only the final value must be
in the register at block exit. The generator may keep it in a temp slot and
move it at the end, unless it is cheaper to keep it in the bound register.

## 6. Compiler architecture

Add one new translation unit, `src/cblock.c`, with a small header
`src/cblock.h`. It runs in the preprocessor stage and produces assembler
source lines.

Pipeline:

```text
#pragma cblock lines
    |
    v
capture raw C lines (pp.c)
    |
    v
C lexer -> parser -> typed AST      (cblock.c)
    |
    v
control-flow lowering -> simple IR  (cblock.c)
    |
    v
register and temp-slot allocation   (cblock.c)
    |
    v
instruction selection -> assembler text
    |
    v
SrcLine stream -> read_statements -> pass 1 / pass 2
```

The compiler does not write GOFF or ELF. It only emits statements that the
existing assembler consumes.

### 6.1 Front end

The lexer and parser handle the subset directly. The AST keeps source spans
so every diagnostic names the C file, line and column.

The type checker resolves integer types, inserts explicit conversions, checks
`cout` assignment before use, and rejects mixed operations outside the
subset.

### 6.2 IR and code generation

Lower the AST to a small three address form:

* `const`, `var`, `temp`;
* unary and binary operations;
* `load`, `store`;
* `label`, `branch`, `cond_branch`;
* `end` for the implicit fall through.

Control flow becomes explicit labels and branches. Short-circuit operators
and the ternary operator become conditional branches.

The first code generator is deliberately simple:

* every local lives in a temp slot;
* bound registers are loaded and stored only when the C code uses them;
* every expression evaluates left to right into `cscratch` registers;
* an expression may spill to a temp slot when the scratch registers are
  exhausted;
* no register is kept live across statements except bound variables that the
  generator chooses to keep in their register;
* the generated code falls through to the next assembler statement.

This produces verbose but predictable code. Holding locals in registers is a
later optimization.

### 6.3 Instruction selection

Use the existing ISA table for names and encodings, but emit textual
instructions in the usual zosasm form. The initial set is:

* load and store: `L`, `LG`, `ST`, `STG`, `LR`, `LGR`;
* integer arithmetic: `AR`, `AGR`, `SR`, `SGR`, `MR`, `MSR`, `MSGR`, `DR`,
  `DSGR`;
* bitwise: `NR`, `NGR`, `OR`, `OGR`, `XR`, `XGR`;
* shifts: `SLL`, `SRL`, `SLA`, `SRA`, `SLLG`, `SRLG`, `SLAG`, `SRAG`;
* comparisons and branches: `CR`, `CGR`, `CLR`, `CLGR`, `BRC`/`BRCL`;
* constants: `LHI`, `LGHI`, `LGFI`, and `IIHF`/`IILF` for 64-bit constants.

The exact set is reviewed during implementation. The generator must not use
instructions that depend on a literal pool or external relocation.

### 6.4 Temp slots

The temp allocator assigns aligned slots for locals and spills. It tracks the
highest slot used and reports the required size in the listing. The direction
only changes the offset calculation:

* `down`: slot `n` starts at `size - n - width`;
* `up`: slot `n` starts at `n`.

The compiler refuses a block when the required size is larger than the
declared size, or when a spill is needed and no `ctemp` exists.

## 7. Integration points

### 7.1 Preprocessor

`pp.c` recognizes `#pragma cblock` at the start of a line before comment and
macro processing. It captures lines until `#pragma cend`, keeping their
original line numbers. It calls `cblock_compile()` and emits the returned
assembler lines into the normal output stream.

### 7.2 Source and listing

`SrcLine` gains a generated flag and an optional C source pointer. Generated
lines carry the original C line number and text. `listing.c` prints the C
line as a generated note and the assembler line below it. Diagnostics use the
C span.

If the structured listing format is too invasive for the first version, emit
`* C: ...` comment lines before each generated group. The final design should
use the structured generated lines.

### 7.3 ZSYM, disassembly and debug information

Generated statements are ordinary statements, so ZSYM and `-disasm` work
without special cases. `-disasm` prints assembly instructions, not C.
`-g` can map the generated instructions to the C line in a later phase. C
variable types and locations are out of scope for the first version.

### 7.4 MCP and CLI

Both use the same preprocessor, so no new MCP tool is needed. The guide and
tool descriptions mention that `source` may contain a `#pragma cblock`.

`-E` prints the generated assembler, which is useful for debugging the
compiler.

## 8. Diagnostics

Reserve `ZA5xxx` for CBLOCK diagnostics:

| code | meaning |
|---|---|
| ZA5001 | C syntax error |
| ZA5002 | unsupported type or operation |
| ZA5003 | type mismatch or invalid cast |
| ZA5004 | undeclared variable |
| ZA5005 | invalid register binding or scratch list |
| ZA5006 | temp area too small, or spill without `ctemp` |
| ZA5007 | constant division by zero or invalid shift |
| ZA5008 | unsupported statement, including `return` |
| ZA5009 | internal compiler error, with a note to report it |

Every diagnostic points at the C source. A note points at the `#pragma
cblock` line and shows the block name or source line.

## 9. Testing

### 9.1 Unit tests

`tests/test_cblock.c` checks:

* lexer and parser acceptance and rejection;
* type checking and constant folding;
* register binding, `cin`/`cout`/`cinout` and `cscratch` validation;
* golden assembler text for expressions and control flow;
* temp slot allocation and size checks;
* diagnostic codes and source positions;
* hostile input: deep nesting, very long identifiers, huge constants,
  unbalanced braces, unterminated comments and strings.

### 9.2 Assembler tests

Compile CBLOCK probes with `zosasm -gz0 -o` and check the object with
`-dump`. Compare the generated assembly with hand-written expected sequences
where the sequence is stable.

For GOFF correctness, write an equivalent hand-written HLASM probe and
compare the generated object structure with `tools/oracle_obj.py`. Put the
block inside a small assembly routine that loads the input registers and
stores the output registers. This checks the encoded instructions and the
register contract, not the C source.

### 9.3 Runtime tests

Compile a CBLOCK to ELF with `-elf`, wrap it in a small assembly driver that
loads the input registers and stores the output registers, link it with a C
main and run it under `qemu-s390x`. Use a C reference evaluator or clang on
the same integer expressions as the oracle for values and edge cases.

The implemented target is `make test-cblock-runtime`. It is optional when the
cross toolchain is absent; CI can install `gcc-s390x-linux-gnu` and
`qemu-user` or skip the target.

For z/OS, a small pok56 probe can run one block with a few inputs and print
the result. This is useful once the register contract is stable, but the ELF
path gives faster feedback.

### 9.4 Fuzzing

Add a CBLOCK mutation fuzzer to the existing ASan/UBSan runs. It must produce
a diagnostic, not a crash, hang or out-of-memory exit.

## 10. Milestones

### M0: skeleton and capture

* Define `#pragma cblock`, `cin`, `cout`, `cinout`, `cscratch`, `ctemp`,
  `cbegin`, `cend`.
* Capture the block in `pp.c` and reject malformed pragmas.
* Add `src/cblock.c` with an empty compiler that returns an error.
* Tests for capture and diagnostics.

### M1: expressions

* Lexer, parser and type checker for the integer expression subset.
* Bound variables and unbound locals with initializers.
* Simple register evaluation with spills to `ctemp`.
* Golden assembler output and parser/type tests.
* Listing of generated lines as comments.

### M2: control flow

* `if`, `else`, `while`, `for`, `do`, `break`, `continue`.
* Blocks and scopes.
* Branch labels and `BRCL`.
* Temp slot reuse.
* Runtime tests under qemu.

### M3: integration and documentation

* Structured generated source lines in the listing.
* ZSYM and `-disasm` checks.
* `docs/CBLOCK.md`, GUIDE chapter, DESIGN section, README example,
  CHANGELOG.
* Fuzzer and ASan/UBSan validation.

### M4: optional extensions

* `switch`, `goto`, labels, byte pointers, memory intrinsics and scan
  intrinsics were implemented after the first version;
* holding locals in registers;
* C line and variable information in DWARF;
* helper blocks called from another CBLOCK.

Each extension gets its own design note and tests.

## 11. Risks

* **Scope creep into C.** Keep the type set and statement set closed. Reject
  pointers, arrays, calls and floats in the first version.
* **Register binding bugs.** The block must never use a bound register as
  scratch and must preserve registers outside the contract. Test every
  binding direction, including `%r0` rejection for `ctemp`.
* **Temp area accounting.** Compute the exact requirement and make the
  listing show it. A wrong slot offset is a hard-to-find runtime failure.
* **Listing integration.** Keep generated source lines separate from user
  source, and keep ZSYM/disassembly behavior explicit.
* **Branch range.** Start with `BRCL` and only use `BRC` after checking the
  range.
* **Hostile input.** The parser is a new attack surface. Use the same caps and
  fuzzing discipline as the macro language.
* **Instruction selection errors.** Use the ISA table, write golden byte
  tests, and cross-check with hand-written HLASM where possible.

## 12. Open decisions

1. Whether the interface uses `cin`/`cout`/`cinout` or one `cbind` pragma
   with a direction word.
2. Whether bound variables are declared in the pragma (as above) or in the C
   body with a separate binding pragma.
3. Whether unbound locals default to zero or the compiler requires an
   initializer.
4. Whether `int` and `unsigned` should be aliases for 32-bit types or
   rejected in favor of fixed-width names only.
5. Whether the first version includes `do` and `for`, or only `while`.
6. Whether the first version emits `BRC` with range checks or always `BRCL`.
7. Whether the listing shows C lines as structured notes or as `*` comment
   lines.
8. Whether a block may declare `cin`/`cout` aliases for the same register.

The recommended answers for the first version are: `cin`/`cout`/`cinout`,
fixed-width types plus 32-bit `int`/`unsigned`, zero-initialized unbound
locals, `while` and `for`, always `BRCL`, and one CBLOCK per source line.
