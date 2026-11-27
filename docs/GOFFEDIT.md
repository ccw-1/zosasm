# goffedit: edit GOFF through JSON

`goffedit` converts a GOFF file to a semantic JSON document. `goffedit -r`
converts that JSON back to GOFF.

The JSON is not a record dump. It stores names, bytes, symbolic ESD keys and
anchors. The reverse converter computes names lengths, text lengths, element
lengths, record counts and ESDIDs.

The full design is in `GOFFEDIT-DESIGN.md`.

## Usage

```sh
build/goffedit -o old.json old.goff
build/goffedit -r -o old.goff.o old.json
```

Options:

| option | meaning |
|---|---|
| `-o FILE` | output file; default `INPUT.json` or `INPUT.goff` |
| `-r` | read JSON and write GOFF |
| `--compact` | compact JSON output |
| `--check` | validate only |

## JSON shape

A file contains modules:

```json
{
  "format": "goffedit",
  "version": 1,
  "modules": [
    {
      "header": {
        "architecture_level": 1,
        "version": 0,
        "properties": "00008000"
      },
      "esds": [],
      "text": [],
      "relocs": [],
      "lengths": [],
      "idrs": [],
      "end": {}
    }
  ]
}
```

ESD items use symbolic keys such as `SD:SECTION` and `ED:B_TEXT`. References
use those keys, not numeric ESDIDs.

```json
{
  "key": "ED:B_TEXT",
  "type": "ED",
  "parent": "SD:SECTION",
  "name": "B_TEXT",
  "offset": 0,
  "length": "auto",
  "behavior": {
    "amode": 0,
    "rmode": 1,
    "text_style": 0,
    "bind": 0,
    "tasking": 0,
    "read_only": 0,
    "executable": 0,
    "duplicate_severity": 0,
    "bind_strength": 0,
    "loading": 0,
    "common": 0,
    "indirect": 0,
    "scope": 0,
    "linkage": 0,
    "alignment": 3
  }
}
```

`length: "auto"` makes the ED or PR length follow its text.

## Text editing

Text contains hex segments:

```json
{
  "esd": "ED:B_TEXT",
  "style": "byte",
  "encoding": 0,
  "true_length": "auto",
  "offset": 0,
  "segments": [
    { "anchor": "a1" },
    { "hex": "47f00000" },
    { "anchor": "a2" },
    { "hex": "0004" }
  ]
}
```

Edit `hex` to grow or shrink the text. The TXT length, continuation records,
true length and `length: "auto"` element length are recomputed.

An `offset` object can use a text anchor:

```json
{ "offset": { "anchor": "a2", "number": 8 } }
```

Anchors are named positions between segments. The forward converter splits
text at every ESD, relocation and END offset that falls inside it. It writes an
anchor reference when an anchor is present. The `number` field is a fallback
for an offset that cannot be anchored, and it also keeps the first round trip
exact. When an anchor resolves, the reverse converter uses the anchor and
ignores `number`.

A user can add an anchor and point a label, relocation or entry point at it.
Inserting bytes before the anchor moves the reference.

## RLD and LEN

Relocations use symbolic keys:

```json
{
  "rid": "ER:EXT",
  "pid": "ED:B_TEXT",
  "offset": 8,
  "reference": "address",
  "referent": "label",
  "action": "add",
  "store": 0,
  "length": 4,
  "bit_length": 0,
  "bit_offset": 0,
  "conditional": 0,
  "amode_sensitive": 0
}
```

The RLD target length and bit fields are semantic and stay explicit.

Deferred lengths use `esd` keys and explicit lengths. The writer places them
before END.

## IDR

IDR data is structured, not stored as a hardcoded 23-byte blob:

```json
{
  "format": 3,
  "translator": "LLVM  1400",
  "version": "14",
  "release": "00",
  "date": "2025090",
  "time": "032111000"
}
```

The writer creates the `B_IDRL` element and its structured text.

## Validation

The reverse converter rejects:

- duplicate or unresolved ESD keys
- text that refers to an unknown ESD
- malformed hex
- text past an explicit ED or PR length
- invalid anchors
- malformed headers

## Tests

`tests/test_goffedit.c` checks GOFF to JSON to GOFF with a writer-built module.
The command also round-trips converted OBJ/XOBJ GOFF files and edited text
manually.
