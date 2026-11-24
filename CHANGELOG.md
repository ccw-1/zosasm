# Changelog

Versions follow semantic versioning (`src/version.h`); releases are git tags
`vX.Y.Z`. Things not compared with IBM's tools are marked in the User's Guide
(section 14).

## Unreleased

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
  release a module and a reporter. The one-shot programs still rely on process
  exit; `make test-free-asan` runs the destructor test under ASan/UBSan.
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
  `#if` overflow, `DXD` length overflow, and the CLI output paths (`-o -`,
  `/dev/null`, `-g` aliases, hidden `-l`/`-o` collisions).

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
