#!/bin/sh
# cblock_oracle.sh SRC.za [HOST] - check a CBLOCK against IBM HLASM.
#
# HLASM cannot read #pragma cblock, so this script expands the block with
# `zosasm -E`, converts the generated GNU-style registers and private labels
# to HLASM source, assembles both sources, and compares the GOFF structure.
# The zosasm source itself is assembled with -gz0.
#
# Environment: TRYHLASM (default ../../tryhlasm.sh), IBM_OUT (default
# /tmp/zosasm-cblock-oracle), ZOSASM (default ../build/zosasm).
set -u
src=${1:-}
host=${2:-pok56}
[ -f "$src" ] || { echo "usage: $0 SRC.za [HOST]" >&2; exit 2; }
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
tryhlasm=${TRYHLASM:-$root/../tryhlasm.sh}
out=${IBM_OUT:-/tmp/zosasm-cblock-oracle}
z=${ZOSASM:-$root/build/zosasm}
name=$(basename "$src" .za)
work=$out/$name
rm -rf "$work"; mkdir -p "$work"
[ -x "$tryhlasm" ] || { echo "$0: $tryhlasm not found" >&2; exit 2; }
[ -x "$z" ] || { echo "$0: $z not found (make)" >&2; exit 2; }
"$z" -E "$src" > "$work/expanded.txt" || { echo "$name: -E failed" >&2; exit 3; }
# HLASM accepts $ in symbols, but use a plain prefix for portability; convert
# %rN to RN and the private labels consistently.
sed -E 's/%[rR]([0-9]+)/\1/g; s/\$CB/CB/g' "$work/expanded.txt" > "$work/$name.asm"
cp "$work/$name.asm" "$work/$name.asm.tmp"
mv "$work/$name.asm.tmp" "$work/$name.asm"
(
    cd "$work" || exit 9
    "$tryhlasm" "$host" "$name.asm" > "$name.try.log" 2>&1
) || { echo "$name: tryhlasm failed (see $work/$name.try.log)" >&2; exit 3; }
"$z" -gz0 -o "$work/$name.zo" "$src" > "$work/$name.zerr" 2>&1 || {
    echo "$name: zosasm failed" >&2
    head -20 "$work/$name.zerr" >&2
    exit 4
}
"$z" -dump "$work/$name.o" > "$work/$name.hl.dump" 2>&1 || { echo "$name: cannot dump HLASM object" >&2; exit 5; }
"$z" -dump "$work/$name.zo" > "$work/$name.zo.dump" 2>&1 || { echo "$name: cannot dump zosasm object" >&2; exit 5; }
python3 "$root/tools/oracle_obj.py" "$work/$name.hl.dump" "$work/$name.zo.dump"
rc=$?
if [ "$rc" -ne 0 ]; then
    echo "$name: CBLOCK object differs from HLASM (path $work)" >&2
    exit 1
fi
echo "$name: CBLOCK expansion is structurally identical to HLASM (path $work)"
exit 0
