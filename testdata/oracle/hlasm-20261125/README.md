# HLASM probes, 2026-11-25

These sources were assembled with IBM HLASM R6.0 (PTF UO07449) on z/OS
03.01.00. They are the evidence for the HLASM comparison in the 2026-11-25
review and for the regression values in `tests/test_asm_basic.c`,
`tests/test_dc_mod.c`, `tests/test_xd.c` and `tests/test_convert.c`.

Assemble a file with `tryhlasm.sh` from the go-dev root:

```sh
cd testdata/oracle/hlasm-20261125
/home/knoppix/go-dev/tryhlasm.sh igor pequ.asm
```

The script writes `pequ.lst` (converted to ASCII) and `pequ.o` (raw GOFF).
Read the object with `build/zosasm -dump pequ.o`.

The listings are not committed: they contain the HLASM job and system
headers. The expected values are in the tests and in the review report.

## Results that the fixes use

| Probe | What it shows |
|---|---|
| `pequ.asm` | `ALIAS EQU SRC` has length 8, `J1 EQU I1` has length 2; `MVC ALIAS,DST` is `D207` |
| `pstar.asm` | `DC 2A(*)` is `00000000 00000004`; `DC A(*,*)` is `00000000 00000004`; `DC 2S(*)` is `F010 F012`; `DC 2A(*+4)` is `0000001C 00000020` |
| `pstar2.asm` | `DC S(*,*)` is `F008 F00A`; two operands with a duplication factor advance independently |
| `pstar3.asm` | `*` is not allowed inside a quoted numeric constant (`ASMA074E`) |
| `ptalign.asm` | a 31-type constant matrix; only `SY` and `QY` differ from zosasm (no alignment in HLASM) |
| `ps.asm`, `pt.asm`, `pu.asm`, `pw.asm` | `S` is halfword aligned, `SY` and `QY` are not; `CCW`, `CCW0` and `CCW1` match |
| `ph.asm` | `DC 0D`, `DC 0F` and `DC 0X'0'` without a value are accepted and force alignment |
| `pb1.asm`, `pb2.asm` | `CNOP 0,32` is accepted with `ASMA500W`; `CNOP 0,2` is `ASMA159S` |
| `pc.asm` | `ORG *,8,2` rounds to 8 and adds 2 |
| `pd.asm` | an unnamed `DSECT` is accepted |
| `pe.asm` | `EQU` with 5 operands is accepted; type A, assembler type GR, program type 7 |
| `pattr.asm` | `XATTR ATTRIBUTES(L)` is accepted |
| `pf.asm`, `pf2.asm`, `pg.asm`, `pps.asm` | `PSECT` on an external symbol, the `START` address and a valid internal PSECT; HLASM writes the class offset 4096 and sets the LD's ADA field to the PSECT class ED |
| `pi.asm` | odd `FPR` operands in `LD`, `CD`, `LDR` and `STD` are accepted, like zosasm |
| `pj.asm` | literal duplication (`=2F'0'`) matches |
| `pk.asm` | `ENTRY A,B`, `EXTRN X,Y` and `WXTRN W1,W2` match |
| `plc2.asm` | `LOCTR` text matches |
| `po.asm`, `ld.asm` | `LTORG` in a DSECT is accepted |
| `pp.asm` | `DC 10EBP(7)L2'12'` is accepted; `DC AP(7)...` is rejected |
| `pq.asm`, `plen0.asm`, `ptyp.asm`, `plit.asm` | `EQU` ranges (`ASMA182E`), type 0 and length 0 are valid, literals in `EQU` are rejected |
| `pv2.asm` | `USING` with several base registers and `USING 0,0` match |
| `px.asm` | `DXD`, `Q`, `QD`, `QY`, `J`, `JD`, `CXD`, `R` and `RD` ESD and RLD match |
| `py.asm` | `A(X-Y)`, `A(X-*)` and `A(*-Y)` RLD actions match; the class-origin addend differs |
| `pz.asm` | `DD`, `ED`, `LD`, `EB`, `DB` and `LB` are byte identical |
| `pfw.asm`, `pfw2.asm` | forward-reference `EQU` (value and length) is accepted; `ALIAS EQU SRC` before `SRC` has length 8 |
