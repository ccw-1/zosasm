# goffedit design: GOFF to JSON and back

## Goal

`goffedit` is a semantic editor for a GOFF file. The forward direction writes
JSON. `goffedit -r` reads JSON and writes GOFF again.

The JSON is not a dump of 80-byte records. It describes the logical object:
modules, ESD items, text, relocations, deferred lengths and entry points. The
writer derives field lengths, record counts, continuation records and ESDIDs
from that model.

The design has one main rule:

> A JSON field describes meaning, not a stored byte count.

## Why a record dump is not enough

A direct record dump would encode values such as:

- ESD name length
- TXT data length and true length
- ED and PR element length
- END record count
- LEN record count
- the number of continuation records

Those values become wrong when the user edits text, a name or a symbol. The
reverse converter must recompute them. The JSON therefore stores names and
bytes as data, and it computes the numeric lengths on output.

The same applies to references. A stored ESDID is an implementation detail.
The JSON refers to an ESD item by a stable key. `goffedit -r` assigns ESDIDs
after it knows the final ESD order.

## File model

The top-level JSON object is:

```json
{
  "format": "goffedit",
  "version": 1,
  "modules": []
}
```

`modules` preserves file order. Each module has its own ESD key space and its
own references. A GOFF file from `obj2goff` can contain several modules.

## Module object

```json
{
  "header": {
    "architecture_level": 1,
    "version": 0,
    "properties": "00008000",
    "property_names": ["SYSTEM_LE"]
  },
  "esds": [],
  "text": [],
  "relocs": [],
  "lengths": [],
  "idrs": [],
  "end": {}
}
```

`header.properties` is a hex string. Its byte count determines the HDR
properties length. `property_names` is display information only. The writer
uses the hex bytes, so unknown property bits survive a round trip.

## ESD items

```json
{
  "key": "ed:code",
  "type": "ED",
  "parent": "sd:section",
  "name": "B_TEXT",
  "name_hex": null,
  "namespace": "normal",
  "offset": 0,
  "length": "auto",
  "behavior": {
    "amode": 64,
    "rmode": 64,
    "text_style": "byte",
    "bind_algorithm": "cat",
    "tasking": "rent",
    "read_only": false,
    "executable": "code",
    "duplicate_severity": 0,
    "bind_strength": "strong",
    "loading": "now",
    "common": false,
    "indirect": false,
    "scope": "unspecified",
    "linkage": "os",
    "alignment": 3,
    "reserved": "000000"
  },
  "fill": null,
  "mangled": false,
  "renameable": false,
  "removable": false,
  "reserved_qwords": 0,
  "extended_attribute": null,
  "ada_id": 0,
  "priority": 0
}
```

Rules:

- `key` is unique in the module. References use this key.
- `type` is `SD`, `ED`, `LD`, `PR` or `ER`.
- `parent` is another key or null.
- `name` is printable text. The writer converts it to IBM-1047 EBCDIC.
- `name_hex` is optional raw EBCDIC. When present it is authoritative and
  `name` is display text. This preserves names that are not printable.
- The name length is derived from `name_hex` or from the encoded `name`.
- `namespace` is `normal`, `pseudo`, `parts` or a numeric fallback.
- `offset` is a number or an anchor reference.
- `length` is a number or `"auto"`.
- `fill` is null or a byte number.
- `extended_attribute` is `{ "esd": "key", "offset": n }` or null.

`length: "auto"` is valid for ED and PR. The writer computes it from the text
that refers to that ESD item. If the input has no text for the item, auto
length is zero. If the original length is not the text extent, the forward
writer emits the number instead, so a no-edit round trip preserves it.

SD length is always zero. The writer rejects a nonzero SD length.

## Text

Text is a list of logical text pieces. Physical TXT records and continuation
records are a writer detail.

```json
{
  "esd": "ed:code",
  "style": "byte",
  "encoding": 0,
  "true_length": "auto",
  "offset": 0,
  "segments": [
    { "hex": "47f0" },
    { "anchor": "a1" },
    { "hex": "0004" }
  ]
}
```

Rules:

- `esd` is an ESD key. For text it normally names an ED or PR item.
- `style` is `byte`, `structured` or `unstructured`.
- `encoding` is a number. Only zero is decoded.
- `offset` is optional. When omitted, the writer appends the text after the
  previous text piece for the same ESD. The first piece for an ESD starts at
  zero. This removes hardcoded offsets for contiguous text.
