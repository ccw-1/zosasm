#!/bin/sh
# oracle_ld.sh - link test on a z/OS host: zosasm objects and HLASM objects
# (as -mgoff) of testdata/oracle/ld/{liba,chk}.asm are linked with a C main
# (xlc, which calls the binder /bin/ld) in all four combinations and run.
# Each run prints "CHK=<bit mask>"; all four must print the same value (511
# when everything works; bit 8 = check 4 is known to be clear with the
# CATTR class in liba.asm, for the objects of IBM's assembler too). xs.asm
# (two CSECTs, relative references between them) must print XB=31 for both.
#
# Environment: ZORACLE_HOST (igor), ZORACLE_DIR (/data/ccw/zoracle),
# ZORACLE_ENV (". /data/ccw/.env"), ZSCP (the z-scp binary).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$here/..
host=${ZORACLE_HOST:-igor}
dir=${ZORACLE_DIR:-/data/ccw/zoracle}
envc=${ZORACLE_ENV:-". /data/ccw/.env"}
zscp=${ZSCP:-$root/../z-scp/z-scp}
tmp=$(mktemp -d "${TMPDIR:-/tmp}/oracle_ld.XXXXXX")
trap 'rm -rf "$tmp"' EXIT INT TERM
for f in liba chk; do
    "$root/build/zosasm" -o "$tmp/$f.zo" "$root/testdata/oracle/ld/$f.asm"
    "$zscp" "$root/testdata/oracle/ld/$f.asm" "$host:$dir/ld_$f.asm" > /dev/null
    "$zscp" "$tmp/$f.zo" "$host:$dir/ld_${f}z.o" > /dev/null
done
"$zscp" "$root/testdata/oracle/ld/main.c" "$host:$dir/ld_main.c" > /dev/null
# xs.asm: two CSECTs of one object, relative references between them
"$root/build/zosasm" -o "$tmp/xs.zo" "$root/testdata/oracle/ld/xs.asm"
"$zscp" "$root/testdata/oracle/ld/xs.asm" "$host:$dir/ld_xs.asm" > /dev/null
"$zscp" "$tmp/xs.zo" "$host:$dir/ld_xsz.o" > /dev/null
"$zscp" "$root/testdata/oracle/ld/xsmain.c" "$host:$dir/ld_xsmain.c" > /dev/null
res=$(ssh -o BatchMode=yes "$host" "$envc; cd $dir; xlc -c -o ld_main.o ld_main.c
for f in liba chk; do as -mgoff -o ld_\$f.o ld_\$f.asm 2>&1 | tail -5; done
for a in ld_liba.o ld_libaz.o; do for c in ld_chk.o ld_chkz.o; do
echo \"== \$a \$c\"; rm -f ld_prog; xlc -o ld_prog ld_main.o \$c \$a 2>&1 | head; ./ld_prog; done; done
xlc -c -o ld_xsmain.o ld_xsmain.c; as -mgoff -o ld_xs.o ld_xs.asm 2>&1 | tail -5
for x in ld_xs.o ld_xsz.o; do echo \"== \$x\"; rm -f ld_prog; xlc -o ld_prog ld_xsmain.o \$x 2>&1 | head; ./ld_prog; done")
printf '%s\n' "$res"
chks=$(printf '%s\n' "$res" | sed -n 's/.*CHK=\([0-9][0-9]*\).*/\1/p' | sort -u)
[ -n "$chks" ] && [ "$(printf '%s\n' "$chks" | wc -l)" -eq 1 ] || {
    echo "oracle_ld.sh: the four CHK values are not identical" >&2
    exit 1
}
[ "$chks" = "511" ] || echo "oracle_ld.sh: CHK=$chks (expected 511; bit 8 can be clear, investigate)" >&2
xbs=$(printf '%s\n' "$res" | sed -n 's/.*XB=\([0-9][0-9]*\).*/\1/p' | sort -u)
[ "$xbs" = "31" ] || { echo "oracle_ld.sh: XB values are '$xbs', expected 31" >&2; exit 1; }
exit 0
