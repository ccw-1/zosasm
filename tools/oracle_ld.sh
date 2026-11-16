#!/bin/sh
# oracle_ld.sh - link test on a z/OS host: zosasm objects and HLASM objects
# (as -mgoff) of testdata/oracle/ld/{liba,chk}.asm are linked with a C main
# (xlc, which calls the binder /bin/ld) in all four combinations and run.
# Each run prints "CHK=<bit mask>"; all four must print the same value (511
# when everything works; bit 8 = check 4 is known to be clear with the
# CATTR class in liba.asm, for the objects of IBM's assembler too).
#
# Environment: ZORACLE_HOST (igor), ZORACLE_DIR (/data/ccw/zoracle),
# ZORACLE_ENV (". /data/ccw/.env"), ZSCP (the z-scp binary).
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$here/..
host=${ZORACLE_HOST:-igor}
dir=${ZORACLE_DIR:-/data/ccw/zoracle}
envc=${ZORACLE_ENV:-". /data/ccw/.env"}
zscp=${ZSCP:-$root/../z-scp/z-scp}
tmp=${TMPDIR:-/tmp}/oracle_ld.$$
mkdir -p "$tmp"
for f in liba chk; do
    "$root/build/zosasm" -o "$tmp/$f.zo" "$root/testdata/oracle/ld/$f.asm"
    "$zscp" "$root/testdata/oracle/ld/$f.asm" "$host:$dir/ld_$f.asm" > /dev/null
    "$zscp" "$tmp/$f.zo" "$host:$dir/ld_${f}z.o" > /dev/null
done
"$zscp" "$root/testdata/oracle/ld/main.c" "$host:$dir/ld_main.c" > /dev/null
ssh -o BatchMode=yes "$host" "$envc; cd $dir; xlc -c -o ld_main.o ld_main.c
for f in liba chk; do as -mgoff -o ld_\$f.o ld_\$f.asm 2>&1 | tail -5; done
for a in ld_liba.o ld_libaz.o; do for c in ld_chk.o ld_chkz.o; do
echo \"== \$a \$c\"; rm -f ld_prog; xlc -o ld_prog ld_main.o \$c \$a 2>&1 | head; ./ld_prog; done; done"
rm -rf "$tmp"
