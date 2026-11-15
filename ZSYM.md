# ZSYM: symbolic information in the object (design; Z1 implemented)

Status: **Z1 implemented** (writer, reader, `-gz0/-gz1`, `-dump`; DESIGN.md section 11),
Z2 (smart `-disasm`) and tier 2 (`-gz2`) are proposals. Where this text and
DESIGN.md 11 differ, DESIGN.md 11 is what the code does (operand templates
instead of RPN, scopes instead of section/class names).
zosasm has no compatibility duty towards HLASM, so this design is free to
change the language where that gives a better result (section 7).

## 1. Problem

A programmer writes relationships, the object file stores numbers:

```
LEN      EQU   *-START             -> a constant
         DC    AL2(END-START)      -> two bytes
         LA    1,L'REC             -> an immediate
         L     3,NEXT-HDR(,2)      -> a displacement
WORK     DSECT                     -> nothing at all (no ESD, no bytes)
```

`-disasm` therefore writes `DC X'0042'` and `L %r3,8(%r0,%r2)`. If the size
of the DSECT changes in the reversed source, every dependent number is wrong
and nothing tells the reader which numbers depended on what. The converter
has to be generic: it knows bytes, not intent.

Wanted: the object keeps the intent, so that

1. `-disasm` of an object zosasm wrote gives back the programmer's symbols,
   EQUs, DSECT layouts and expressions (a *smart* reversal that survives
   edits such as "make the DSECT bigger");
2. nothing of this is loaded at run time (the code image is identical with
   or without it) but it travels with the object into the program object;
3. debuggers can use the same data (DSECT layouts, EQU constants, lines).

## 2. Where to store it: the ADATA mechanism of GOFF

GOFF already has the right container (`_context/goff-format-knowledge.md`
section 8, *Associated Data*): a translator may add a class whose ED has
**loading behaviour NOLOAD**, unstructured text, binding algorithm CAT, and
write its own records into it; the binder keeps the records in the program
object, appends the records of every module to the same class and never maps
them into storage. The class name is `C_ADATA` plus the four hex digits of the
record type; types `X'E000'-X'EFFF'` are for non-IBM translators.

* record type **`X'E9C1'`** ("ZA" in EBCDIC): class **`C_ADATAE9C1`**,
  holds ZSYM (this document);
* record type `X'E9C2'`, class `C_ADATAE9C2`: optional original source text
  (tier 2, section 6).

The ED goes under the first SD of the module. Each ZSYM chunk is one GOFF
text record. Because the binder concatenates the classes of all modules,
chunks are self describing and a reader splits the stream into *module
blocks* (section 4). Nothing refers to ESD ids or absolute offsets: places
are named by (section name, class name, offset), so a block stays valid after
the binder moved the sections.

Why not DWARF: a DWARF `.debug_*` form would need a DWARF writer, and DWARF
cannot say "these two bytes are `END-START`". DWARF is a *consumer* of the
same data (section 8, phase Z5), not the carrier.

Why not "embed the source text": the original text says nothing about whether
the bytes still match it (patched object, linked object), has no symbol table
for tools and ties the object to one syntax. It stays available as the
optional tier 2.

## 3. What is recorded (the overlay)

The assembler already has everything in its model (symbols, `Expr` trees,
USING table, layout). ZSYM serializes the part the GOFF object cannot hold.

### 3.1 Symbols

One record per source symbol that matters (all by default):

| field | meaning |
|---|---|
| name | as written, decoded (quoted symbols) |
| kind | LABEL, EQU, SECT (CSECT/RSECT/START/COM), DSECT, FIELD (symbol of a DSECT), EXT, WEXT, ENTRY, PART, DXD |
| place | (scope, class, offset) for LABEL/FIELD; value (signed 64 bit) for EQU, plus the base symbol for a relocatable EQU |
| attributes | `L'`, `T'` (letter), `S'`, `I'`, count (duplication factor of the DS, so arrays are known), program type |
| size | DSECT / section / class piece: high-water mark of the location counter (= `sizeof`) |
| def | statement id of the definition |

