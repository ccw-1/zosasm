# Changelog

Versions follow semantic versioning (`src/version.h`); releases are git tags
`vX.Y.Z`. Things not compared with IBM's tools are marked in the User's Guide
(section 14).

## Unreleased

* `XATTR RENAME(YES|NO)` sets the GOFF ESD renameable flag (bit 41.2).
  `-disasm` writes it back and `-dump-readable` shows it.
* `-dump-readable FILE` prints a grouped GOFF listing with section, text and
  relocation headings. Without `-insn`, text uses an xxd-style layout with
  EBCDIC and ASCII columns. `-insn` (with `-dump` or `-dump-readable`)
  disassembles text records using a best-effort even-byte skip over data. The
  existing machine-readable `-dump` output is unchanged.
* CBLOCK: initial direct z/Architecture builtins `__popcnt`, `__popcnt4`,
  `__popcnt8`, `__lrv`, `__lrvh`, `__lrvg`, `__strv`, `__strvh`, `__strvg`, `__stck`, `__stcke`,
  `__stckf`, `__lad`/`__ladl`/`__ladg`/`__ladlg`, `__lan`/`__lang`,
  `__lax`/`__laxg`, `__lao`/`__laog`, `__csg`, `__cdsg`, `__lpd` and `__lpdg`.
  Constant-length decimal/character builtins `__ap`, `__cp`, `__sp`, `__mp`,
  `__dp`, `__zap`, `__nc`, `__oc`, `__xc`, `__tr`, `__ed`, `__tp`, `__srp`,
  `__cvb`, `__cvd`, `__cvbg`, `__cvdg`, `__pack`, `__unpk`, `__pka`, `__pku`,
  `__unpka`, `__unpku` and `__mvcrl` are included. PLO/TM/vector/far/float groups are
  deferred. `__lpd*` needs an even-odd pair in `cscratch`; decimal lengths
  must be compile-time constants.
* CBLOCK IEEE `float` and `double` scalars, pointers and `%fN`
  `cin`/`cout` bindings, with `cscratch` FPRs. Direct builtins: `__sqebr`,
  `__sqdbr`, `__fiebr`, `__fidbr`, `__maebr`, `__madbr`, `__msebr`,
  `__msdbr`, `__fmadd`, `__fmadds`, `__fmsub`, `__fmsubs`, `__lpebr`,
  `__lpdbr`, `__lnebr`, `__lndbr`, `__cfebr`, `__cfdbr`, `__cgebr`,
  `__cgdbr`, `__efpc` and `__sfpc`. Floating literals use `1.5` for
  `double` and `1.5f` for `float`; `+ - * /`, comparisons, unary operators
  and conditions work on `float` and `double`, with integer operands
  converted to the floating type. Compound assignments work.
  Float-to-integer casts truncate toward zero. `++`/`--`, `%` and bit
  operations on floating-point values are not implemented. `long double`
  uses IEEE binary128, `1.5L` literals and FPR pairs, with `__sqxbr`,
  `__fixbr`,
  `__lpxbr`, `__lnxbr`, `__cfxbr` and `__cgxbr`, plus long double
  arithmetic and comparisons. Long double and hexadecimal float forms are deferred.
* CBLOCK 16-bit `int16_t`/`uint16_t` scalars and pointers, with `LGH`/`LLGH`
  loads, `STH` stores, integer promotion, typed pointer arithmetic, and
  `__lrvh`/`__strvh`.

## 0.7.3 - 2026-11-25

* CBLOCK folds comparisons of `__memcmp`/`__strcmp` that are fixed by their
  -1/0/1 result range: `> 2` emits only the false path and `!= 2` only the true
  path, with no compare loop or result temporary.

## 0.7.2 - 2026-11-25

* CBLOCK conditions such as `if (__memcmp(a,b,n) > 0)` and
  `if (__strcmp(a,b) == 0)` also branch directly on the `CLC` condition code,
  without materializing the -1/0/1 result. Comparisons against nonzero
  constants still use the general expression path.

## 0.7.1 - 2026-11-25

* Fix the `__memcmp` loop for an exact match: it now stops when the length
  reaches zero instead of comparing past the requested range. This affected
  both the -1/0/1 result and the direct `if (__memcmp(...))` condition.

## 0.7.0 - 2026-11-25

* CBLOCK `__strcpy(dst, src)` copies through the zero byte and returns the
  original destination pointer.
* `__memcmp` and `__strcmp` used directly as `if`/`while`/`for` conditions
  branch on the `CLC` condition code without creating a -1/0/1 temporary.

