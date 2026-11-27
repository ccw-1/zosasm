# obj2goff: convert OBJ and XOBJ to GOFF

`obj2goff` reads the older z/OS object module format (OBJ) and the extended
object module format (XOBJ). It writes GOFF, so a GOFF-only consumer can read
an old object without reassembly.

The binder still accepts OBJ records. Use `obj2goff` when a tool needs GOFF,
or when you want a best-effort conversion of old symbol information.

## Usage

```sh
build/obj2goff -o old.goff.o old.obj
build/obj2goff -dwarf -o old.goff.o old.obj
```

Options:

| option | meaning |
|---|---|
| `-o FILE` | output GOFF path; default `INPUT.goff.o` |
| `-no-sym` | do not preserve SYM records in `C_OBJSYM` |
| `-dwarf` | write `FILE.dbg` and append its GOFF link module |

`-dwarf` creates a side file named `OUTPUT.dbg` and embeds the matching link
module in the GOFF object. The debug information is best effort: it uses ESD
labels and the names and addresses that can be read from SYM records.

## Record mapping

The converter follows Appendix C of *z/OS MVS Program Management: Advanced
Facilities*.

| OBJ/XOBJ | GOFF |
|---|---|
| SD or PC | SD, `B_TEXT` ED, and an LD for the external name |
| CM | common SD, `B_TEXT` ED, and an LD |
| LD | LD under the current element |
| ER/WX | ER, with weak binding for WX |
| PR/XD | PR with part name space |
| TXT | TXT in the element remembered for the section |
| RLD | RLD with 4-byte ESDIDs and mapped reference types |
| END entry by ESDID | END entry by ESDID and offset |
| END entry by name | END entry by name |
| END section length | LEN record |
| END IDR | structured `B_IDRL` text |

ESDIDs are remapped because the GOFF writer assigns new IDs in emission
order. RLD R and P pointers use the new IDs. An XOBJ file can contain several
subfiles; `obj2goff` writes one GOFF module for each subfile.

## SYM and DWARF

OBJ SYM records have no direct GOFF equivalent. By default `obj2goff`
preserves the raw SYM payload in the unloaded class `C_OBJSYM`. `-no-sym`
turns this off.

`-dwarf` also builds a DWARF `.dbg` file. It reads the ESD labels and the
symbol names and addresses it can recover from SYM records. It emits DWARF
label entries and the GOFF link module that names the `.dbg` file. This is a
best-effort path:

- There is no source line table because OBJ does not carry source rows.
- XSD names longer than one record are truncated to the bytes available.
- Some SYM data entries are skipped because their variable-length layout is
  not needed for label names.
- XPLINK, DLL linkage descriptors and binder-generated classes cannot be
  manufactured from an old OBJ module.
- XOBJ debug information is taken from the largest subfile.

The output is intended to recover useful symbol names for debugging, not to
reproduce the original compiler debug information.

## Limits

The converter reads fixed 80-byte records with the `X'02'` prefix. It does not
read VB or U OBJ data sets, and it does not read load modules. It preserves
the common OBJ attributes and emits defaults for attributes that OBJ cannot
express.