Symbols that also have an ESD (SD, LD, ER, PR) are recorded by name only; the
ESD carries the rest.

### 3.2 Statements

One record per statement that has an effect, **in source order** (after the
preprocessor): INSN, DC, DS, EQU, ORG, USING, DROP, LTORG, CNOP, LOCTR/section
switches (`CSECT`, `DSECT`, `COM`, `CATTR`...). A record has

```
kind, scope, class, offset, length          where its bytes are (final layout)
crc32 of its bytes, relocation fields masked        validity check
file id, line                                       line table, built in
operands                                            see below
```

Operands are stored **structured**, with symbols by id:

* an *expression* is a small RPN list: `NUM v`, `SYM id`, `LOC delta`
  (the location counter, relative to the statement start; for DC the offset
  of the constant), `ATTR letter sym`, `CALL builtin argc`, `OP + - * / neg`;
* an operand that is not an expression (register, mask, string, packed or
  float nominal value, `D(L,B)` shape) is kept as **opaque text**. This
  limits the work in the first version and costs nothing, because such
  operands do not depend on other symbols;
* for an instruction only the operands are stored, not the mnemonic: the
  converter decodes the bytes anyway and writes operand *k* from the record
  instead of from the number;
* DC/DS keep dup factor, type, length, scale/exponent as expressions and the
  nominal values as expression or text, so `DC AL2(END-START)` comes back as
  such instead of `X'0042'`, `DC CL8'TEXT'` instead of hex, `DS 0H`/`DS CL20`
  instead of zero bytes;
* USING/DROP keep their operands, so the converter can print `USING WORK,2`
  and then `L 1,FIELD` instead of `L 1,8(0,2)`.

Statements in a DSECT or DXD have no bytes at all; their record carries the
offset inside the dummy section. This is the only way the object can hold a
DSECT, and it makes a layout like `WORK` editable again.

### 3.3 Not recorded

Comments and blank lines, the macro/`#define` history (tier 2 can), listing
control, anything the ESD/TXT/RLD data already says exactly.

## 4. Format

```
chunk   = 'Z' 'S' version:u8 kind:u8 length:u32 payload crc32:u32
kind    = 1 MODULE | 2 SYMS | 3 STMTS | 4 END
```

* Big endian fixed header, payload in LEB128 varints and a string pool, so a
  chunk is about the size of the source's statements. No pointers: records
  refer to each other by index.
* `MODULE` (first): producer and version, ZSYM level, file table, scope table
  (scope id -> SD name, ED class name, kind), string pool.
* `END` closes the block; a reader that finds a block without it reports a
  truncated stream and ignores it.
* Several blocks follow each other after binding. Unknown versions or kinds
  are skipped, never fatal.
* Size limits and every length are validated (the reader is fuzzed like
  the GOFF reader).

## 5. Reversal ("smart" `-disasm`)

1. Read the blocks; resolve scopes by name.
2. Walk the statement records in order. For a record with bytes, compare the
   crc of the object's bytes (relocation fields masked, as at assembly time)
   with the recorded one:
   * equal: write the statement from the record (symbolic operands, the
     programmer's label names, DSECT definitions, EQUs, USINGs in their
     original place);
   * different (patched or foreign object): write that range with the
     generic converter and print a warning comment; the rest stays smart.
3. Bytes not covered by any record and objects without ZSYM: generic converter
   as today. `-disasm -generic` forces it.
4. Output is **zosasm syntax**, not an attempt to look like the original text:
   the intent is preserved, not the layout of the source.

Invariants (tested):

* assemble, `-disasm`, assemble gives identical object bytes **and** an
  identical ZSYM (fixed point);
* edit test: enlarge a DSECT field in the reversed source, reassemble:
  all offsets that depended on it follow (the test that motivated this);
* patched byte: only that statement degrades;
* ZSYM present or not does not change one byte of the loaded image.

## 6. Levels