## 0.6.0 - 2026-11-25

* CBLOCK `__memcmp(a, b, len)` and `__strcmp(a, b)` return -1, 0 or 1,
  compare unsigned bytes, and use `CLC` loops without extra scratch registers.

## 0.5.0 - 2026-11-25

* CBLOCK string literal tables: `char *p = "..."` and
  `const char *p = "..."` emit a private byte table in the current CSECT and
  load its address with `LARL`. Adjacent literals concatenate and C escapes
  work. `__align(N) "..."` aligns the table to a power of two from 1 to 4096;
  the default is no alignment. `__strlen` of a literal or of a pointer
  initialized with one and never reassigned is compiled to a constant.
* CBLOCK rejects `%r1` as the `ctemp` base register. Like `%r0`, `%r1` is an
  internal scratch register; using it as the temp base made the generated code
  clobber the base part way through a block.

## 0.4.1 - 2026-11-25

Patch release: HLASM comparison fixes, section `START`/`LTORG` model updates,
library macro paths, `#line`, and one-shot cleanup. No object format change.

### Fixed (HLASM comparison, 2026-11-25)

* `EQU` inherits the length attribute of its leftmost term. `ALIAS EQU SRC`
  now has the length of `SRC`, so `MVC ALIAS,DST` moves the whole field.
* A location counter reference in a duplicated or multi-value address
  constant advances with each constant (`DC 2A(*)`, `DC A(*,*)`, `DC 2S(*)`).
* `SY` and `QY` constants are 3 bytes and are not aligned, as in HLASM.
* The type and length attributes can be 0 (`EQU ...,0`), and the EQU type
  attribute follows the character set (`C'A'` is 0x41 with `-ascii`).
* The EQU length attribute is checked against 0 to 65535 (`ZA1034`).
* Numeric constants no longer leak: the Big temporaries live in the Module
  arena, the `dc_int`/`dc_float` strings and error buffers are freed, and
  `rep_free()` walks the whole expansion map. `make test-free-asan` now runs
  with leak detection on.
* `-o` and `-l` cannot overwrite the input source; `-disasm -o` cannot
  overwrite the input object; `-E` honours `-o` and rejects `-l`.
* `hlasm2za` no longer warns about legal HLASM forms that zosasm now accepts:
  `EQU` with 4 or 5 operands, `DC 0D` without a value, `CNOP` above 16, a
  three-operand `ORG`, an unnamed `DSECT`, a `START` address and
  `XATTR ATTRIBUTES`.
* Removed the dead `Asm.round8` fallback; `DESIGN.md` now says that a LOCTR
  layout that does not settle after five passes is `ZA9002`.
* Forward-reference `EQU`: `X EQU Y+4` before `Y` is defined.  The value is
  resolved lazily (including its length); a value that never resolves is
  `ZA1035`.  `EQU` still reports a CSECT size that would have to size storage
  (`ZA1042`) immediately.
