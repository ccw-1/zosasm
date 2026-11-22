#!/bin/sh
# oracle_obj.sh SRC.asm - assemble with HLASM (as -mgoff on the z/OS host),
# fetch that object, assemble with zosasm (-gz0), and compare both objects
# structurally with `zosasm -dump` (tools/oracle_obj.py). Environment as in
# oracle.sh. Objects are kept in ZORACLE_OUT (default /tmp/lt/or) as
# NAME.hl.o and NAME.zo.
set -eu
src=$1
[ -f "$src" ] || { echo "usage: $0 SRC.asm" >&2; exit 2; }
here=$(cd "$(dirname "$0")" && pwd)
host=${ZORACLE_HOST:-igor}
dir=${ZORACLE_DIR:-/data/ccw/zoracle}
envc=${ZORACLE_ENV:-". /data/ccw/.env"}
out=${ZORACLE_OUT:-/tmp/lt/or}
zscp=${ZSCP:-$here/../../z-scp/z-scp}
name=$(basename "$src" .asm)
mkdir -p "$out"
"$zscp" "$src" "$host:$dir/ob_$name.asm" > /dev/null 2>&1
ssh -o BatchMode=yes "$host" "$envc; cd $dir; rm -f ob_$name.o; as -mgoff -o ob_$name.o ob_$name.asm > ob_$name.out 2>&1; tail -3 ob_$name.out" || true
"$zscp" "$host:$dir/ob_$name.o" "$out/$name.hl.o" > /dev/null 2>&1 || { echo "HLASM produced no object"; exit 3; }
"$here/../build/zosasm" -gz0 -o "$out/$name.zo" "$src" > "$out/$name.zerr" 2>&1 || { echo "zosasm failed"; head "$out/$name.zerr"; exit 3; }
"$here/../build/zosasm" -dump "$out/$name.hl.o" > "$out/$name.hl.dump" 2>&1
"$here/../build/zosasm" -dump "$out/$name.zo" > "$out/$name.zo.dump" 2>&1
python3 "$here/oracle_obj.py" "$out/$name.hl.dump" "$out/$name.zo.dump"