- `segments` are concatenated in order. `{ "hex": ... }` contributes bytes and
  `{ "anchor": ... }` contributes no bytes.
- The TXT data length is the sum of the hex bytes.
- `true_length: "auto"` means the data length when `encoding` is zero. With a
  nonzero encoding the model requires an explicit true length, because the
  encoded bytes do not determine it.
- The writer splits the logical text into as many TXT records as GOFF needs.
  It computes continuation records and record prefixes.

### Anchors

An anchor is a named position between two text segments. Anchors solve the
grow and shrink problem.

The forward converter finds every ESD offset and RLD offset that falls inside
a text piece. It splits the text at those offsets and emits one anchor for
each distinct offset. A label or relocation can then refer to the anchor
instead of carrying a number:

```json
{ "anchor": "a1" }
```

```json
{
  "offset": { "anchor": "a1" }
}
```

If a user grows or shrinks a hex segment before an anchor, the anchor moves
with the text. The label, relocation and entry point that use the anchor move
too. If the user edits text but wants a numeric offset to stay fixed, the JSON
leaves that offset as a number.

The forward converter creates anchors only where a modeled location needs one.
Users can add an anchor between segments and point a new label or relocation
at it.

## Relocations

```json
{
  "rid": "er:EXT",
  "pid": "ed:code",
  "offset": { "anchor": "a1" },
  "reference": "address",
  "referent": "label",
  "action": "add",
  "store": false,
  "length": 4,
  "bit_length": 0,
  "bit_offset": 0,
  "conditional": false,
  "amode_sensitive": false
}
```

Rules:

- `rid` and `pid` are ESD keys or null when the field is absent.
- `offset` is a number or an anchor reference.
- `length` is the target field byte length. It is semantic and is never
  inferred from the text.
- `reference` is `address`, `offset`, `length`, `relimm`, `rtype` or
  `longdisp`.
- `referent` is `label`, `element`, `class` or `part`.
- `action` is `add` or `sub`.
- `bit_length`, `bit_offset`, `conditional` and `amode_sensitive` are stored
  explicitly because the text does not encode them.
- An optional `raw` string preserves unknown RLD flag bits from a foreign
  object. The writer rejects `raw` when the user has changed a semantic
  field that maps into the same bytes.

The reverse converter assigns R and P ESDIDs from the final ESD table. No
stored numeric ESDID appears in the JSON.

## Deferred lengths

```json
[
  { "esd": "ed:code", "length": "auto", "reserved": "00000000" }
]
```

`length: "auto"` uses the computed length of the ED or PR item. This is useful
after editing text. A number preserves an explicit deferred length. The
reserved field is optional and defaults to zeros.

## Translator records

IDR data is modeled as fields, not as a 23-byte blob:

```json
[
  {
    "esd": "ed:idrl",
    "format": 3,
    "translator": "LLVM  1400",
    "version": "14",
    "release": "00",
    "date": "2025090",
    "time": "032111000"
  }
]
```

Rules:

- The writer creates the `B_IDRL` ED and the structured text if the JSON does
  not already contain them.
- Format 1 has a five-character date and no time.
- Format 3 has a seven-character date and a nine-character time.
- Format 2 and unknown formats use `raw_hex` and preserve their bytes.
- The IDR length is computed from the format. There is no hardcoded 23 in the
  model.

## END

```json
{
  "entry": {
    "kind": "by_id",
    "esd": "ed:code",
    "offset": { "anchor": "a1" },
    "name": null,
    "name_hex": null,
    "amode": 0
  },
  "record_count": "auto"
}
```

`kind` is `none`, `by_id` or `by_name`. `record_count` is `"auto"` or a
number. The writer computes the real record count, including HDR, ESD, text,
relocation, length, IDR and END records. A no-edit round trip preserves an
explicit count.

## Derived values

The reverse converter derives these values:

| JSON input | GOFF output |
|---|---|
| `name` or `name_hex` | ESD name length and continuation records |
| `properties` hex | HDR properties length |
| all `segments` hex | TXT data length and continuation records |
| `true_length: auto` with encoding 0 | TXT true length |
| `length: auto` on ED/PR | ESD element length |
| `lengths[].length: auto` | LEN payload |
| `record_count: auto` | END record count |
| `segments` and anchors | TXT offsets and anchored ESD/RLD/END offsets |
| ESD array order | final ESDIDs, after validation |
| ESD keys | parent, RLD R/P, LEN, IDR and END references |

