# CBLOCK plan: scalar C expressions and statements in zosasm

Status: design proposal, not implemented.

## 1. Goal

Make complex integer expressions and short control-flow sequences easier to
write and review, without making zosasm a general C compiler, a linker, or a
runtime library.

A CBLOCK is a leaf code block written in a small C subset. zosasm compiles it
to ordinary assembler statements. The statements go through the normal
assembler passes, so labels, `USING`, listing, ZSYM and object emission work
unchanged. The listing shows the C source next to the generated instructions.

The primary user is a person or agent writing assembly who wants to express
arithmetic and branching clearly. Performance is secondary to predictable
code and a readable listing.

## 2. Non-goals for the first version

* pointers, arrays, structures, unions, strings and memory access;
* floating point;
* function calls and recursion;
* external symbols, relocations or literal pools from CBLOCK code;
* a C preprocessor inside a CBLOCK;
* global variables;
* a full C type system;
* optimized code. Correct, simple code comes first.

`switch`, `goto` and inlining of helper blocks can come later if the first
version is useful.

## 3. Source syntax

A CBLOCK starts with `#pragma cblock`, carries ABI metadata in `#pragma`
lines, starts its body at `#pragma cbegin`, and ends at `#pragma cend`.

```asm
#pragma cblock int32_t clamp(int32_t x, int32_t lo, int32_t hi)
#pragma cargs    x=%r2, lo=%r3, hi=%r4
#pragma cresult  %r2
#pragma cscratch %r0, %r1, %r5, %r14
#pragma ctemp    %r12, 64, down
#pragma cbegin
    int32_t t = x;
    if (t < lo) t = lo;
    if (t > hi) t = hi;
    return t;
#pragma cend
```

The function name becomes the entry label. Internal labels use a reserved
prefix, for example `clamp$c1`, and cannot collide with user symbols.

The preprocessor captures the block before it processes comments, `#` lines
or continuations. The C lexer handles `//` and `/* */` itself. A `#pragma`
line inside a body is an error except `cend`.

The ABI pragmas are:

| pragma | meaning |
|---|---|
| `cargs name=%rN,...` | parameter name and its entry register |
| `cresult %rN` | register that holds the result |
| `cscratch %rN,...` | registers the generated code may use and clobber |
| `ctemp %rN,SIZE,DIR` | caller-provided scalar temporary area, size in bytes, `up` or `down` |
| `ctemp` omitted | the compiler may use only `cscratch` registers and must not spill |

`cscratch` accepts ranges, for example `%r5-%r7`. The temp base register
cannot be `%r0` because `%r0` cannot be a base register in many address forms.
The temp base register is preserved by the generated code.

The compiler reports the temp area it actually needs. It is a diagnostic if
the declared size is smaller. A block with no `ctemp` produces a diagnostic
when an expression needs a spill.

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
* a scalar local without an initializer is zero-initialized. This is an
  extension to C and is stated in the guide;
* comparisons produce `0` or `1` in the result type;
* `&&`, `||`, the ternary operator and assignments evaluate in the usual C
  order. Operands with side effects are only variables and `++`/`--` in the
  first version.

### 4.3 Statements and expressions

Statements:

* declarations with or without an initializer;
* expression statements;
* assignment and compound assignment;
* `if`, `else`, `while`, `for`, `do`, `break`, `continue`, `return`;
* nested blocks and scalar locals.

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

Locals are block scoped and have a fixed slot in the temp area, or a register
in a later optimization. The first version assigns every local a slot. A
variable declared inside a loop is reinitialized on entry to its block.

Labels generated for `if`, loops and `return` are private to the block and
appear in the listing.

## 5. ABI and generated code

The generated code is leaf code:

* arguments arrive in the registers named by `cargs`;
* the result is left in the register named by `cresult`;
* `%r14` holds the return address; the block returns with `BR %r14`;
* the block preserves every register not in `cscratch` and preserves the
  `ctemp` base register;
* no save area is created, no stack pointer is changed, and no call is made;
* the temp area is a caller-owned scalar frame. The block only loads and
  stores fixed slots in it.

Illustrative output for the clamp block:

```asm
CLAMP     DS    0H
         LR    %r5,%r2
         CR    %r5,%r3
         BRCL  4,CLAMP$C1
         LR    %r5,%r3
CLAMP$C1 CR    %r5,%r4
         BRCL  2,CLAMP$C2
         LR    %r5,%r4
CLAMP$C2 LR    %r2,%r5
         BR    %r14
```

The exact mnemonics and condition masks are code generator decisions. The
generator uses `BRCL` first for a simple and safe branch range, then can
switch to `BRC` when the block is small.

The listing shows the original C line as a generated note and then the
instructions it produced:

```text
        CBLOCK clamp, C source at demo.asm:10
10      int32_t t = x;
        CLAMP     DS    0H
        LR        %r5,%r2
10      if (t < lo) t = lo;
        CR        %r5,%r3
        BRCL      4,CLAMP$C1
        LR        %r5,%r3
        ...
        * cblock temp used: 0 of 64 bytes
```

