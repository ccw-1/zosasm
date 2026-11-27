# Inline C blocks (`#pragma cblock`)

Status: experimental. Capture, integer expressions, control flow including
`switch`, labels and `goto`, byte and typed scalar pointers, opaque named
pointers, string literal tables, the memory intrinsics, the `__strlen`/`__memchr`/`__memcmp`/`__strcmp`/`__strcpy` intrinsics,
and DWARF locals with `-g` are implemented. The original design is in
`CBLOCK-DESIGN.md`.

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

## Examples

* `docs/examples/cblock.za` assembles only. It shows a loop, `switch`, byte
  and typed scalar pointers, an opaque DSECT pointer, `__sizeof`,
  `__offsetof`, `__memcpy`, `__memmove`, `__memset`, `__strlen`,
  `__memchr`, `__memcmp`, `__strcmp` and `__strcpy`. Comments give the
expected values.
* `docs/examples/cblock-linux.za` is a runnable Linux on Z program. It builds
  `strlen=5 pos=2` and writes it with `SVC 4`.
* `docs/GUIDE.md` section 22 prints both examples.

## Pragmas

| pragma | meaning |
|---|---|
| `cblock [name]` | start a block; the name is used in diagnostics and the listing |
| `cin type name=%rN` | general register read by the block |
| `cin type name=%fN` | floating-point register read by the block |
| `cout type name=%rN` | general register written by the block |
| `cout type name=%fN` | floating-point register written by the block |
| `cinout type name=%rN` | general register read and written |
| `cinout type name=%fN` | floating-point register read and written |
| `cscratch %rN,%fN,...` | registers the generated code may clobber |
| `ctemp %rN,SIZE,up\|down` | caller-provided temporary area |
| `cbegin` | start the C body |
| `cend` | end the block |

The temp base register cannot be `%r0` or `%r1`; both are internal scratch registers.
A bound register cannot also appear in
`cscratch`. `%r0` and `%r1` are internal scratch registers. `%r0` is used only as a value
scratch; it is never a base, index or branch register. If both are missing from
`cscratch`, the block saves and restores them as a pair with `STMG`/`LMG`;
if only one is missing, it uses `STG`/`LG`. A bound `cin`/`cout` register
therefore cannot be `%r0` or `%r1`. A `float` or `double` binding uses
`%f0` to `%f15`; a `long double` binding uses an FPR pair (`%f0`/`%f2`,
`%f4`/`%f6`, `%f8`/`%f10` or `%f12`/`%f14`). List every FPR that the
generated code may use in `cscratch`; for long double list both members of
each pair. Floating-point `cscratch` ranges are not supported.

`ctemp` size is in bytes and must be a multiple of 8. The compiler computes the
slots it needs and reports `ZA5006` when the area is too small. The base
register is preserved. Private labels are unique across all CBLOCKs in one
assembly, so a source file may contain several blocks.

## Language

Types:

* `int32_t`, `uint32_t`, `int64_t`, `uint64_t`
* `int` and `unsigned` as 32-bit aliases
* `float`, `double` and `long double` scalars and typed pointers, for the
  floating-point builtins below. They are IEEE binary floating point.
  A scalar holds its raw 4-byte, 8-byte or 16-byte value in temporary
  slots; use a `%fN` binding to read or write it without a conversion.
  Floating-point literals with an optional `f` suffix initialize `float`
  and `double` variables; `L` initializes a `long double` through a
  conversion. Casts convert between integer, `float`, `double`
  and `long double` values. Arithmetic (`+ - * /`), comparisons and
  conditions work on floating-point values; mixed integer and
  floating-point operands are converted to the floating type. The `%`
  operator is not available for floating point.
* `uint8_t *`, `void *` and `char *` byte pointers
* 16-bit scalars promote to 32 bits in expressions; loads use `LGH`/`LLGH`
  and stores use `STH`
* string literals as pointer initializers and intrinsic arguments
  (`char *p = "text";`); adjacent literals concatenate, C escapes work
