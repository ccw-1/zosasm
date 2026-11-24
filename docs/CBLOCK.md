# Inline C blocks (`#pragma cblock`)

Status: experimental on branch `ccw-inc`. The M1 expression subset and M2
control flow are implemented. The full design is in `PLAN-CBLOCK.md`.

A CBLOCK is inline code, not a function. It appears where it is written,
reads and writes named registers, and falls through to the next assembler
statement. It is the reverse of C inline assembly: the block is written in a
small C subset, and zosasm generates the assembly.

## Example

```asm
        L      %r2,X
        L      %r3,Y
#pragma cblock squares
#pragma cin     int32_t a=%r2, int32_t b=%r3
#pragma cout    int32_t c=%r4
#pragma cscratch %r0, %r1, %r5
#pragma ctemp   %r12, 96, down
#pragma cbegin
    c = a*a + b*b;
#pragma cend
        ST     %r4,Z
```

The block reads `%r2` and `%r3`, writes `%r4`, uses `%r0`, `%r1` and `%r5`
as scratch, and keeps locals and spills in the 96-byte area based at `%r12`.
It creates no symbol and does not touch `%r14` or the stack pointer.

## Pragmas

| pragma | meaning |
|---|---|
| `cblock [name]` | start a block; the name is used in diagnostics and the listing |
| `cin type name=%rN` | register read by the block |
| `cout type name=%rN` | register written by the block |
| `cinout type name=%rN` | register read and written |
| `cscratch %rN,...` | registers the generated code may clobber |
| `ctemp %rN,SIZE,up\|down` | caller-provided temporary area |
| `cbegin` | start the C body |
| `cend` | end the block |

The temp base register cannot be `%r0`. A bound register cannot also appear in
`cscratch`. `%r0` and `%r1` are internal scratch registers. If both are missing from
`cscratch`, the block saves and restores them as a pair with `STMG`/`LMG`;
if only one is missing, it uses `STG`/`LG`. A bound `cin`/`cout` register
therefore cannot be `%r0` or `%r1`.

`ctemp` size is in bytes and must be a multiple of 8. The compiler computes the
slots it needs and reports `ZA5006` when the area is too small. The base
register is preserved.

## Language

Types:

* `int32_t`, `uint32_t`, `int64_t`, `uint64_t`
* `int` and `unsigned` as 32-bit aliases
* `uint8_t *`, `void *` and `char *` byte pointers

Pointers can be assigned, compared and adjusted by an integer byte count
(`p + 4`, `p - 4`). They cannot be dereferenced yet, and pointer subtraction
is not implemented.

Statements:

* declarations, scalar locals, assignment and compound assignment;
* `if`/`else`, `while`, `do ... while`, `for`;
* `break`, `continue` and nested loops;
* `switch`/`case`/`default`, with fallthrough and `break`;
* labels and `goto`;
* expression statements.

Expressions:

* integer literals, variables, casts;
* `+ - * / %`, shifts, bitwise operators;
* comparisons, `&&`, `||`, `!`, `?:`;
* prefix and postfix `++` and `--`.

There are no arrays, structures, strings, floating point, function calls,
global variables or recursion. Byte pointers are address values only; `*p`
and `p[i]` are not implemented. `return` is not allowed; the block ends
at `#pragma cend`.

Signed overflow wraps two's complement. Division by a constant zero is a
compile-time error; runtime division by zero traps. A constant shift count
outside the type width is an error; a variable count is masked by the machine
shift instruction.

`cinout` and `cout` values are written back at block exit. A `cout` variable
must be assigned before it is read; an obvious read before assignment is
`ZA5003`.

## Memory intrinsics

Three statement intrinsics move bytes:

```c
__memcpy(dst, src, len);   // non-overlapping
__memmove(dst, src, len);  // overlapping allowed
__memset(dst, byte, len);
```

`dst` and `src` are byte pointers; `byte` and `len` are integers. The size and
fill value may be constant expressions or runtime values.

* A constant `__memcpy` size up to 4096 emits `MVC` chunks.
* A zero constant `__memset` emits `XC`; other fills use a byte `STC` loop.
* `__memmove` compares the addresses at run time and uses a forward or
  backward `MVC` byte loop.
* Runtime `__memcpy` and `__memset` currently use byte loops. They do not use
  `MVCL`; a faster chunked `MVC` loop can replace them later.
* The intrinsics need nonzero scratch registers for the addresses. The
  compiler reports `ZA5005` when the `cscratch` list is too small.
* `__memcpy` requires non-overlapping ranges. Use `__memmove` when the ranges
  may overlap.
* Addresses are caller-supplied values. The caller is responsible for AMODE
  and for keeping the ranges valid and writable.

## Generated code

The block compiles to ordinary assembler statements. They pass through the
normal two-pass assembler, so labels, ZSYM, `-disasm` and object emission work.
The listing shows the C source line beside the generated instructions:

```text
                                6   * cblock squares
00001C  C0010000 0005          11   c = a*a + b*b;
000022  500C0060               11   c = a*a + b*b;
...
```

`-disasm` shows the generated assembly, not the C source. `-E` prints the
expanded assembly.

## Error codes

| code | meaning |
|---|---|
| ZA5001 | syntax or block structure error |
| ZA5002 | unsupported type, operator or feature |
| ZA5003 | type or assignment error |
| ZA5004 | undeclared or duplicate variable |
| ZA5005 | invalid register binding, scratch list or ctemp |
| ZA5006 | temp area too small |
| ZA5008 | unsupported statement, or break/continue outside a loop |
| ZA5009 | internal compiler error |

## Tests

* `make test` runs `tests/test_cblock.c`.
* `make test-cblock-runtime` runs generated ELF programs under
  `qemu-s390x` when `s390x-linux-gnu-ld` and `qemu-s390x` are installed.
* `tools/cblock_oracle.sh SRC.za pok56` expands a CBLOCK with `zosasm -E`,
  converts it to HLASM source and compares the GOFF structure with HLASM
  through `tryhlasm.sh`.

## Not implemented

Typed pointer dereference (`*p`, `p[i]`), pointer subtraction, a faster
chunked runtime copy, and structured DWARF for C variables. The generated
code already maps line information back to the C source, but local variable
locations and types are not described in `.debug_info`.