| option | content |
|---|---|
| `-gz0` | nothing (today's output) |
| `-gz1` (proposed default) | symbols, statements, lines (this document) |
| `-gz2` | adds `C_ADATAE9C2`: the original files (before the preprocessor) and the `#define`/`#include` structure, so even macro names, `__LINE__`-style built-ins and include files can be rebuilt; compressed (small LZ, no external library) |

Default on is proposed because the class is NOLOAD and the cost is only file
size; `-gz0` is there for sealed builds.

## 7. Language changes that make the overlay better (free to choose)

Without HLASM compatibility the assembler can offer the programmer a direct
way to say what they now spell with `*-LABEL` tricks. All of these are
assembly time, recorded as `CALL` nodes, written back as such by `-disasm`.

* **`sizeof(x)`**: bytes of a DSECT, section, class piece, or the element
  length of a data symbol (`L'`). No `WORKLEN EQU *-WORK` at the end of the
  DSECT; forward references allowed; keeps working when the layout changes.
* **`offsetof(S,f)`**: offset of field *f* in dummy section *S* (today:
  `f-S`), `alignof(x)`, `countof(x)` (duplication factor of the DS).
* **`ASSERT expr,'message'`**: assembly-time check (a real use of what
  `MNOTE` plus conditional assembly did): `ASSERT sizeof(WORK)<=4096,'WORK
  does not fit one page'`. The checks are recorded too, so `-disasm` keeps
  them.
* **Preprocessor built-ins** (text, resolved before assembly):
  `__FILE__`, `__LINE__` (exist), `__COUNTER__`, `__BASE_FILE__`,
  `__INCLUDE_LEVEL__`, `__ZASM_VERSION__`, `__DATE__`, `__TIME__` (the last two
  make objects differ between builds; they are off with `-reproducible`).
  Position-dependent values (`__LINE__`) turn into plain numbers in the
  reversed source at tier 1; tier 2 can bring the name back.
  A string form is needed (a file name inside `DC C'...'`): a `#`
  stringify operator in function-like macros, as in C, is proposed.
* **Link-time length**: HLASM's `J(DSECT)` and `Q()` give the *binder*'s
  value after merging same-named dummy sections; they stay (Phase 2, with
  `DXD`/`CXD`), the compile-time `sizeof` is for everything else.
* **Relocatable difference** `A(X-Y)` across sections (RLD action SUB): the one
  case where the binder, not the assembler, has to compute the number. It is
  already listed as not implemented in DESIGN.md 10; ZSYM must be able to
  describe it, and it is needed before the smart reversal is complete.

## 8. Phases

| phase | content |
|---|---|
| Z1 | collector in the assembler (statement/symbol records with final offsets, crc), `src/zsym.c` writer, ED/TXT emission in `emit.c`, `-gz` options, `-dump` prints the blocks |
| Z2 | reader (GOFF reader gets the class), smart `-disasm`, original names, DSECT/EQU/USING output, tests of section 5, fuzzing |
| Z3 | `sizeof/offsetof/alignof/countof`, `ASSERT`, extra preprocessor built-ins, `-reproducible` |
| Z4 | tier 2 (original source, pp history, compression) |
| Z5 | DWARF bridge: DSECT as struct types, EQU as constants, line table, for LLDB (`frame variable (WORK*)$r2`); reading ZSYM from a program object through the binder API (needs a z/OS system; pok56 is unreachable now) |

## 9. Open questions

1. Structured operands (this design) or plain statement text per record?
   Text is a quarter of the work and exact, but bound to today's syntax and
   opaque to tools. Recommended: structured, with opaque text for the operands
   that do not depend on symbols.
2. Default level (`-gz1` on?).
3. Record type `X'E9C1'` and the class name (any free type in
   `X'E000'-X'EFFF'` is fine).
4. Which built-ins of section 7 are wanted, and the string form of `__FILE__`.
5. Whether a program can read the class of its own load module at run time
   (NOLOAD vs DEFLOAD, binder API or `LOAD`). Needs a test on z/OS; until then
   "at run time" means: stays in the program object, readable by tools.