* typed scalar pointers: `int32_t *`, `uint32_t *`, `int64_t *` and
  `uint64_t *`
* opaque named pointers such as `PARM *p`, where `PARM` is a DSECT or
  section name

Pointers can be assigned, compared, adjusted by an integer count
(`p + 4`, `p - 4`, `p += 4`) and subtracted (`p - q`). For byte pointers and
opaque named pointers the count is in bytes. For typed scalar pointers the
count is in elements: `p + 1` advances by 4 bytes for `int32_t *` and by 8
bytes for `int64_t *`; `p - q` returns an element count.

`*p` and `p[i]` load the pointer's element type. A byte pointer loads one
byte as unsigned 32-bit data; `int32_t *` loads 32 bits and `int64_t *` loads
64 bits. Assignment stores the same element width. `&local` returns a byte
pointer to the local's slot in `ctemp`; cast it to a typed pointer when
needed. `&*p` is just `p`. A bound `cin`/`cout` register has no storage, so
`&` rejects it.

A named pointer is opaque. It can be assigned, compared, passed to the
memory intrinsics and adjusted by a byte count, but it cannot be
dereferenced with `*p` or `p[i]` and has no `p->field` operator.

### Addressing a DSECT element

CBLOCK has no `p->field` operator, so it does not know the layout of a
DSECT. Compute the address of a field yourself: cast the named pointer to a
byte pointer, add `__offsetof(DSECT, field)`, and cast the result to the
field's type when you want a typed load or store.

```c
PARM     DSECT
P_FLAGS  DS    F        /* offset 0 */
P_NAME   DS    CL4      /* offset 4 */
P_SIZE   DS    D        /* offset 8 */

PARM *parm;             /* opaque: cannot write parm->P_FLAGS */
uint8_t *base;
uint32_t *pf;

base = (uint8_t *)parm;                              /* opaque -> byte pointer */
pf = (uint32_t *)(base + __offsetof(PARM, P_FLAGS)); /* &parm->P_FLAGS */
*pf = 12;                                            /* write the fullword   */
base[__offsetof(PARM, P_NAME)] = 88;                 /* &parm->P_NAME[0]     */
```

The base must be a byte pointer (`uint8_t *`, `char *` or `void *`). Its
`+` count is in bytes and it can be indexed byte by byte. A typed pointer
such as `uint32_t *` counts in elements, so `base + __offsetof(...)` and
then a cast is safer than mixing the two. `__offsetof` returns an integer,
not a typed field, and the DSECT must be visible to the assembler.

Statements:

* declarations, scalar locals, assignment and compound assignment;
* `if`/`else`, `while`, `do ... while`, `for`;
* `break`, `continue` and nested loops;
* `switch`/`case`/`default`, with fallthrough and `break`;
* labels and `goto`;
* expression statements.

Expressions:

* integer and floating-point literals (`1.5`, `1.5f`), string literals,
  variables, casts;
* `__align(N) "text"` aligns a generated literal table to a power-of-two
  boundary from 1 to 4096 (default: no alignment);
* `__sizeof(section)` and `__offsetof(section, field)`;
* unary `*` (element load) and `&` (address of a local);
* postfix `p[i]` (element load from `p + i`);
* `+ - * / %`, shifts, bitwise operators;
* comparisons, `&&`, `||`, `!`, `?:`;
* prefix and postfix `++` and `--` on variables, not on `*p`.

There are no arrays, structures, user-defined function calls,
global variables or recursion. Floating point is limited to the scalar
types and direct builtins described below. String literals are data tables, not values:
they can initialize a `char *` and are not otherwise assignable or indexable as
objects. The `__strlen`,
`__memchr`, `__memcmp`, `__strcmp`, `__strcpy`, `__sizeof` and
`__offsetof` forms are the only call-like expressions. There is no pointer-to-pointer type and no array type. `return`
is not allowed; the block ends at `#pragma cend`.

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

