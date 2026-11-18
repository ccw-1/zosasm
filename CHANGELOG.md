# Changelog

Versions follow semantic versioning (`src/version.h`); releases are git tags
`vX.Y.Z`. Things not compared with IBM's tools are marked in the User's Guide
(section 14).

## Unreleased

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
