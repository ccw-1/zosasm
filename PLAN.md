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

- [x] `CATTR`: ALIGN, RMODE, EXECUTABLE/NOTEXECUTABLE, READONLY, LOAD/NOLOAD/
      DEFLOAD, FILL, PART, PRIORITY, MERGE/CAT binding
- [x] `XATTR`: LINKAGE(OS|XPLINK), SCOPE(SECTION|MODULE|LIBRARY|EXPORT|IMPORT),
      REFERENCE(CODE|DATA), DATAREFERENCE, PSECT, DEFLOAD, ... (ATTRIBUTES output open)
- [x] `ACONTROL`, `PUSH`/`POP`, `CEJECT`, `EXITCTL`, `REPRO`, `-diag json`, listing carets (DESIGN.md 4.5, 6, 15)
- [x] quoted symbols `"any name"` replace HLASM `ALIAS` (DESIGN.md 2; `-disasm` uses them)
- [x] `DXD`/`CXD`, `Q`/`J`/`R` constants, `REFERENCE(...,INDIRECT)` (DESIGN.md 4.4; RLD encodings unverified)
- [ ] `COM` details, deferred lengths (LEN record), XATTR `ATTRIBUTES`/`PSECT` output,
      XPLINK call descriptors, `B_IDRL` ident record
- [x] END with entry point by name / ESDID+offset

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
- [x] bit-length modifiers (`L.n`)

## Phase 3 - quality

- [x] GOFF -> source converter (`-disasm`, DESIGN.md 10)

- [ ] register pair checks (even-odd GPR pairs, floating-point extended
      pairs) derived from the POP prose; the table has no pair information
- [x] `-diag-format=json`, `-Werror` (warning classes open)
- [x] listing file (`-l`), symbol cross reference (DESIGN.md 15)
- [x] HLASM source -> zosasm source converter `hlasm2za` (DESIGN.md 16; macros and open-code conditional assembly are converted, `COPY` members are not)
- [x] source formatter `zafmt` (guide chapter 20, DESIGN.md 21)
- [x] ELF64 output for Linux on Z (`-elf`, `-ascii`, `-charset`; DESIGN.md 17; checked with binutils, not run on a machine)
- [x] MCP server `zosasm-mcp` for AI agents (tools, guide as resource; guide 18, DESIGN.md 18)
- [x] macro language M1 (C-like braces, `&` substitution, `-M` library, guide 19; MACRO.md, DESIGN.md 19; not compared with HLASM)
- [x] `hlasm2za` conversion of MACRO..MEND to `.zmac` with structure recovery (MACRO.md C1, sections 10 and 12), `-flat`, `-macro-dir`, `make test-maclib` (DESIGN.md 16.1; the macro forms of COPY/AREAD/PUNCH/AINSERT/OPSYN are converted; other system macros still need a library)
- [x] MCP: restrict `path`/`output` to a workspace (the server directory;
      `ZOSASM_UNRESTRICTED` lifts the limit; DESIGN.md 18). Open: MCP prompts
- [x] ELF: debug information (DWARF in `.debug_*`, `readelf`/`ld`/`gdb` under qemu-s390x)
- [ ] ELF: 31-bit class, run under qemu-s390x
- [ ] `hlasm2za`: macro expansion of common IBM macros (`SAVE`, `RETURN`, `WTO`...), `EQUREGS`, DC forms zosasm lacks
- [x] golden tests (`golden.dump.txt`, `golden-rel.dump.txt`, `golden.elf` and `golden.dbg`), fuzzing of the reader/expression parser
- [ ] documentation of every directive in `docs/`
- [ ] (macro language: covered by the preprocessor, see Phase 1)

## Phase 5 - CBLOCK inline scalar blocks

`#pragma cblock` implements the inline scalar C-like block designed in
`PLAN-CBLOCK.md`. It is implemented on branch `ccw-inc`, not merged to main.
It is not a callable function: the block falls through, reads and writes
explicitly bound registers, and uses an optional caller-provided temp area.
The implemented subset covers 32-bit and 64-bit integers, scalar locals,
expressions, casts, assignments, `if`/`else`, `while`, `do`, `for`,
`break`/`continue`, short-circuit `&&`/`||`, `?:`, `++`/`--`, and the
CBLOCK listing and oracle checks. It compiles to ordinary assembler
statements. Pointers, arrays, floating point, calls, recursion, `switch`,
`goto`, C variable DWARF and a full `STMG`/`LMG` save mode stay out of scope.