`dst` and `src` may be byte, typed, or opaque named pointers; the length is
always in bytes. `byte` and `len` are integers. The size and fill value may
be constant expressions or runtime values.

A complete copy block:

```asm
#pragma cblock copy
#pragma cin     uint8_t *src=%r2
#pragma cinout  uint8_t *dst=%r3
#pragma cscratch %r0, %r1, %r5
#pragma ctemp   %r12, 32, down
#pragma cbegin
    __memcpy(dst, src, 16);
#pragma cend
```

This copies 16 bytes with constant-size `MVC` chunks. A runtime length uses
the same call with an integer expression as the third argument.

Two assembly-time lookup intrinsics are also available:

```c
uint64_t n = __sizeof(PARM);             // sizeof(PARM)
uint64_t o = __offsetof(PARM, P_TABLE);  // offsetof(PARM, P_TABLE)
```

They emit the assembler's `sizeof` and `offsetof` expressions as `LGFI`
immediates. The section or DSECT must be visible to the assembler. They do not
give CBLOCK a layout: `__offsetof` returns an integer, not a typed field.

Byte scan and compare intrinsics:

```c
uint64_t n = __strlen(str);            // length up to the first zero byte
uint8_t *p = __memchr(str, byte, len); // pointer to byte, or 0
int32_t c = __memcmp(a, b, len);       // <0, 0, >0
int32_t c = __strcmp(a, b);            // <0, 0, >0
char *r = __strcpy(dst, src);          // copies through the zero byte, returns dst
```

`__strlen` takes a pointer and returns an unsigned 64-bit length. When the
argument is a string literal, or a pointer initialized with one and never
reassigned, the length is a compile-time constant and no scan loop is emitted.
`__memchr` takes a pointer, an integer byte, and a length, and returns a byte
pointer to the first matching byte, or 0. `__memcmp` compares exactly `len`
bytes and `__strcmp` compares through the first zero byte; both compare
unsigned bytes and return -1, 0 or 1. In a boolean condition,
`if (__memcmp(a,b,n))` is true when the ranges differ and false when they are
equal. `__strcpy` copies one byte at a time,
including the terminating zero, and returns the original destination pointer.
It does not check the destination size or overlap. When one is the condition of an `if`,
`while` or `for`, or is compared to zero with `!`, `==`, `!=`, `<`, `<=`, `>`
or `>=`, the generated code branches directly on the `CLC` condition code and
does not create the -1/0/1 result. Comparisons that are impossible for the
-1/0/1 range are folded: `if (__memcmp(a,b,n) > 2)` emits only the false path,
and `if (__memcmp(a,b,n) != 2)` emits only the true path. The byte value is truncated to eight
bits. Both use a byte loop with `LLGC`, so the loaded byte does not inherit the
upper bits of the scratch register. `LLGC` zero-extends the byte; an `IC`
would leave the previous high bits in place and break the zero test.

### Initial z/Architecture builtins

The first group of direct-instruction builtins is implemented:

```c
uint64_t n = __popcnt(x);       // POPCNT
uint32_t v = __lrv(p);          // LRV
uint16_t v = __lrvh(p);         // LRVH
uint64_t v = __lrvg(p);         // LRVG
__strv(v, p);                   // STRV, result value 0
__strvh(v, p);                  // STRVH, result value 0
__strvg(v, p);                  // STRVG, result value 0
int cc = __stck(p);             // STCK, returns CC
int cc = __stcke(p);            // STCKE, returns CC
int cc = __stckf(p);            // STCKF, returns CC
```

`__lrv*` and `__strv*` take a byte/typed pointer; `__lrvh`/`__strvh`
use `uint16_t *` and halfword loads/stores. `__stck*` take a
16-byte (STCK/STCKE) or 8-byte (STCKF) output area. The clock builtins return
the instruction condition code. `__strv*` return 0.

Load-and-atomic builtins:

```c
int cc = __lad(old, value, mem);   /* LAA: old = *mem; *mem += value */
int cc = __ladl(old, value, mem);  /* LAAL */
int cc = __lan(old, value, mem);   /* LAN */
int cc = __lax(old, value, mem);   /* LAX */
int cc = __lao(old, value, mem);   /* LAO */
int cc = __csg(compare, mem, new); /* CSG; compare receives the old value */
int cc = __cdsg(compare, mem, new);/* CDSG; 16-byte compare/new pairs */
int cc = __lpd(a, b, c, d);        /* LPD; load pair from c,d into a,b */
```

`__ladg`, `__ladlg`, `__lang`, `__laxg`, `__laog` and `__lpdg` are the
64-bit forms. `__lpd`/`__lpdg` require an even-odd register pair in
`cscratch`; both registers of the pair must be listed. `__cdsg` requires two
even-odd pairs plus one address register. `__csg` needs three nonzero address
registers. The current allocator does not yet implement pair-aware allocation
for `CLCL`, `MVCLE`, or vector/FPR overlaps; those builtins are not exposed
yet.

Constant-length decimal and character builtins are also available:

```c
int cc = __ap(a, l1, b, l2);   /* AP */
int cc = __cp(a, l1, b, l2);   /* CP */
int cc = __sp(a, l1, b, l2);   /* SP */
int cc = __mp(a, l1, b, l2);   /* MP */
int cc = __dp(a, l1, b, l2);   /* DP */
int cc = __zap(a, l1, b, l2);  /* ZAP */
int cc = __nc(a, b, len);      /* NC */
int cc = __oc(a, b, len);      /* OC */
int cc = __xc(a, b, len);      /* XC */
void  __tr(a, table, len);     /* TR */
int cc = __cvb(p);             /* CVB */
void  __cvd(v, p);             /* CVD */
int cc = __pack(a, l1, b, l2); /* PACK */
int cc = __unpk(a, l1, b, l2); /* UNPK */
int cc = __ed(a, b, len);      /* ED */
int cc = __tp(a, len);         /* TP */
int cc = __srp(a, len, shift, pattern); /* SRP */
int64_t v = __cvbg(a);        /* CVBG */
void __cvdg(v, a);            /* CVDG */
void __pka(a, b, len);        /* PKA */
void __pku(a, b, len);        /* PKU */
void __unpka(a, len, b);      /* UNPKA */
void __unpku(a, len, b);      /* UNPKU */
void __mvcrl(a, b, len);      /* MVCRL */
```

### IEEE floating-point builtins

The block supports `float` and `double` bindings and direct IEEE
floating-point builtins. List the FPRs the generated code may use in
`cscratch`.