`-disasm` shows the generated assembly, not the C source. ZSYM stores the
generated statements like any other statements. `-g` initially has no C
variable types; line mapping to C lines is a later phase.

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

The type checker resolves integer types, inserts explicit conversions and
rejects mixed operations outside the subset.

### 6.2 IR and code generation

Lower the AST to a small three address form:

* `const`, `var`, `temp`;
* unary and binary operations;
* `load`, `store`;
* `label`, `branch`, `cond_branch`, `return`.

Control flow becomes explicit labels and branches. Short-circuit operators
and the ternary operator become conditional branches.

The first code generator is deliberately simple:

* every local lives in a temp slot;
* every expression evaluates left to right into `cscratch` registers;
* an expression may spill to a temp slot when the scratch registers are
  exhausted;
* the result is loaded from a temp slot into the result register at
  `return`;
* no register is kept live across statements.

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

If the listing format is too invasive for the first version, emit `* C: ...`
comment lines before each generated group. The final design should use the
structured generated lines.

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
| ZA5005 | invalid register mapping or scratch list |
| ZA5006 | temp area too small, or spill without `ctemp` |
| ZA5007 | constant division by zero or invalid shift |
| ZA5008 | unsupported statement |
| ZA5009 | internal compiler error, with a note to report it |

Every diagnostic points at the C source. A note points at the `#pragma
cblock` line and shows the entry name.

## 9. Testing

### 9.1 Unit tests

`tests/test_cblock.c` checks:

* lexer and parser acceptance and rejection;
* type checking and constant folding;
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
compare the generated object structure with `tools/oracle_obj.py`. This
checks the ABI and the encoded instructions, not the C source.

### 9.3 Runtime tests

Compile a CBLOCK to ELF with `-elf`, link it with a small assembly driver and
run it under `qemu-s390x`. Use a C reference evaluator or clang on the same
integer expressions as the oracle for values and edge cases.

Add a `make test-cblock-qemu` target for this. It is optional when the
cross toolchain is absent; CI can install `gcc-s390x-linux-gnu` and
`qemu-user` or skip the target.

For z/OS, a small pok56 probe can call one generated function with a few
arguments and print the result. This is useful once the ABI is stable, but
the ELF path gives faster feedback.

### 9.4 Fuzzing

Add a CBLOCK mutation fuzzer to the existing ASan/UBSan runs. It must produce
a diagnostic, not a crash, hang or out-of-memory exit.

## 10. Milestones

### M0: skeleton and capture

* Define `#pragma cblock`, `cargs`, `cresult`, `cscratch`, `ctemp`, `cbegin`,
  `cend`.
* Capture the block in `pp.c` and reject malformed pragmas.
* Add `src/cblock.c` with an empty compiler that returns an error.
* Tests for capture and diagnostics.

### M1: expressions

* Lexer, parser and type checker for the integer expression subset.
* Locals with initializers, assignments and `return`.
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

* `switch`;
* holding locals in registers;
* C line and variable information in DWARF;
* `goto`;
* helper blocks called from another CBLOCK.

Each extension gets its own design note and tests.

## 11. Risks

* **Scope creep into C.** Keep the type set and statement set closed. Reject
  pointers, arrays, calls and floats in the first version.
* **Register allocation bugs.** The simple stack of scratch registers with
  explicit spills is easier to test than an optimizer. Do not optimize until
  the semantics and the ABI tests are stable.
* **ABI misuse.** The generated code must preserve the declared registers and
  the temp base. Test every register mapping, including `%r0` rejection.
* **Temp area accounting.** Compute the exact requirement and make the
  listing show it. A wrong slot offset is a hard-to-find runtime failure.
* **Listing integration.** Keep the generated source lines separate from
  user source, and keep ZSYM/disassembly behavior explicit.
* **Branch range.** Start with `BRCL` and only use `BRC` after checking the
  range.
* **Hostile input.** The parser is a new attack surface. Use the same caps and
  fuzzing discipline as the macro language.
* **Instruction selection errors.** Use the ISA table, write golden byte
  tests, and cross-check with hand-written HLASM where possible.

## 12. Open decisions

1. Exact pragma spelling: `cargs`, `cresult`, `cscratch`, `ctemp` as above,
   or a single `#pragma cabi ...` line.
2. Whether the function signature is part of the `cblock` line or supplied by
   `cargs`/`cresult` only.
3. Whether locals default to zero or the compiler requires an initializer.
4. Whether `int` and `unsigned` should be aliases for 32-bit types or
   rejected in favor of fixed-width names only.
5. Whether the first version includes `do` and `for`, or only `while`.
6. Whether the first version emits `BRC` with range checks or always `BRCL`.
7. Whether the listing shows C lines as structured notes or as `*` comment
   lines.
8. Whether a CBLOCK may define more than one entry function in one block.

The recommended answers for the first version are: the syntax in section 3,
fixed-width types plus 32-bit `int`/`unsigned`, zero-initialized locals,
`while` and `for`, always `BRCL`, and one entry function per block.