The converter never uses a stored name length, TXT length, ED length, LEN
length or END count when the JSON supplies `"auto"` or omits the value.

## Editing rules

The editing contract is:

1. A hex edit inside a segment changes bytes at that position. Segments after
   it move by the size difference.
2. An anchor points between segments. It moves with every byte change before
   it.
3. A numeric offset does not move when text changes.
4. An auto length grows and shrinks with the referenced text.
5. A numeric length stays fixed. The writer reports an error when text extends
   past an explicit ED or PR length.
6. A symbolic ESD reference remains valid when names or ESDIDs change. The key
   is the identity.
7. A name edit changes the derived ESD name length and continuation records.
8. A user who wants a fixed binary image after editing supplies numeric
   lengths and numeric offsets instead of auto values and anchors.

This makes an ordinary edit safe: users edit bytes and names, and the writer
recomputes layout. Users who need exact placement retain explicit numbers.

## Ordering and identity

The forward writer assigns ESDIDs in final JSON ESD order. It writes all ESD
items before text and relocations. The reader does not require a numeric ID in
the JSON.

The reader generates stable keys such as:

- `sd:NAME`
- `ed:NAME@PARENT_KEY`
- `ld:NAME`
- `pr:NAME`
- `er:NAME`

A duplicate key gets a numeric suffix. Users may rename keys, but every
reference must then be updated. The key is an opaque string; the type and name
are display fields.

The writer validates:

- every key is unique
- every parent and RLD reference resolves
- a child type is valid for its parent
- an explicit length is not smaller than its text extent
- every anchor exists in a text piece for the referenced ESD
- an anchor is used only for an offset inside a text piece
- every IDR format has enough fields
- a properties or name hex string has an even number of digits

## Reading all modules

The current GOFF reader stops after the first END. `goffedit` needs a reader
that returns every module in the file. Add `goff_parse_all` (or an equivalent
file object) and keep the existing first-module helper for compatibility.

The HDR writer also needs a properties setter. `goff_writer_new` writes only
the architecture level today. Add a header object or a setter so the writer
can emit the bytes from `header.properties`.

## CLI

```text
goffedit [-o FILE] [--compact] [--check] FILE.goff
goffedit -r [-o FILE] [--compact] [--check] FILE.json
```

- The default mode reads GOFF and writes JSON.
- `-r` reads JSON and writes GOFF.
- `-o` selects the output file.
- `--compact` writes JSON without indentation.
- `--check` validates and reports differences that a reverse conversion would
  introduce, without writing the output.
- Standard input and standard output use `-`.

## Implementation plan

1. Add a GOFF file reader for all modules and a writer header-properties API.
2. Add a small JSON reader and writer. The MCP server already has a parser;
   factor a reusable parser into `src/json.c` instead of duplicating it.
3. Add the API model in `src/goffedit.c`: module, ESD, text, relocation, LEN,
   IDR and END structures.
4. Implement forward conversion. Generate keys, auto values and anchors.
5. Implement reverse conversion. Resolve keys, assign ESDIDs, compute text
   offsets, lengths and record counts, then call the existing GOFF writer.
6. Add validation and edit diagnostics.
7. Add round-trip and edit tests.
8. Add the CLI, documentation and examples.

## Tests

The test suite should cover:

- No-edit GOFF to JSON to GOFF semantic equality.
- A file with several modules.
- Growth and shrinkage of a text segment before an anchored label and
  relocation.
- Auto ED and PR lengths after text edits.
- Auto LEN lengths and auto END record counts.
- Name edits with derived name lengths and continuation records.
- ESD key reordering and reference remapping.
- Structured IDR format 1 and format 3 generation.
- Unknown properties, names and RLD raw bits.
- Errors for broken keys, bad anchors, negative lengths and text past an
  explicit ED length.

The comparison should use the semantic object, not byte identity. Physical
record order and ESDID numbering are not promises of the editor.

## Non-goals

`goffedit` does not assemble or disassemble text. It does not turn GOFF into
source. It does not invent DWARF or source lines. `obj2goff` is the separate
OBJ/XOBJ converter; `goffedit` edits an existing GOFF object.