```c
float  f = __sqebr(x);        /* SQEBR */
double d = __sqdbr(x);        /* SQDBR */
float  f = __fiebr(mode, x);  /* FIEBR, mode is 0 to 7 */
double d = __fidbr(mode, x);  /* FIDBR, mode is 0 to 7 */
float  f = __maebr(a, b, c);  /* MAEBR */
double d = __madbr(a, b, c);  /* MADBR */
float  f = __msebr(a, b, c);  /* MSEBR */
double d = __msdbr(a, b, c);  /* MSDBR */
float  f = __fmadds(a, b, c); /* same as __maebr */
double d = __fmadd(a, b, c);  /* same as __madbr */
float  f = __fmsubs(a, b, c); /* same as __msebr */
double d = __fmsub(a, b, c);  /* same as __msdbr */
int cc = __lpebr(&f, x);      /* LPEBR, stores the result through &f */
int cc = __lpdbr(&d, x);      /* LPDBR */
int cc = __lnebr(&f, x);      /* LNEBR */
int cc = __lndbr(&d, x);      /* LNDBR */
int cc = __cfebr(&i, mode, x);/* CFEBR, mode is 0 to 15 */
int cc = __cfdbr(&i, mode, x);/* CFDBR */
int cc = __cgebr(&l, mode, x);/* CGEBR */
int cc = __cgdbr(&l, mode, x);/* CGDBR */
int fpc = __efpc();           /* EFPC */
__sfpc(fpc);                  /* SFPC */
long double x = __sqxbr(y);   /* SQXBR */
long double x = __fixbr(mode, y); /* FIXBR, mode is 0 to 7 */
int cc = __lpxbr(&x, y);      /* LPXBR */
int cc = __lnxbr(&x, y);      /* LNXBR */
int cc = __cfxbr(&i, mode, y);/* CFXBR, mode is 0 to 15 */
int cc = __cgxbr(&l, mode, y);/* CGXBR */
int cc = __tceb(x, m);        /* TCEB; m is the mask address */
int cc = __tcdb(x, m);        /* TCDB */
int cc = __tcxb(x, m);        /* TCXB */
int cc = __thder(&d, x);      /* THDER: BFP short to HFP */
int cc = __thdr(&d, x);       /* THDR: BFP long to HFP */
int cc = __tbedr(&f, mode, x);/* TBEDR: HFP to BFP short */
int cc = __tbdr(&d, mode, x); /* TBDR: HFP to BFP long */
int cc = __diebr(&q, &r, a, b, mode); /* DIEBR: 7.0 / 2.0, mode is 0 to 15 */
int cc = __didbr(&q, &r, a, b, mode); /* DIDBR */
void __srnm(m);               /* SRNM; m is the mask address */
```

`long double` uses 16 bytes and an FPR pair. Store it in a local, pass it
through a `%fN` binding, or produce it with a cast or a long double
builtin. `__fixbr` and `__sqxbr` return `long double`; `__lpxbr` and
`__lnxbr` store the result through their first pointer and return the
condition code; `__cfxbr` and `__cgxbr` store the integer through their
first pointer and return the condition code. The long double forms use the
same arithmetic and comparison operators as `float` and `double`.

The load positive and load negative forms store the converted operand
through the first pointer and return the instruction condition code. The
convert-to-fixed forms store the integer through the first pointer and
return the condition code. `__efpc` returns the floating-point control
register; `__sfpc` writes it.

Floating-point arithmetic keeps the IEEE binary result in the variable's
type. `float` and `double` values can be compared with `<`, `>`, `<=`, `>=`,
`==` and `!=`; the result is an `int32_t`. Unary `+`, unary `-` and `!` work,
and floating-point values can be used in `if`, `while`, `for`, `&&` and
`||`. Compound assignments such as `f += 2` work. A conversion to an integer
truncates toward zero. `++`/`--`, `%` and bit operations on floating-point
values are not implemented. The test-data-class forms take the mask
address as their integer argument, matching XL C. Hexadecimal floating
point and the `-qfloat=hex` forms are not implemented.

### Decimal floating point

`_Decimal64` and `_Decimal128` values are supported for the conversion
builtins. `_Decimal64` uses one FPR, `_Decimal128` uses an FPR pair, and
both can be bound through `%fN`. Decimal arithmetic, casts and literals are
not implemented.

```c
_Decimal64  d = __cdzt(p, length, mask);   /* CDZT: zoned to DFP64 */
_Decimal128 x = __cxzt(p, length, mask);   /* CXZT: zoned to DFP128 */
int cc = __czdt(d, p, length, mask);       /* CZDT: DFP64 to zoned */
int cc = __czxt(x, p, length, mask);       /* CZXT: DFP128 to zoned */
```

`length` and `mask` are compile-time constants. The source or result
pointer points to the zoned decimal field. `__czdt` and `__czxt` return the
instruction condition code. The `-qdfp` option activates these builtins in
XL C; CBLOCK accepts the names directly.

### Hexadecimal floating-point builtins

The `-qfloat=hex` conversion and arithmetic forms use the same
`float`, `double` and `long double` CBLOCK types, but the instructions
operate on hexadecimal floating-point values:

