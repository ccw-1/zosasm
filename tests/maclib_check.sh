#!/bin/bash
# Acceptance run of hlasm2za over a real macro library (local only: the library is not
# part of this repository).   ZASM_MACLIB=/path/to/maclib make test-maclib
# ZASM_CONV_FLAGS=-keep-goto checks the form with #goto instead of the dispatch loop.
# 1. convert every DIR/*.mac (structured and flat), 2. compile each result, 3. expand the
# flat and the structured form with several argument sets: the output must be equal
# (runs that end in a run time error of the macro language are skipped: after an error
# the two forms may take different paths), 4. report the number of #goto left.
LIB=${ZASM_MACLIB:?set ZASM_MACLIB to a directory of HLASM macro members (*.mac)}
B=$(cd "$(dirname "$0")/.." && pwd)/build
T=${ZASM_TMP:-$(mktemp -d)}
mkdir -p "$T/zm" "$T/zf" "$T/w"
$B/hlasm2za $ZASM_CONV_FLAGS -macro-dir "$LIB" -o "$T/zm" 2> "$T/conv.log"; rc1=$?
$B/hlasm2za -flat -macro-dir "$LIB" -o "$T/zf" 2> "$T/conv_flat.log"; rc2=$?
tail -1 "$T/conv.log"
[ $rc1 -eq 0 ] || { echo "hlasm2za -macro-dir failed: $rc1"; tail -20 "$T/conv.log"; exit 1; }
[ $rc2 -eq 0 ] || { echo "hlasm2za -flat -macro-dir failed: $rc2"; tail -20 "$T/conv_flat.log"; exit 1; }
grep -q "members failed" "$T/conv.log" || { echo "hlasm2za summary missing"; exit 1; }
cfail=0; ndiff=0; nrun=0; nskip=0
filt() { grep -v -E '^ *[0-9]* *[|]' | sed -E "s@$T/z[mf]/@@g; s/[A-Za-z0-9_]+\.zmac:[0-9]+(:[0-9]+)?/Z:N/g; s/:[0-9]+:[0-9]+/:N:N/g; s/line [0-9]+/line N/g" | sort; }
for f in "$T"/zm/*.zmac; do
  b=$(basename "$f" .zmac)
  printf 'T        CSECT\n#include "%s.zmac"\n         END\n' "$b" > "$T/w/c.za"
  if ! timeout 10 $B/zosasm -I"$T/zm" -o /dev/null "$T/w/c.za" > /dev/null 2>&1; then
    cfail=$((cfail+1)); echo "does not compile: $b"; continue
  fi
  [ -f "$T/zf/$b.zmac" ] && ! cmp -s "$f" "$T/zf/$b.zmac" || continue
  for args in "" "A" "1,2,3" "(1),X,Y" "A,B,C,D,E,F,G,H" "X=1,Y=2"; do
    for v in zm zf; do
      printf 'T        CSECT\n#include "%s.zmac"\n         %s %s\n         END\n' "$b" "$b" "$args" > "$T/w/t.za"
      timeout 10 $B/zosasm -I"$T/$v" -E -o /dev/null "$T/w/t.za" 2> "$T/w/err" > "$T/w/out"
      cat "$T/w/out" "$T/w/err" | filt > "$T/w/o_$v"
    done
    if grep -q 'error\[ZA41' "$T/w/o_zm" "$T/w/o_zf"; then nskip=$((nskip+1)); continue; fi
    nrun=$((nrun+1))
    cmp -s "$T/w/o_zm" "$T/w/o_zf" || { ndiff=$((ndiff+1)); echo "differs: $b [$args]"; }
  done
done
total=$(ls "$T"/zm/*.zmac 2>/dev/null | wc -l)
[ "$total" -gt 0 ] || { echo "no converted macros found in $T/zm"; exit 1; }
free=0; gotos=0
for f in "$T"/zm/*.zmac; do c=$(grep -c '^ *#goto' "$f"); gotos=$((gotos+c)); [ "$c" = 0 ] && free=$((free+1)); done
echo "converted $total, compile failures $cfail, expansions compared $nrun (skipped $nskip), differences $ndiff"
echo "macros without #goto: $free of $total, #goto left: $gotos"
[ $cfail = 0 ] && [ $ndiff = 0 ]
