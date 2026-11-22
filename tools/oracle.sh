#!/bin/sh
# oracle.sh - assemble a source with the real HLASM (/bin/as on a z/OS host)
# and with zosasm, and compare the object bytes per statement.
#
#   tools/oracle.sh SRC.asm            (sources: 71 columns at most, HLASM format)
#
# Environment: ZORACLE_HOST (default igor), ZORACLE_DIR (default
# /data/ccw/zoracle), ZORACLE_ENV (default ". /data/ccw/.env"), ZORACLE_OUT
# (default /tmp/lt/or), ZSCP (the z-scp binary).
# Writes OUT/NAME.tsv (digest of the HLASM listing), OUT/NAME.zlst (zosasm
# listing) and OUT/NAME.cmp (the comparison, also printed).
set -eu
src=$1
[ -f "$src" ] || { echo "usage: $0 SRC.asm" >&2; exit 2; }
host=${ZORACLE_HOST:-igor}
dir=${ZORACLE_DIR:-/data/ccw/zoracle}
envc=${ZORACLE_ENV:-". /data/ccw/.env"}
out=${ZORACLE_OUT:-/tmp/lt/or}
zscp=${ZSCP:-$(dirname "$0")/../../z-scp/z-scp}
here=$(cd "$(dirname "$0")" && pwd)
name=$(basename "$src" .asm)
mkdir -p "$out"
# ZORACLE_REUSE=1: keep an existing OUT/NAME.tsv (the HLASM side) and only rerun zosasm
if [ -z "$ZORACLE_REUSE" ] || [ ! -s "$out/$name.tsv" ]; then
"$zscp" "$src" "$host:$dir/$name.asm" > /dev/null
"$zscp" "$here/oracle_cmp.py" "$host:$dir/oracle_cmp.py" > /dev/null
ssh -o BatchMode=yes "$host" "$envc; cd $dir; rm -f $name.lst $name.o; as -mgoff -a=$name.lst -o $name.o $name.asm > $name.as.out 2>&1; python3 oracle_cmp.py --digest cp1047 $name.lst > $name.tsv" || true
ssh -o BatchMode=yes "$host" "$envc; cat $dir/$name.tsv" > "$out/$name.tsv"
fi
"$here/../build/zosasm" -max-errors 0 -ldata -l "$out/$name.zlst" -o "$out/$name.zo" "$src" > "$out/$name.zerr" 2>&1 || true
python3 "$here/oracle_cmp.py" "$out/$name.tsv" "$out/$name.zlst" | tee "$out/$name.cmp"
