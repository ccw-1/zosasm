#!/bin/sh
# ibm_compare.sh SRC.asm [HOST] - assemble SRC.asm with IBM HLASM on a z/OS
# host (through ../../tryhlasm.sh), assemble it with zosasm, compare the GOFF
# structure (tools/oracle_obj.py) and the per-statement bytes of the listings
# (tools/oracle_cmp.py).
#
# The object comparison is the gate: a structural difference exits 1. The
# listing comparison is printed for inspection; HLASM truncates long data in
# its object field and numbers literal-pool statements differently, so it can
# differ for reasons that are not object differences.
#
# Environment: TRYHLASM (default ../../tryhlasm.sh), IBM_OUT (default
# /tmp/zosasm-ibm), ZOSASM (default ../build/zosasm).
set -u
src=${1:-}
host=${2:-pok56}
[ -f "$src" ] || { echo "usage: $0 SRC.asm [HOST]" >&2; exit 2; }
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
tryhlasm=${TRYHLASM:-$root/../tryhlasm.sh}
out=${IBM_OUT:-/tmp/zosasm-ibm}
z=${ZOSASM:-$root/build/zosasm}
name=$(basename "$src" .asm)
work=$out/$name
rm -rf "$work"; mkdir -p "$work"
cp "$src" "$work/"
[ -x "$tryhlasm" ] || { echo "$0: $tryhlasm not found" >&2; exit 2; }
[ -x "$z" ] || { echo "$0: $z not found (make)" >&2; exit 2; }
(
    cd "$work" || exit 9
    "$tryhlasm" "$host" "$name.asm" > "$name.try.log" 2>&1
) || { echo "$name: tryhlasm failed (see $work/$name.try.log)" >&2; exit 3; }
cd "$work" || exit 9
"$z" -gz0 -max-errors 0 -ldata -l "$name.zlst" -o "$name.zo" "$name.asm" > "$name.zerr" 2>&1
if [ $? -ne 0 ]; then
    echo "$name: zosasm failed:" >&2
    head -20 "$name.zerr" >&2
    exit 4
fi
"$z" -dump "$name.o" > "$name.hl.dump" 2>&1 || { echo "$name: cannot dump HLASM object" >&2; exit 5; }
"$z" -dump "$name.zo" > "$name.zo.dump" 2>&1 || { echo "$name: cannot dump zosasm object" >&2; exit 5; }
objrc=0
python3 "$root/tools/oracle_obj.py" "$name.hl.dump" "$name.zo.dump" > "$name.objcmp" 2>&1 || objrc=$?
lstcount=0
python3 "$root/tools/oracle_cmp.py" "$name.lst" "$name.zlst" > "$name.lstcmp" 2>&1 || lstcount=$?
cat "$name.objcmp"
if [ "$lstcount" -ne 0 ]; then
    echo "listing differences (inspect $work/$name.lstcmp):"
    head -20 "$name.lstcmp"
else
    echo "listing: identical per-statement bytes"
fi
if [ "$objrc" -ne 0 ]; then
    echo "$name: OBJECT STRUCTURE DIFFERS (see $work/$name.objcmp)" >&2
    exit 1
fi
echo "$name: object structure identical (path $work)"
exit 0
