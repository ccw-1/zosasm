# zosasm plan

Status key: [x] done, [-] in progress, [ ] open.

## Phase 0 - infrastructure (this checkpoint)

- [x] repo, Makefile, DESIGN.md
- [x] C99 port of everything below (the Go version was dropped with the
      history rewrite); `make test` green with gcc and clang, ASan/UBSan clean
- [x] `internal/ebcdic`: IBM-1047 tables generated from iconv, tests
- [x] `internal/goff`: writer (HDR ESD TXT RLD LEN END, continuation
      records) and reader/dumper, round-trip tests
- [x] instruction table generated from the Principles of Operation
      (`tools/pop2json.py`, 2369 entries incl. extended mnemonics), field
      encoder incl. RXB, tests
- [x] `internal/diag`: spans, codes, notes, help, excerpts, suggestions
- [x] `internal/asm` vertical slice: reader, operands, expressions,
      symbols, sections/classes/LOCTR, DC/DS/EQU, section directives, GOFF
      emission
- [x] `cmd/zosasm`: `zosasm [-o out.o] [-dump FILE.o] [-color] [-Werror] [-max-errors N] in.za`

## Phase 1 - useful assembler

- [x] machine instructions: operand classes, `USING`/`DROP` base/displacement
      resolution, relative branches and `LARL`/`BRASL` (+ RLD relative
      immediate for other elements), extended mnemonics, `CNOP`, `ORG`
- [x] `DC` types (all forms incl. S SY G QY, bit lengths; CU, CCW, scale/exponent, decimal float and
      rounding modes: DESIGN.md 13)  C X B F H FD P Z A AD Y S V Q (+ E D L hex/binary float,
      CA/CE character sets), duplication/length/scale modifiers, multiple
      nominal values, `DS` with `0` boundary alignment
- [x] literals and `LTORG` (pool at END in the first CSECT)
- [x] preprocessor `src/pp.c` (`#include`, `#define`, `#if`, `#pragma`; DESIGN.md 8) replaces `COPY`
- [x] attribute references `L' T' S' I' D' O'`, `L'*` (K'/N' are macro language: ZA1032)

## Phase 2 - GOFF completeness

- [ ] `CATTR`: ALIGN, RMODE, EXECUTABLE/NOTEXECUTABLE, READONLY, LOAD/NOLOAD/
      DEFLOAD, FILL, PART, PRIORITY, MERGE/CAT binding
- [ ] `XATTR`: LINKAGE(OS|XPLINK), SCOPE(SECTION|MODULE|LIBRARY|EXPORT|IMPORT),
      REFERENCE(CODE|DATA), DATAREFERENCE, PSECT, DEFLOAD, ...
- [x] `ACONTROL`, `PUSH`/`POP`, `CEJECT`, `EXITCTL`, `REPRO`, `-diag json`, listing carets (DESIGN.md 4.7, 6, 15)
- [x] quoted symbols `"any name"` replace HLASM `ALIAS` (DESIGN.md 2; `-disasm` uses them)
- [x] `DXD`/`CXD`, `Q`/`J`/`R` constants, `REFERENCE(...,INDIRECT)` (DESIGN.md 4.4; RLD encodings unverified)
- [ ] `COM` details, deferred lengths (LEN record), XATTR `ATTRIBUTES`/`PSECT` output,
      XPLINK call descriptors, `B_IDRL` ident record
- [ ] END with entry point by name / ESDID+offset

## Phase 2b - proposals awaiting decisions

- [x] ZSYM Z1: symbolic information in a NOLOAD ADATA class, `-gz0/-gz1`, `-dump` (DESIGN.md 11)
- [x] ZSYM Z2: smart `-disasm` from the ZSYM statements, per-section fallback, `-generic` (DESIGN.md 11)
- [x] ZSYM tier 2 `-gz2`: original source files in `C_ADATAE9C2`, LZ4 block compression, `-unpack` (DESIGN.md 14)
- [x] labeled and dependent USING, USING ranges, qualified symbols (DESIGN.md 4.3)
- [x] `sizeof`/`offsetof`/`alignof`/`countof`, comparisons, `ASSERT`, preprocessor
      built-ins, `#(x)` stringify, `-reproducible` (DESIGN.md 12, 8)
- [x] scale/exponent modifiers, CU, CCW/CCW0/CCW1, decimal float, rounding modes and special
      values (DESIGN.md 13; not checked against IBM `as` yet)
- [x] `A(X-Y)` (RLD SUB items, also in `-disasm`; not checked against IBM `as` yet)
- [ ] bit-length modifiers (`L.n`)

## Phase 3 - quality

- [x] GOFF -> source converter (`-disasm`, DESIGN.md 10)

- [ ] register pair checks (even-odd GPR pairs, floating-point extended
      pairs) derived from the POP prose; the table has no pair information
