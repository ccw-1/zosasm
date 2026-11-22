#!/bin/sh
# oracle_amb.sh NAME - second, independent check with z/OS amblist.
# Needs the files from oracle_obj.sh NAME.asm (ZORACLE_OUT/NAME.hl.o and
# NAME.zo). Both objects are decoded by `amblist LISTOBJ` on the z/OS host
# and the decoded listings are diffed. Expected differences: the B_IDRL class
# and IDRL record of HLASM (shifts the ESDIDs), END record count (HLASM 0).
# The listings stay in ZORACLE_OUT and are never committed.
set -eu
name=$1
[ -n "$name" ] || { echo "usage: $0 NAME" >&2; exit 2; }
here=$(cd "$(dirname "$0")" && pwd)
host=${ZORACLE_HOST:-igor}
dir=${ZORACLE_DIR:-/data/ccw/zoracle}
envc=${ZORACLE_ENV:-". /data/ccw/.env"}
out=${ZORACLE_OUT:-/tmp/lt/or}
zscp=${ZSCP:-$here/../../z-scp/z-scp}
"$zscp" "$out/$name.zo" "$host:$dir/am_${name}z.o" > /dev/null 2>&1
ssh -o BatchMode=yes "$host" "$envc; cd $dir; echo ' LISTOBJ ' | amblist ob_$name.o > am_${name}_hl.txt 2>&1; echo ' LISTOBJ ' | amblist am_${name}z.o > am_${name}_zo.txt 2>&1"
for k in hl zo; do
    "$zscp" "$host:$dir/am_${name}_$k.txt" "$out/am_${name}_$k.txt" > /dev/null 2>&1
    python3 "$here/oracle_amb.py" "$out/am_${name}_$k.txt" > "$out/am_${name}_$k.tok"
done
diff "$out/am_${name}_hl.tok" "$out/am_${name}_zo.tok" || true
