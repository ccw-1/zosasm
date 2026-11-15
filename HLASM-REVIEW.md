# HLASM feature review (for decisions)

Source: *HLASM V1R6 Language Reference* (`asmr1024.pdf`, chapter 5 and the
DC section) and *Programmer's Guide*. Only GOFF output is considered.
zosasm is not bound to HLASM compatibility, so each row is a free decision:
**Do** (worth building), **Redo** (the need is real, the HLASM form is not
the best one), **Skip**.

Proposed verdict is in the last column; "?" marks a question for you.

## Assembler instructions not in zosasm

| Instruction | What it really does | Proposed |
|---|---|---|
| `ACONTROL` | Changes assembler options in the middle of the source: `AFPR` (which FP registers are legal), `CA(ccsid)`/`CE(ccsid)` (code page of `CA`/`C` constants, only for `DC`), `COMPAT` (macro/case compatibility), `FLAG` (message levels), `MACHINE`/`OPTABLE` (instruction set level), `RA2` (alignment warnings), `TYPECHECK` (register type checks), `ILMA`, `LIBMAC`, `CU`. | **Redo** as `#pragma` and command line: `-march` (reject instructions newer than a machine level; needs facility data in the ISA table), code page of character constants (zosasm is fixed to IBM-1047 today) ?. Rest **Skip**. |
| `ADATA` | Writes a user record into the *associated data file* (a side file of the assembler). Only checked and listed unless the ADATA option is on. | **Skip**. ZSYM (ZSYM.md) is the GOFF-native replacement, and it is generated, not hand written. |
| `AINSERT` | Puts a record into the input stream, from a macro (queue front/back). | **Skip** (macro language is out; the preprocessor `#include`/`#define` covers it). |
| `CCW`, `CCW0` | 8 byte format 0 channel command word: command code, 24 bit data address, flags, count; aligned on a doubleword; the symbol gets length 8. Used by EXCP/EXCPVR I/O code. | **Done** (DESIGN.md 13). |
| `CCW1` | Same, format 1, 31 bit data address. | **Done** (DESIGN.md 13). |
| `CEJECT n` | Conditional page eject in the listing. | **Skip**; accept as no-op like `EJECT` (no listing yet). |
| `CXD` | Reserves a fullword that the binder fills with the total length of all external dummy sections (the class `B_PRV`). | **Done** (DESIGN.md 4.4). |
| `DXD` | Defines an *external dummy section*: a named piece of **pseudo-register** storage (size and alignment from a DS-like operand) that the binder merges by name over all modules and allocates at run time. Referenced through `Q(name)`. In GOFF it is a PR in class `B_PRV`. This is how reentrant static storage (LE, PL/I) is assigned. | **Done** (DESIGN.md 4.4). |
| `EXITCTL` | Sets four parameters for user exits of the assembler. | **Skip**. |
| `MNOTE sev,'text'` | User defined message with severity, mostly from macros and conditional assembly. | **Redo**: `ASSERT expr,'text'` (ZSYM.md 7) fails the assembly when an assembly-time condition is false, and `#error`/`#warning` already exist. No `MNOTE`. |
| `OPSYN` | Gives an operation code another name, or deletes it (used to rename or replace instructions in macro libraries). | **Skip**; `#define` on the operation field already renames. |
| `PUSH` / `POP` | Save and restore the `PRINT`, `USING`, `ACONTROL` state on a stack (macros use it to change the USING environment temporarily). | **Skip** `PRINT`/`ACONTROL`. `USING` state: **?** (a real need in long routines, but a scoped form is nicer than a stack: see `USING` below). |
| `REPRO` | Copies the next record unchanged into the *object deck*. The manual says: do not use with GOFF. | **Skip**. |
| `*PROCESS` | Options inside the source. | **Skip** (`#pragma`); a `*` in column 1 is a comment already. |
| `ICTL`, `ISEQ` | Column layout of the source. | zosasm has no columns: accepted and ignored today; **Skip** (could become an error later). |
| `ALIAS` | Long or odd external names. | **Done differently**: quoted symbols `"a.b"`. |
| `COPY`, `AREAD`, `MEXIT`, `AEJECT`, `ASPACE`, macro and conditional assembly | Macro language. | **Skip** (preprocessor). |