* More HLASM source forms: `DC 0D`/`0F` without a value (duplication 0),
  `CNOP` boundaries 4 to 4096, `ORG expression,boundary,offset`, an unnamed
  `DSECT`, `EQU` program and assembler type operands, and the `DC` program
  type subfield `P(n)`, and `XATTR ATTRIBUTES(label)` (the LD/PR extended
  attribute is the class ED id and the label's class offset).  `LTORG` may
  assemble a pool in a DSECT (no object bytes; a CSECT reference still needs
  addressability).  `START address` now drives the section location space,
  the ED/TXT offsets, labels and addends.  `CNOP` above 16 keeps the
  `0700`/`47000700` no-op fill (HLASM may use a different long NOP pattern).
  Generic `-disasm` keeps `XATTR ATTRIBUTES` best effort; the ZSYM path is
  byte exact.
* `XATTR PSECT(name)` now writes the ADA field of the LD or PR with the ESD id
  of the PSECT class, as HLASM does.  A bad name on an internal symbol is
  `ZA3038`; on an external symbol the attribute is accepted and ignored.
  Generic `-disasm` writes `XATTR PSECT(classname)` from ADA; a class name is
  accepted as an extension.
* Thread-local assembler state: `value_arena`, `za_ascii`, `ieee_ebits`, the
  Big arena, the DP/MP length, the instruction/debug caches and the ZSYM CRC
  table use `ZA_TLS` (`__thread` on gcc/clang).  Two threads can assemble at
  the same time; `make test-threads` checks it and CI runs it.

* Library macro lookup now searches `#pragma macro_path` directories, `-M`
  directories, the directory of the source file, `ZOSASM_MACLIB` (colon
  separated) and `-I` directories, in that order.  `hlasm2za -macro-path DIR`
  writes `#pragma macro_path "DIR"` at the start of the converted source.
* `#line N ["file"]` renumbers the following source lines; diagnostics, the
  listing, ZSYM and DWARF use the new file and line.
* The one-shot CLI frees the preprocessor, its source and output lines, the
  events, and the emitter's ZSYM temporaries.  A plain source assembles
  LeakSanitizer clean; macro-heavy sources still leave a few hundred bytes
  of macro-system internals to process exit.

### Tests

* HLASM-derived cases for the EQU length, forward EQU, `*` duplication,
  `SY`/`QY` alignment, zero attributes, the ASCII type and PSECT/ADA.
* `make test-threads` runs two concurrent assemblies; CI runs it under gcc.
* `test_asm_free` includes numeric constants and runs with `detect_leaks=1`;
  CI runs it and the CBLOCK runtime test (qemu-s390x).

## 0.4.0 - 2026-11-24

CBLOCK (`#pragma cblock`): inline integer C blocks with bound registers,
scalar locals, expressions, control flow including `switch` and `goto`, byte
and typed scalar pointers with `*p`, `p[i]` and `&local`, opaque named
pointers, `__sizeof`/`__offsetof`, `__memcpy`/`__memmove`/`__memset`, and
`__strlen`/`__memchr`; `-g` writes DWARF lexical blocks and locals for
CBLOCK variables. Private labels are unique across blocks. See
`docs/CBLOCK.md`; examples are `docs/examples/cblock.za` and
`docs/examples/cblock-linux.za`.

Review fixes (2026-11-24): `asm_free()`/`rep_free()` correctness, an arena
for `Value` terms owned by the Module, operand/expression/emitter
destructors, `#define` recursion and macro string/list limits,
`R()` of a LOCTR, `ENTRY` in a DSECT, statements after END, comment lines in
continuations, `#if`/`DXD` overflow, the CLI output paths (`-o -`,
`/dev/null`, `-g` aliases, collisions), exact oracle exit statuses, golden
GOFF/ELF/`.dbg` objects, `make test-free-asan` and `make ebcdic-check`.

## 0.3.0 - 2026-11-23

* Column anchor `^`: a line whose first non-blank character is `^` is read as if
  the text after it started in column 1 (`    ^LB1  DC  AD(0)`; GUIDE 2.1). It
  indents statements that have a name, also in macro bodies (`^NAME op` is a model
  statement with the label `NAME`). Diagnostics point at the real columns; `zafmt`
  keeps such lines as written.
* Block comments `/* ... */` (several lines possible, GUIDE 2.1): blanked by the
  preprocessor before anything reads the lines, so line numbers and columns do not
  change; not recognised inside `'..'`, `".."`, comment lines (also indented ones) and after `//`; the
  listing shows the lines as written; an unclosed comment is ZA4036; `zafmt` keeps
  lines that touch a comment. `hlasm2za` writes `/ *` in a remark that holds `/*`
  (PL/X style text in `IHAFPC` and others would swallow the rest of the macro).

* `hlasm2za` converts all of SYS1.MACLIB (2080 members, 2214 macros, 0 failed; was
  2210 and 8 failed). Fixed: a comment line with a non-blank column 72 continues
  on the next record (HLASM rule, `IXGCON`); `MNOTE` with a blank after the comma
  or an unquoted message is passed on as a model statement (HLASM ASMA163W, no
  message; `CPF`, `IHBRDWRA`); `LCLx @NAME` without `&` is accepted; a prototype
  operand that is not a symbolic parameter is ignored with a warning (`IGWRMRCD`);
  text after the last `MEND` of a member no longer counts as a failed macro
  (`IRRPICRX`, `CBRSMF`). `-macro-dir` lists PL/X members, members without a
  macro and failures separately, and exits 1 only for failures. Expansions of
  `IXGCON`, `IGWRMRCD`, `IHBRDWRA` and `CPF` equal HLASM R6.0's
  (`tools/oracle_macro_cmp.py`); the rest of the library is not compared (PLAN.md 4.8).
* `-elf -g` writes the DWARF 4 into the ELF64 object as `.debug_*` sections
  with RELA relocations against the `.text.NAME` sections (`src/dbg_dwarf.c`,
  `dbg_build_elf`, `module_emit_elf_dbg`). Checked with readelf, ld and gdb
  (`gdb-multiarch` under `qemu-s390x`): breakpoints, source, variables and `bt`
  work. `-gside`, `-gdwarf` and `-gdbgpath` are rejected with `-elf`.
* CFI directive `DWCFI CFA|SAVE|SAME|UNDEF` (GUIDE 17): `.debug_frame` with one
  FDE per CSECT, so gdb unwinds through the callers (checked under `qemu-s390x`).
  Named `DWCFI` because `CFI` is an instruction.
* `-g` puts the DWARF into the object: the link module gets the class
  `C_ADATAE9C3` (NOLOAD, framed copies), so no `.dbg` file is needed for LLDB.
  The `.dbg` file is now written only with `-gside` (new); `-gdwarf` implies
  `-gside`. dbx still reads the `.dbg` file. LLDB reads the embedded copy first
  (`CDADwarf.cpp`, `locateEmbedded`); not yet verified on z/OS.

* Library memory management: `asm_free(Module *)` and `rep_free(Reporter *)`
  release a module and a reporter; the one-shot CLI also frees its
  preprocessor, source/output lines, events and ZSYM temporaries.
  `make test-free-asan` runs the destructor test under ASan/UBSan.
* New macro built-ins `sys.asm`, `sys.ver`, `sys.system_id`, `sys.clock`,
  `sys.stmt` and `sys.opt_elf|strict_registers|ascii|ebcdic`; `fill(n, v)`
  and `aread()` are documented with the other functions.
* `-elf` rejects objects with 65535 or more sections instead of truncating
  section indices. A single `DC` or bit-length `DS` is limited to 64 MiB
  (`ZA2087`; `ZOSASM_MAX_DC_BYTES` raises the cap).
* MCP hardening: 16 MB request cap, per-request cleanup (ASan/LSan verified),
  workspace sandbox for every file name, `ZOSASM_UNRESTRICTED` escape hatch,
  and a bounded result instead of discarding a whole reply.
* `make isa-check` verifies that `src/isa_tab.c` matches `isa/isa.json`, and
  `make ebcdic-check` verifies the EBCDIC tables. Golden GOFF/ELF objects
  (`testdata/golden*`) pin the emitted bytes.
* Correctness fixes from the reviews: `R()` of a LOCTR name, `ENTRY` of a
  DSECT label, statements after `END`, a comment line inside a continuation,
  `#if` overflow (depth cap 200, ZA4030), `#define` recursion depth cap 200
  (ZA4042), macro string/list limits (MAX_STR 4 MiB, ZA4112), `DXD` length
  overflow, and the CLI output paths (`-o -`, `/dev/null`, `-g` aliases,
  hidden `-l`/`-o` collisions).

## 0.2.0 - 2026-11-18

Hardening and tooling after a full review of the sources: hostile source
gets a diagnostic instead of a crash, hang or out-of-memory exit; the MCP
server restricts file access to a workspace, frees its memory per request
and caps the request size; the object writers validate what the format can
hold before writing; CI runs the whole suite on every push (gcc, clang,
sanitizers); `zosasm -` reads the program from standard input. New
built-in names `__AMODE__` and `__RMODE__` (preprocessor and `#macro`).

* New built-in names `__AMODE__` and `__RMODE__`, in the preprocessor and in
  `#macro` code (`&__amode__`): 0 until an `AMODE` or `RMODE` statement with
  24, 31 or 64 is read, then that number (the last one read; macro generated
  statements count). `RMODE ANY` gives 31 (as the assembler reads it),
  `AMODE ANY` and `MIN` change nothing. Guide 15.5 shows one program built
  for both modes (`docs/examples/showargs.za`, run on z/OS in both builds).
* `zosasm -` reads the program from standard input, as `hlasm2za` and
  `zafmt` already do. `-diag json` escapes bytes >= 0x80 in messages (as
  `\u00xx`), so the document is valid JSON even when a message quotes
  non-ASCII source text. The diagnostics are sorted with `qsort` (stable
  through the original order as the tie break) instead of an insertion
  sort whose cost grew quadratically with the number of messages.

* `hlasm2za` fixes: `-macro-dir` returns a failure exit code when a member
  cannot be read or its `.zmac` cannot be written (it used to print a warning
  and succeed); open code conditional assembly that cannot be converted no
  longer emits a `MACRO`..`MEND` definition inside it twice (once converted,
  once as a comment); a stray `#}` where no macro block is open is an error
  (`ZA4035`) instead of a line passed to the assembler, and the unterminated
  `#if` error says when the condition was written like a macro `#if` whose
  `{` belongs on the same line.
* CI: GitHub Actions builds and tests every push with gcc, clang and the
  sanitizers.

* An `END` entry point that belongs to a DSECT is an error (`ZA3012`); it
  used to assemble into an END record with ESDID 0 that the binder cannot
  resolve. Before emission, both object writers validate what the format can
  hold (class lengths and the entry offset in 32 bits, relocations inside
  their class, the entry resolvable to an ESD; the ELF writer also bounds
  checks the relocation field before writing it), so an assembler bug is an
  error, not a silently truncated object.
* MCP server: per-request memory is given back (the server is a long lived
  process; verified leak-free with an ASan/LSan build over hundreds of
  requests), a request line longer than 16 MB is rejected with a JSON-RPC
  error instead of being buffered, and a failed parse no longer leaks its
  partial tree. New `tests/test_mcp.c` (77 checks: JSON parser, base64, the
  workspace resolver, whole requests).
* MCP server: file names are limited to a **workspace**, the directory of the
  server binary. Every file name a tool call gives (`path`, `output`,
  `dwarf_output`, `output_dir`, `macro_dir`, `copy_dir`, `include_dirs`,
  `macro_dirs`) must resolve to inside it; relative names are relative to the
  workspace root, `..` and symlinks cannot lead out. Setting the environment
  variable `ZOSASM_UNRESTRICTED` lifts the limit (any file name, relative
  names relative to the working directory, as before). Closes an
  arbitrary-file read (the diagnostics quote source lines) and an
  arbitrary-file write through the tool arguments.
* Robustness against hostile source (a diagnostic, never a crash, hang or out
  of memory): a section is limited to 2 GB - 1 bytes because GOFF class lengths
  and text offsets are 32 bit (`ZA2087` for `DS`/`DC`, `ZA3053` for `ORG`,
  `ZA3054` when several statements grow a section past the limit); macro and
  `#if` expressions nest at most 200 levels (`ZA4100`, `ZA4030` - deeply nested
  parentheses used to overflow the C stack); macro numbers that do not fit in
  64 bits and `substr`/`repeat` counts that overflow are errors instead of
  wrapped values; "did you mean" suggestions skip tokens longer than 64
  characters; `hlasm2za` rejects an `ICTL` whose continuation column lies
  outside begin..end (it used to make the continuation padding loop run near
  2^64 iterations).
* `-g`, `-gdwarf`, `-gdir`, `-gmap`, `-gdbgpath`: debug information as z/OS `as -g`
  writes it (a `.dbg` file with DWARF 4 plus the `D_@@ADBG_*` link module appended to
  the object); MCP `zosasm_assemble` parameters `dwarf*`; guide chapter 21, DESIGN 21.
  Checked with `ld`, `dwarfdump` and LLDB on z/OS, not with dbx.
* `-g`: a DSECT becomes a DWARF structure type, and a `USING` of a DSECT with a base register
  gives a variable of that type (`frame variable` in LLDB) over the range it holds (dependent
  `USING`s too); `EQU` values are constants in the DWARF.
* `-dump` shows every module of a file; the readers use the first module.

## 0.1.0 - 2026-11-17

First versioned release.

Tools (all in `build/`):

* `zosasm`: assembler for an HLASM-like language (`.za`), output z/OS GOFF
  objects or, with `-elf`, ELF64 s390x objects for Linux on Z. Instruction
  table generated from the z/Architecture Principles of Operation (2369
  entries). C-style preprocessor, built-ins (`sizeof`, `offsetof`, `ASSERT`),
  quoted symbols, `USING` forms, DXD/CXD/Q/J/R, DC/DS modifiers, literal
  pools, listing (`-l`), JSON diagnostics, ZSYM debug data (`-gz0/1/2`),
  `-dump`, `-disasm` (exact round trip), `-unpack`, `-strict-registers`.
* Macro language: C-like `#macro` with `#if/#while/#for/#switch`, `-M`
  libraries, `-macro-steps`.
* `hlasm2za`: HLASM to zosasm converter incl. `MACRO`..`MEND` with structure
  recovery and open-code conditional assembly (`OPENCODE` macro).
* `zafmt`: source formatter.
* `zosasm-mcp`: MCP server for AI agents.

Verification: an oracle run against IBM HLASM 6.0 and the binder on z/OS
(26 probe programs, objects identical, mixed links with `/bin/ld`, one real
program identical). Not yet compared with IBM: USING forms, ORG, `L.n`, CU,
CCW, DFP, 32-bit expression arithmetic, ADATA/MNOTE and more (guide section
14).

Documentation: `docs/GUIDE.md` and `docs/zosasm-users-guide.pdf`, `DESIGN.md`,
`MACRO.md`, `PLAN.md`.