Docs: `docs/CBLOCK.md`, design and milestones in `PLAN-CBLOCK.md`.

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
| 3 | ED for a plain CSECT: class `B_TEXT`, alignment doubleword, RMODE from RMODE stmt | as DESIGN.md | **yes** (`tools/oracle_obj.sh`, probes st1 am1): RMODE-only gives AMODE 31/64 |
| 4 | LD emitted for the CSECT name, offset 0, scope module | yes | no |
| 5 | A-con to a local label: referent = class (ED, or PR), R-ID = ED, offset as addend in text; to ER: referent label | yes (as `hello.goff` and the spec) | **yes**: a local label is a LABEL referent (g1/g2/g3), fixed in `emit.c` |
| 6 | Same-class relative branch: resolved by assembler, no RLD (an A-con to a label in the same ED still needs an RLD) | yes | no |
| 7 | Relative-immediate (`BRASL X`) to another ED/ER: ref type 6 | yes | **yes** for an EXTRN (only the addend is stored; linked and run) |
| 8 | V-con: ER, ref type 0, AMODE-sensitive bit | yes | ER and ref type yes (st3); the AMODE bit is not compared |
| 9 | `CATTR PART(p)`: PR with name space 2, text attached to PR | yes | **yes** (cat1, st4): merge, no PR fill, CATTR defaults ALIGN 3 / RMODE 24 / NONREUS |
| 10 | XATTR `SCOPE(EXPORT)` -> binding scope 4, name space 3 on PR | per manual | **yes** (st4: SCOPE/LINKAGE/REFERENCE on parts and entries) |
| 11 | Name > 8 chars in ESD continuation records exactly as written | yes | no |
| 12 | Text longer than 56 bytes: continuation record layout | yes | no (only the objects of `-dump` are compared; text is compared as bytes) |
| 13 | Pad bytes of fixed records are zero | yes | no |
| 14 | Binder (`ld`) accepts our minimal module (SD/ED/LD/TXT/END) | untested | **yes**: `ld` and `xlc` accept our objects like those of `as`; `tools/oracle_ld.sh` links all four mixes of HLASM/zosasm `liba`+`chk` (entries, V/A/weak constants, CATTR class, LOCTR, second CSECT, DSECT) with a C main and runs them: identical results (503 of 511, check 4 fails the same way with IBM's objects) |
| 15 | EBCDIC vs ASCII `DC C'..'` default matches HLASM (`CODEPAGE`) | EBCDIC 1047 | **yes** (also `^ [ ]`); HLASM option CODEPAGE(1148) on igor |

**Reactivated against pok56 (2026-11-24).** `../../tryhlasm.sh` uploads a
source, runs `/bin/as -a=NAME.lst -m goff -o NAME.o NAME.asm`, and fetches
the listing (ASCII) and the raw GOFF object now. `tools/ibm_compare.sh
SRC.asm [HOST]` runs it and compares the object (`oracle_obj.py`) and the
per-statement listing bytes (`oracle_cmp.py`). New probes:
`dfpccw` (DFP `ED/DD/LD`, `CCW/CCW0/CCW1`, `CU`, `BL.n`/`XL.n`),
`float2` (HFP, IEEE binary, DFP, scale and rounding), `chars`
(`C/CA/CE/CU`) and `reloc2` (`A(SECOND-FIRST)`, `A(FIRST-*)`,
`A(*-FIRST)`). All have identical object structure **and identical
per-statement listing bytes**. The earlier successful probes were rechecked:
`am1 cat1 cn1 cn2 com1 dc2b er1 g1 g1x g2 g3 lc1 lc2 lt1-lt4 st1-st9`
are structurally identical; `st3`/`st6`-`st9` cover `Q`, `J`, `QY`, `R`,
`CXD` and the RLD SUB case. Listing differences on `st3`/`g1`/`er1` are
HLASM listing facts (8-byte object field plus continuation lines, literal
pool rows numbered by HLASM, long DATA truncated), not object differences.
`DC A(2147483647+1)` and the division form are **rejected by HLASM**
(`ASMA075E arithmetic overflow`) while 64-bit zosasm accepts them; any valid
32-bit expression keeps the same low 32 bits.
Also checked (not in the table): the operand rules of the instructions
(`isa/oracle_rules.json`, ZA2080-2084), the DC/DS length windows, value
ranges, blanks in nominals, P/Z digits, float lengths (GUIDE 5.1.2), SD flags
and entry AMODE/RMODE statements.

Object comparison (`tools/oracle_obj.sh`, 2026-11-16) is clean for all probes `g1 g1x g2 g3 dc2b cat1 cn1 cn2 com1 lc1 lc2 am1 st1-st9`
(except the accepted differences: B_IDRL, empty B_PRV, IDRL record, HDR level 0, END counts).
Literal pool order (by length 16/8/4/2/1) and `*` in literals found with the user's program `wr3.asm` (probes `lt1`-`lt4`, 0 differences); the program itself (GETMAIN, IHASAVER macros via `hlasm2za` + `-M`) assembles to an object identical to HLASM's and links and runs with `/bin/ld`.
ER rule found 2026-11-16 (`er1` probe): HLASM makes an ER for every EXTRN/WXTRN, parent = section current at the statement; done.
`tools/oracle_amb.sh NAME` (z/OS `amblist` on both objects): same content, only B_IDRL/IDRL/ESDID shift, END count, HDR level differ.
Still open: HDR/END fields (accepted: zosasm writes real count, level 1),
V-con AMODE bit, long names, text over 56 bytes, `A(EXTA-A)` referent, extra
`B_IDRL`/`B_PRV` classes of HLASM, LEN records (no probe emits one yet),
32-bit expression overflow (zosasm is 64-bit and accepts what HLASM rejects),
and macro-less directives not exercised here (ACONTROL, PUSH/POP, OPSYN,
PUNCH, COPY, AREAD, AINSERT, `&SYS*`). Verified on pok56 now: USING/ORG
(`st2`), `L.n`, `CU`, `CCW/CCW0/CCW1`, decimal float.

How to run the oracle:

1. HLASM source of at most 71 columns, second statement `PRINT DATA` (the
   listing shows only 8 bytes of a DC otherwise),
2. `tools/oracle_obj.sh testdata/oracle/X.asm` compares the OBJECT (ESD/TXT/RLD, no `PRINT DATA` needed; `-dump` of the fetched HLASM object), prints `N difference(s)`;
   `tools/oracle.sh testdata/oracle/X.asm` (host `igor`, see the header of the
   script); `ZORACLE_REUSE=1` repeats only the zosasm side,
3. read the `same / different / hlasm-error` lines; `warn` lines list
   HLASM warnings that zosasm may not give.

## Phase 4 - full conversion of SYS1.MACLIB (analysis 2026-11-21)

Run: `hlasm2za -macro-dir ../maclib_utf8 -o DIR` -> 2080 members, 2210 macros
converted, **8 not converted**, 119 warnings. 2076 members write a `.zmac`; 4 write
nothing. `make test-maclib` (compile + flat == structured) is green for what was
converted; it says nothing about the 8 or about HLASM equivalence.

### Where it falls short (every one of the 8, plus the 4 silent members)

| member | what the converter says | real cause | HLASM R6.0 on igor (probe, 2026-11-21) | class |
|---|---|---|---|---|
| IXGCON | `line 882: MACRO is not converted` (the whole 2430-line macro is lost) | line 881 is a comment (`*` in column 1) with a non-blank column 72; HLASM continues a comment line over the following record(s) while column 72 is non-blank. Line 882 `Macro Reference". Do ...` is the second line of that comment, hlasm2za reads it as the statement `MACRO` (nested definition, no MEND) | `as` assembles the member with ONE error (ASMA144E, unrelated); no "Missing MEND". A statement `Macro ...` (any case) in a body does start a nested definition (ASMA086S), so the comment rule is the only explanation | **converter bug (reader)** |
| IHBRDWRA | `cannot read the declaration "@IDS3270"` | `LCLB @IDS3270`: bare name (no `&`) starting with `@`. `declare_list` accepts a bare name only when it starts with a letter, `@ # $ _` are refused. The macro itself uses `&IDS3270` | no diagnostic at all for `LCLB @QQ` | **converter bug (small)** |
| CPF | `unexpected character '?' in a conditional assembly expression` (line 719) | `MNOTE 4, 'OWNER KEYWORD IGNORED...'`: a blank after the comma ends the operand field, the message operand is empty, the quoted text is a remark | the generated statement is passed through, ASMA163W "Operand not properly enclosed in quotes", no MNOTE message; the macro works | **converter bug (MNOTE)**; same family: IHBRDWRA line 2026 `MNOTE 4,SEQNO=&SEQNO INVALID - IGNORED'` (unquoted message) after the `@IDS3270` fix; probe u2: ASMA163W, statement passed through, no message |
| IGWRMRCD | `cannot read the prototype operand "!"` | prototype `IGWRMRCD  !  @T4C`: `!` is the operand, `@T4C` the remark | ASMA025S "Invalid macro prototype operand", macro still defined, expansion works (assembly rc 12) | **converter too strict**: drop the operand with a warning |
| IRRPICRX, CBRSMF | `MEND is missing` (line 110 / 2445) | NOT a loss: the real macro (59-92 / 112-2278) IS converted (`IRRPICRX.zmac`, `CBRSMF.zmac` exist). The PL/X text after MEND has a continuation line starting with blanks and the word `macro` (`macro IRRPIDID */`), read as a new MACRO that never ends | HLASM reads the member only up to the MEND of the named macro, never sees it | **false failure**, counted in the 8 |
| ERBSTSGT, ERBFITEM | `MEND is missing` | PL/X macro libraries (`%ERBFITEM:` / `MACRO KEYS(FLOAT /* ...`), not assembler | n/a | **not assembler**: classify and skip, not a failure |
| GDKICOB, HWTJICOB, IGGCSINP, IKJTMP3 | (silent, no `.zmac`) | COBOL / PL/I / C copybooks, no MACRO statement | n/a | **not macros**: list them in the summary |

So the 8 are: 4 real converter defects (IXGCON, IHBRDWRA, CPF, IGWRMRCD), 2 false
failures (IRRPICRX, CBRSMF: already converted), 2 non-assembler members (ERBSTSGT,
ERBFITEM). The "8 not converted" number is misleading; after the work below the
honest report is "N converted, 0 failed, 6 members that are not assembler
macros (listed)".

Other warnings in the run (not failures, listed so the plan covers them):
52 x `&X is not declared` (HLASM: ASMA003E at expansion, value null, same as the
converter's empty local: faithful), 44 x `sequence symbol not defined: branch
became #error` (HLASM reports at run time of the branch: faithful), 7 x `text after
the MNOTE message is ignored` (check against the MNOTE rule below), 5 x
`continuation mark on the last line` (check that they are real file ends, not
mis-split comments), 3 x `declaration has no &` (accepted), 1 x `*PROCESS`.

### HLASM facts established by the probes (probe sources kept in testdata/oracle/maclib/: m1 MNOTE/LCLB/prototype, m3-m6 nested MACRO spelling, c1 comment continuation, u1 undeclared variable, u2 unquoted MNOTE)

1. A comment line (`*` or `.*` in column 1) whose column 72 is non-blank continues on
   the next record; the continuation record is part of the comment whatever it
   contains (columns 1-15 not blank give ASMA144E but the record is still swallowed);
   the chain ends with the first record whose column 72 is blank (probe c1:
   `LR 1,2` and `LR 7,8` on continuation records generate no code, the later `LR 9,10`
   does).
2. `LCLB @NAME` is silently accepted.
3. `MNOTE sev,` followed by a blank, or an unquoted message: the generated
   statement is handed on, ASMA163W, no message. (`MNOTE 4,'..'` correct form is
   unchanged.)
4. A prototype operand that is not `&name[=default]` is ASMA025S but the macro is
   defined and usable.
5. Undeclared variable symbol: ASMA003E when executed, value null.
6. `Macro`, `macro`, `MACRO` as an operation inside a macro body always open a
   nested definition (ASMA086S "Missing MEND generated" when it has no MEND).

### Work items

- [x] **4.1 comment continuation (IXGCON)**. In `src/convert.c` all three places that
      take a comment line without looking at column 72 (`collect_stmts` ~l.501/504,
      the open-code loop ~l.685/692, `raw_comment` ranges at l.578/661) must swallow
      the following records while `has_cont_mark()` is true, and put them into the
      same comment (one `*` line per record, so line numbers and `-comment-macros`
      output stay aligned). Also check `src/fmt.c`/`zafmt`, which formats `.za` only
      (no change expected). Test: tests for `*` and `.*` with 1, 2 and 3 marked
      records, a marked record at EOF (existing warning), mark on a `.*` inside a
      macro, and the IXGCON macro converts and compiles.
- [x] **4.2 MNOTE**. Parse MNOTE operands like HLASM: operand field ends at the first
      blank outside quotes; when the message operand is missing or not a quoted
      string, convert the statement as a plain model line (it is emitted into the
      output, as HLASM does) and warn "MNOTE with an unquoted message: HLASM gives
      ASMA163W and no message". Check what `zosasm` does with such a generated
      `MNOTE` line (MNOTE is still on the open list in the notes): it must give a
      diagnostic, not crash. Re-check the 7 "text after the MNOTE message is ignored"
      sites with the same rule. Test: probes u2 and the `MNOTE 4, 'text'` form
      expand to the same output text as HLASM.
- [x] **4.3 declarations**. `declare_list`: accept `@ # $ _` as first character of a
      bare name (warn as now). Test: `LCLB @X` then `&X SETB 1`.
- [x] **4.4 prototype operands**. `parse_proto`: an operand that is not `&name`
      or `&name=default` is dropped with the warning "ignored (HLASM ASMA025S)"
      instead of failing the macro; the remark after it stays a remark. Test:
      IGWRMRCD converts and expands.
- [x] **4.5 trailers after MEND (IRRPICRX, CBRSMF)**. In member mode (`c->o.member`)
      a MACRO statement after a completed macro that reaches the end of the member
      without a MEND is "trailer text": do not count it as a failure, report it
      as a note ("text after the last MEND ignored from line N"). Do NOT stop at the
      first MEND: CBDZPARS, IGGENC, IEWBIND2... legitimately hold several top-level
      macros (inline-source members).
- [x] **4.6 classification and an honest summary**. `hlasm2za -macro-dir` detects
      PL/X macro members (first non-comment line `%NAME:`/`MACRO NAME(...` with `/*`
      comments, no assembler statements) and members without any MACRO statement,
      and prints "converted M macros from X members; not assembler: ERBFITEM,
      ERBSTSGT (PL/X); no macro definition: GDKICOB HWTJICOB IGGCSINP IKJTMP3;
      failed: none". Exit code 1 only for real failures. `make test-maclib` checks the
      numbers (expected after 4.1-4.5: 2214 macros = 2210 + IXGCON, IHBRDWRA, CPF, IGWRMRCD; 0 failed) and each
      listed non-assembler member by name so a new one cannot hide.
- [x] **4.7 regression**. Rerun `ZASM_MACLIB=../maclib_utf8 make test-maclib` (compile all,
      flat == structured, `#goto` count not worse than the 0 of the dispatch form),
      gcc + clang + ASan/UBSan (clean first, Makefile ignores flag changes), guide 16.4,
      DESIGN 16.1, wiki zosasm.md 11, STATUS numbers.
- [-] **4.8 (beyond "converted": equivalence)**. Nothing so far shows that a converted
      macro expands like HLASM; the structured/flat check only compares two
      conversions of the same macro. Oracle run on igor: for a sample of macros
      (all 2214 would need arguments), assemble `M args` with HLASM `as -a` using
      the real macro text and with `zosasm -E` using the `.zmac`, compare the
      generated statements and MNOTEs. Start with the 4 repaired macros, the 20 most
      called ones (GETMAIN, WTO, SAVE, RETURN, ...) and one per `-keep-goto`
      family. Needs a harness `tools/oracle_macro.sh` (generates the call, runs both
      sides, normalises `+`/ASMA lines). Decide with the user how far to go.

Order: 4.1, 4.2, 4.3, 4.4, 4.5, 4.6 (small, independent, each with tests), 4.7, then
4.8 as its own milestone. Expected effort: 4.1-4.6 about one session; 4.8 open-ended.

Result (2026-11-21): `hlasm2za -macro-dir` on SYS1.MACLIB = 2080 members, 2214
macros converted, 0 members failed, 122 warnings; not assembler (PL/X): ERBSTSGT
ERBFITEM; no macro definition: 46 copybooks. `make test-maclib`: compile failures
0, 4286 expansions compared, 0 differences, 0 `#goto`; `make test` gcc and
clang+ASan+UBSan clean (the clang run also showed `h + 70 - 3` pointer arithmetic
past a 69 byte array in `src/goff.c`, now `h + (70 - 3)`, no behaviour change).
4.8 so far: the expansions of IXGCON (169 statements), IGWRMRCD (397), IHBRDWRA
(34, with the OPTCD MNOTEs) and CPF (17, MF=L/E forms) equal HLASM R6.0's on igor
(`tools/oracle_macro_cmp.py`: name, operation and first operand of every generated
statement). The CPF branch with `MNOTE 4, 'text'` was not reached by those calls.
Still open in 4.8: a systematic run over more macros.