## Language features the manual describes and zosasm lacks

| Feature | What it is | Proposed |
|---|---|---|
| **Labeled `USING`** | `W1 USING WORK,2` gives the mapping a name, so the same DSECT can be mapped twice (two registers) and the fields are used as `W1.FIELD`. | **Done** (DESIGN.md 4.3). The qualified name `W1.FIELD` is a new expression form. |
| **Dependent `USING`** | `USING RECMAP,INREC` maps a DSECT onto an area that is itself covered by another USING; no new register, the address is computed from the base of the first USING. Ends with it. | **Done**. |
| `USING` range, `USING (a,b),reg` | Limit of the covered area (default 4096), several bases. | **Done**. |
| Scoped `USING` | (not in HLASM) | **?** `USING ... ENDUSING`-style block as an alternative to `PUSH`/`POP`. |
| `Q`, `QY`, `QD` constants | Offset of an external dummy section / DSECT / PART in the pseudo-register area; the binder fills it (RLD type Q). | **Done** (`QY` not yet). |
| `J`, `JD` constants | Length of a DXD/class/DSECT, filled by the binder (GOFF LEN record). | **Done** (as RLD length items; LEN records not needed). |
| `R` constant | Address of the PSECT (private read/write area) of a symbol, for reentrant code (`V` is the entry address, `R` the data area). | **Done** (DESIGN.md 4.4); `XATTR PSECT` is parsed but not written yet. |
| `S`, `SY` constants | A base-displacement value computed from a USING (2 or 3 bytes). | **Maybe**: little use outside old code. |
| `CU` (UTF-16), `G` (DBCS) constants | Text in other encodings. | `CU` **Done** (UTF-16BE only, DESIGN.md 13); `G` **Skip**. |
| Scale `S`, exponent `E` modifiers | `FS8'35.92'`, `EE3'3.414'`, fixed/float scaling. | **Done** (DESIGN.md 13). |
| Decimal float `DD ED LD` (and `DB EB LB` binary float) | Hex float (E D L) exists; the other formats do not. | **Done** (DD ED LD with all rounding modes, special values, LQ; DESIGN.md 13). |
| Program type `P(...)` in DC and `EQU` | A 32 bit user tag on a symbol, no effect on code. | **Skip** (ZSYM can carry such tags if wanted). |
| Relocatable difference `A(X-Y)` between sections | Needs RLD action SUB for the binder. | **Done** (DESIGN.md 13). |
| `XATTR ATTRIBUTES(label)` | Points the ESD at extra attribute data (ESDID+offset of a label). | **Maybe**. |
| `XATTR REFERENCE(DIRECT\|INDIRECT)` | The "indirect reference" attribute bit. | **Do** (one flag). |
| `COM` | Common section: no object code, size is the maximum over modules. | Exists; check behaviour against the manual text (location counter continuation). |
| `EQU` 4th operand (program type), `L'`, `T'` forms | See above. | **Skip** except what exists. |
| `K'`, `N'`, `SYSATTRP`, `&SYS...` | Macro language only. | **Skip**. |

## New, not from HLASM (ZSYM.md 7)

`sizeof(x)`, `offsetof(S,f)`, `alignof(x)`, `countof(x)`, `ASSERT`,
preprocessor `__COUNTER__`, `__BASE_FILE__`, `__INCLUDE_LEVEL__`,
`__ZASM_VERSION__`, a stringify operator, `-reproducible`.

## Suggested order

1. **ZSYM Z1-Z2** (ZSYM.md): symbolic object, smart reversal.
2. `DXD`/`CXD`/`Q`/`J` (+ LEN records): completes the GOFF data model.
3. Labeled and dependent `USING`: changes how ZSYM records USING, so do it
   before Z1 writes its format.
4. Scale/exponent modifiers, `CU`, `A(X-Y)` (SUB), `R`, `CCW*`.
5. `sizeof` family and `ASSERT` (Z3) - **done**, DESIGN.md 12.