```c
int cc = __lper(&f, x);       /* LPER; stores through &f */
int cc = __lpdr(&d, x);       /* LPDR */
int cc = __lner(&f, x);       /* LNER */
int cc = __lndr(&d, x);       /* LNDR */
float  f = __sqer(x);         /* SQER */
double d = __sqdr(x);         /* SQDR */
long double x = __sqxr(y);    /* SQXR */
float  f = __fier(x);         /* FIER */
double d = __fidr(x);         /* FIDR */
long double x = __fixr(y);    /* FIXR */
int cc = __cfer(&i, mode, x); /* CFER */
int cc = __cfdr(&i, mode, x); /* CFDR */
int cc = __cfxr(&i, mode, y); /* CFXR */
int cc = __lpxr(&y, x);       /* LPXR */
int cc = __lnxr(&y, x);       /* LNXR */
int cc = __cger(&l, mode, x); /* CGER */
int cc = __cgdr(&l, mode, x); /* CGDR */
int cc = __cgxr(&l, mode, y); /* CGXR */
```

The `mode` values are compile-time constants from 0 to 15. These
builtins do not convert between the hexadecimal and binary formats. Do
not mix their results with ordinary BFP arithmetic; use an explicit
conversion or keep the HFP value as raw data.

### Runtime facility checks

Three builtins expose the facility list:

```c
int cc = __stfle(area);              /* one STFLE, stores 8 bytes, returns CC */
int has = __facility(129);           /* issues STFLE internally and tests bit 129 */
int has = __facility_test(list, 129);/* tests bit 129 in a caller-supplied list */
```

Facility bits are numbered from 0 at the most significant bit of the first
8-byte word. `__facility` uses a 32-byte temporary block and supports bits
0 through 255. `__stfle` and `__facility_test` let the caller supply the
storage, for example a local variable or a z/OS absolute address. Prefer an
explicit storage area over the z/OS location-200 convention: writing a
fixed system location is platform-specific and can clash with the caller.
The result is `1` when the bit is set and `0` otherwise.

### Vector builtins

The first vector group supports `__vector` types, `%vN` bindings and
`%vN` entries in `cscratch`.

```c
__vector unsigned int v;
v = __vec_splats_u4(7);              /* VLREP */
v = __vec_load_len_u4(p, 16);        /* VLL */
v = __vec_load_bndry_u4(p, 16);      /* VL; the boundary is an alignment hint */
__vec_store_len(v, p, 16);           /* VSTL */
v = __vec_sel(a, b, mask);           /* VSEL */
v = __vec_perm(a, b, mask);          /* VPERM */
v = __vec_permi(a, b, 1);            /* VPDI */
cb = __vec_cmpeq(a, b);              /* VCEQ, integer vectors */
cb = __vec_cmpge(a, b);              /* VCH/VCHL plus VNO */
cb = __vec_cmpgt(a, b);
cb = __vec_cmple(a, b);
cb = __vec_cmplt(a, b);
v = __vec_max(a, b);                 /* VMX/VMXL/VMXF/VMXG */
v = __vec_min(a, b);                 /* VMN/VMNL/VMNF/VMNG */
v = __vec_avg(a, b);                 /* VAVG/VAVGL/VAVGF/VAVGG */
x = __vec_extract_s4(v, 3);          /* VSTEF; index is a constant */
v = __vec_insert_s4(x, v, 1);        /* VLEF */
v = __vec_promote_s4(x, 2);          /* VGBM plus VLEF */
v = __vec_insert_and_zero_s4(p);     /* VLLEZ */
```

The element types are `char`, `short`, `int`, `long long`, `float` and
`double`, with `signed`, `unsigned`, `bool` and the `s1/u1/s2/u2/s4/u4/s8/u8/f4/f8`
builtin suffixes. Vector select, permute, permute-doubleword-immediate, integer
ordered/equal compares, max, min, average, and scalar element
extract/insert/promote are implemented. Vector arithmetic and the remaining
`__vec_*` builtins are not implemented yet. `__vec_load_bndry_*` accepts the boundary argument
but uses an unaligned `VL`, which is correct and only loses an alignment
optimization.