- [ ] `-diag-format=json`, warning classes, `-Werror`
- [x] listing file (`-l`), symbol cross reference (DESIGN.md 15)
- [x] HLASM source -> zosasm source converter `hlasm2za` (DESIGN.md 16; macros, conditional assembly and `COPY` members are not converted)
- [x] ELF64 output for Linux on Z (`-elf`, `-ascii`, `-charset`; DESIGN.md 17; checked with binutils, not run on a machine)
- [x] MCP server `zosasm-mcp` for AI agents (tools, guide as resource; guide 18, DESIGN.md 18)
- [x] macro language M1 (C-like braces, `&` substitution, `-M` library, guide 19; MACRO.md, DESIGN.md 19; not compared with HLASM)
- [x] `hlasm2za` conversion of MACRO..MEND to `.zmac` with structure recovery (MACRO.md C1, sections 10 and 12), `-flat`, `-macro-dir`, `make test-maclib` (DESIGN.md 16.1; open: `#switch`/`#for`, `else if`, COPY/AREAD/PUNCH/AINSERT/OPSYN in macros)
- [ ] MCP: prompts, restrict `path`/`output` to a directory
- [ ] ELF: 31-bit class, debug information, run under qemu-s390x
- [ ] `hlasm2za`: macro expansion of common IBM macros (`SAVE`, `RETURN`, `WTO`...), `EQUREGS`, DC forms zosasm lacks
- [ ] golden tests, fuzzing of the reader/expression parser
- [ ] documentation of every directive in `docs/`
- [ ] (macro language: covered by the preprocessor, see Phase 1)

## Reference material

* the instruction encodings in `tests/test_isa_pop.c` were written out by hand
  from the POP formats; the POP PDF is `SA22-7832-14.pdf`.
* `dev/src/debug/goff/testdata/hello.goff` is a real clang GOFF object;
  `hlasm-dwarf/dwemb.asm` is real HLASM source (assembles with zosasm).
* `/home/knoppix/pop/` (z/Architecture documentation, `zcg/`, SA22-7832-14).

## Oracle checks (run on z/OS host igor, 2026-11-15/16)

Facts in the GOFF writer/mapping that come from the manual and must be
confirmed against objects produced by real HLASM (`as -mgoff`) and by the
binder. The oracle is `as -mgoff` (HLASM R6.0) on host `igor`; sources
`testdata/oracle/*.asm`; `tools/oracle.sh` compares per statement, results are
checked by `tests/test_oracle.c`.

| # | Question | Assumed | Verified |
|---|---|---|---|
| 1 | HDR: architecture level and module properties | level 1, no properties | HLASM writes 0 in the architecture level; not changed |
| 2 | END record count counts logical records incl. HDR/END | yes | no |
| 3 | ED for a plain CSECT: class `B_TEXT`, alignment doubleword, RMODE from RMODE stmt | as DESIGN.md | no |
| 4 | LD emitted for the CSECT name, offset 0, scope module | yes | no |
| 5 | A-con to a local label: referent = class (ED, or PR), R-ID = ED, offset as addend in text; to ER: referent label | yes (as `hello.goff` and the spec) | **yes**: a local label is a LABEL referent (g1/g2/g3), fixed in `emit.c` |
| 6 | Same-class relative branch: resolved by assembler, no RLD (an A-con to a label in the same ED still needs an RLD) | yes | no |
| 7 | Relative-immediate (`BRASL X`) to another ED/ER: ref type 6 | yes | **yes** for an EXTRN (only the addend is stored; linked and run) |
| 8 | V-con: ER, ref type 0, AMODE-sensitive bit | yes | no |
| 9 | `CATTR PART(p)`: PR with name space 2, text attached to PR | yes | no |
| 10 | XATTR `SCOPE(EXPORT)` -> binding scope 4, name space 3 on PR | per manual | no |
| 11 | Name > 8 chars in ESD continuation records exactly as written | yes | no |
| 12 | Text longer than 56 bytes: continuation record layout | yes | no |
| 13 | Pad bytes of fixed records are zero | yes | no |
| 14 | Binder (`ld`) accepts our minimal module (SD/ED/LD/TXT/END) | untested | **yes** (`xlc -o prog main.o a.o b.o`, run) |
| 15 | EBCDIC vs ASCII `DC C'..'` default matches HLASM (`CODEPAGE`) | EBCDIC 1047 | **yes** (also `^ [ ]`); HLASM option CODEPAGE(1148) on igor |

Also checked (not in the table): the operand rules of the instructions
(`isa/oracle_rules.json`, ZA2080-2084), the DC/DS length windows, value
ranges, blanks in nominals, P/Z digits, float lengths (GUIDE 5.1.2), SD flags
and entry AMODE/RMODE statements.

Still open: ER parent section (HLASM: the section current at the EXTRN),
HDR/END fields, COM, CATTR PART/XATTR forms, V-con, long names, text over 56
bytes, `A(EXTA-A)` referent, extra `B_IDRL`/`B_PRV` classes of HLASM, 32-bit
expression arithmetic of HLASM, literals, USING forms, ORG, `L.n`, `CU`, `CCW`,
decimal float, macro-less directives (ACONTROL, PUSH/POP, OPSYN, PUNCH, COPY,
AREAD, AINSERT, `&SYS*`).

How to run the oracle:

1. HLASM source of at most 71 columns, second statement `PRINT DATA` (the
   listing shows only 8 bytes of a DC otherwise),
2. `tools/oracle.sh testdata/oracle/X.asm` (host `igor`, see the header of the
   script); `ZORACLE_REUSE=1` repeats only the zosasm side,
3. read the `same / different / hlasm-error` lines; `warn` lines list
   HLASM warnings that zosasm may not give.
