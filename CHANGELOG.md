# Changelog

Versions follow semantic versioning (`src/version.h`); releases are git tags
`vX.Y.Z`. Things not compared with IBM's tools are marked in the User's Guide
(section 14).

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