The length arguments must currently be compile-time constants in the
architectural 0..15 length-field range. Runtime lengths need the same
`EXRL`/template machinery as the compiler; that is a later step.

### String literal tables

A string literal used as a pointer initializer emits a byte table after the
block, preceded by a branch over the table:

```c
const char *table = __align(8) "\x00ABC\n";
```

`__align(8)` is optional. The default is no alignment. The generated table is
part of the current CSECT, and the block loads its address with `LARL`. The
pointer is ordinary writable storage as far as CBLOCK is concerned; put the
table in a read-only class if writes must fault. Adjacent string literals are
concatenated, and the table always ends with a zero byte.

* A constant `__memcpy` size up to 4096 emits `MVC` chunks.
* Runtime `__memcpy` copies 256-byte `MVC` chunks first. The remainder uses
  an `EX` over an `MVC` template when a third scratch register is available;
  otherwise it falls back to a byte loop. `EX` does not change the template in
  storage. It ORs the low byte of its length register into the template length
  byte. The length register must be nonzero: `EX` with an R1 field of zero does
  no ORing at all, so `EX %r0,...` executes the template with its original
  length. The generated template starts with length zero.
* A zero constant `__memset` emits `XC`; other fills use a byte `STC` loop.
* `__memmove` compares the addresses at run time and uses a forward or
  backward `MVC` byte loop.
* None of the intrinsics uses `MVCL`. Runtime `__memset` and `__memmove` still
  use byte loops; chunked versions can follow.
* `__strlen` and `__memchr` use byte loops. `SRST` (Search String) can replace
  the scan loop later, but the loop is the current tested form.
* The `__strlen` path was also tested on z/OS by replacing the scan loop in
  `docs/examples/showargs.za` with a CBLOCK, building AMODE 31 and AMODE 64,
  and running both with `a bb ccc dddd eeeee`. Both builds printed the
  arguments cleanly.
* The intrinsics need nonzero scratch registers for the addresses. The
  compiler reports `ZA5005` when the `cscratch` list is too small.
* `__memcpy` requires non-overlapping ranges. Use `__memmove` when the ranges
  may overlap.
* Addresses are caller-supplied values. The caller is responsible for AMODE
  and for keeping the ranges valid and writable.

## Debug information

With `-g`, each CBLOCK emits a lexical block and a DWARF variable for every
local. The variable location is `DW_OP_breg<ctemp-reg> <slot-offset>`. A
32-bit local is `int32` or `uint32`, a 64-bit local is `int64` or `uint64`,
and a pointer local is represented as an unsigned 64-bit value. Bound
`cin`/`cout` registers are not variables with storage, so they get no DWARF
variable.

The block is one scope. Locals declared inside nested blocks or loops are
visible for the whole CBLOCK range. Debug line mapping still uses the `.za`
line that contains the generated statement, not the C source line.

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
* `make test-cblock-runtime` runs 48 generated ELF cases under `qemu-s390x`
  when `s390x-linux-gnu-ld` and `qemu-s390x` are installed. The suite covers
  arithmetic, control flow, pointer arithmetic, `*p`, `p[i]`, `&local`,
  `__memcpy`, `__memmove`, `__memset`, `__strlen`, `__memchr`, `__memcmp`,
  `__strcmp`, `__strcpy`, direct conditions, `__popcnt`, `__lrv`, and the
  runnable example.
* `tools/cblock_oracle.sh SRC.za pok56` expands a CBLOCK with `zosasm -E`,
  converts it to HLASM source and compares the GOFF structure with HLASM
  through `tryhlasm.sh`.

## Not implemented

Pointer-to-pointer types, `p->field` access, arrays, `SRST`-based string and
byte scans, a faster chunked runtime `memset` and `memmove`, nested CBLOCK
scopes, and C source line mapping in DWARF. Debug lines still use the `.za` line that contains the generated
statement.
