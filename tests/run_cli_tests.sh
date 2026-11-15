#!/bin/sh
# Command line tests of build/zosasm.
X=build/zosasm
T=build/cli_tmp
fail=0
P=$(pwd)
mkdir -p $T
check() { if [ "$2" != "$3" ]; then echo "FAIL cli: $1 (got '$2', want '$3')"; fail=1; fi; }

$X -o $T/hello.o testdata/hello.za 2>$T/err; check "hello exit" $? 0
[ -s $T/hello.o ] || { echo "FAIL cli: no object"; fail=1; }
$X -dump $T/hello.o | grep -q "ESD" ; check "dump" $? 0

printf 'T CSECT\n LA 1,UNDEF\n' > $T/bad.za
$X -o $T/bad.o $T/bad.za 2>$T/err; check "error exit" $? 1
grep -q ZA1011 $T/err; check "error code printed" $? 0
[ -e $T/bad.o ] && { echo "FAIL cli: object written after errors"; fail=1; }

printf 'T CSECT\n DROP 5\n BR 14\n' > $T/warn.za
$X -o $T/warn.o $T/warn.za 2>/dev/null; check "warning only" $? 0
$X -Werror -o $T/warn.o $T/warn.za 2>/dev/null; check "-Werror" $? 1

cp $T/warn.za $T/dflt.za; rm -f $T/dflt.o
$X $T/dflt.za 2>/dev/null; [ -s $T/dflt.o ]; check "default output name" $? 0

$X 2>/dev/null; check "no input" $? 2
$X -bogus x.za 2>/dev/null; check "bad option" $? 2
$X $T/missing.za 2>/dev/null; check "missing file" $? 1

# preprocessor
mkdir -p $T/inc
printf ' DC F%s\n' "'7'" > $T/inc/k.inc
printf '#include <k.inc>\n#ifdef BIG\n DC F%s\n#endif\n' "'9'" > $T/pp.za
$X -E -I$T/inc -DBIG $T/pp.za 2>/dev/null > $T/pp.out; check "-E exit" $? 0
[ "$(cat $T/pp.out)" = " DC F'7'
 DC F'9'" ]; check "-E output" $? 0
printf 'T CSECT\n#include <k.inc>\n' > $T/pp2.za
$X -I$T/inc -o $T/pp2.o $T/pp2.za 2>/dev/null; check "include assembles" $? 0
$X -o $T/pp3.o $T/pp2.za 2>$T/err; check "missing include" $? 1
grep -q ZA4011 $T/err; check "missing include code" $? 0
printf '#error stop here\n' > $T/pp4.za
$X -o $T/pp4.o $T/pp4.za 2>$T/err; check "#error exit" $? 1
grep -q ZA4050 $T/err; check "#error code" $? 0
printf 'T CSECT\n COPY X\n' > $T/pp5.za
$X -o $T/pp5.o $T/pp5.za 2>$T/err; check "COPY rejected" $? 1
grep -q ZA9001 $T/err; check "COPY code" $? 0

# built-ins, -reproducible, -version
printf '#define S(x) C ## #(x)\nT CSECT\n DC S(__DATE__)\n DC S(__BASE_FILE__)\n' > $T/b2.za
SOURCE_DATE_EPOCH=1700000000 $X -E $T/b2.za 2>/dev/null | grep -q "DC C'2023-11-14'"; check "__DATE__ with SOURCE_DATE_EPOCH" $? 0
env -u SOURCE_DATE_EPOCH $X -E -reproducible $T/b2.za 2>$T/err >/dev/null; check "-reproducible without a time" $? 1
grep -q ZA4080 $T/err; check "-reproducible code" $? 0
SOURCE_DATE_EPOCH=1700000000 $X -reproducible -o $T/b2a.o $T/b2.za 2>/dev/null; check "-reproducible with a time" $? 0
SOURCE_DATE_EPOCH=1700000000 $X -reproducible -o $T/b2b.o $T/b2.za 2>/dev/null
cmp -s $T/b2a.o $T/b2b.o; check "-reproducible gives equal objects" $? 0
$X -version | grep -q "^zosasm [0-9]"; check "-version" $? 0
printf 'S DSECT\nA DS F\nM CSECT\n ASSERT sizeof(S)==8,"S must be 8"\n' | sed "s/\"/'/g" > $T/as.za
$X -o $T/as.o $T/as.za 2>$T/err; check "ASSERT fails the assembly" $? 1
grep -q "assertion failed: S must be 8" $T/err; check "ASSERT message" $? 0

# ZSYM: on by default, -gz0 leaves it out
$X -dump $T/hello.o | grep -q "^ZSYM: 1 block"; check "ZSYM by default" $? 0
$X -dump $T/hello.o | grep -q "complete"; check "ZSYM block complete" $? 0
$X -gz0 -o $T/hello0.o testdata/hello.za 2>/dev/null; check "-gz0 exit" $? 0
$X -dump $T/hello0.o | grep -q "ZSYM"; check "-gz0 has no ZSYM" $? 1
$X -dump $T/hello0.o | grep -q "C_ADATA"; check "-gz0 has no ADATA class" $? 1
[ $(wc -c < $T/hello.o) -gt $(wc -c < $T/hello0.o) ]; check "-gz1 object is larger" $? 0

# disassembler: object -> source -> the same object
$X -disasm $T/hello0.o -o $T/back.za 2>$T/err; check "-disasm exit" $? 0
[ ! -s $T/err ]; check "-disasm without warnings" $? 0
$X -gz0 -o $T/back.o $T/back.za 2>/dev/null; check "-disasm output assembles" $? 0
cmp -s $T/hello0.o $T/back.o; check "-disasm round trip" $? 0
$X -disasm $T/hello0.o 2>/dev/null | cmp -s - $T/back.za; check "-disasm to stdout" $? 0
$X -disasm $T/hello.o -o $T/backz.za 2>$T/err; check "-disasm of an object with ZSYM" $? 0
[ ! -s $T/err ]; check "-disasm with ZSYM without warnings" $? 0
$X -generic -disasm $T/hello.o -o $T/backg.za 2>$T/err; check "-generic exit" $? 0
[ ! -s $T/err ]; check "-generic without warnings" $? 0
cmp -s $T/back.za $T/backg.za; check "-generic ignores ZSYM" $? 0
$X -disasm $T/hello.o 2>/dev/null | $X -gz0 -o $T/backz.o /dev/stdin 2>/dev/null; check "ZSYM source assembles" $? 0
cmp -s $T/hello0.o $T/backz.o; check "ZSYM source gives the object back" $? 0
head -c 200 $T/hello.o > $T/trunc.o
$X -disasm $T/trunc.o 2>/dev/null; check "-disasm bad object" $? 1
$X -disasm $T/missing.o 2>/dev/null; check "-disasm missing file" $? 1

# tier 2: the original sources in the object
mkdir -p $T/src/inc
printf '#include "inc/regs.inc"\nT1 CSECT\n LR R1,R2\n DC A(N)\n END\n' > $T/src/main.za
printf '#define N 3\nR1 EQU 1\nR2 EQU 2\n' > $T/src/inc/regs.inc
(cd $T/src && $P/$X -gz2 -DFOO=2 -o m2.o main.za 2>/dev/null); check "-gz2 exit" $? 0
$X -dump $T/src/m2.o | grep -q "^ZSRC: 1 block"; check "-gz2 has ZSRC" $? 0
$X -dump $T/src/m2.o | grep -q "define FOO=2"; check "-gz2 keeps the -D options" $? 0
$X -dump $T/src/m2.o | grep -q "file 1: inc/regs.inc"; check "-gz2 keeps the included file" $? 0
(cd $T/src && $P/$X -o m1.o main.za 2>/dev/null); $X -dump $T/src/m1.o | grep -q "ZSRC"; check "default has no ZSRC" $? 1
$X -unpack $T/src/m2.o -o $T/unp >$T/list; check "-unpack exit" $? 0
cmp -s $T/src/main.za $T/unp/main.za; check "-unpack main file" $? 0
cmp -s $T/src/inc/regs.inc $T/unp/inc/regs.inc; check "-unpack included file" $? 0
(cd $T/unp && $P/$X -gz2 -DFOO=2 -o m2.o main.za 2>/dev/null); check "unpacked sources assemble" $? 0
cmp -s $T/src/m2.o $T/unp/m2.o; check "unpacked sources give the object back" $? 0
$X -unpack $T/src/m1.o -o $T/unp1 2>/dev/null; check "-unpack without sources" $? 1
$X -disasm $T/src/m2.o 2>$T/err | grep -q "DC    A(3)"; check "-disasm of a -gz2 object" $? 0
[ ! -s $T/err ]; check "-disasm of a -gz2 object without warnings" $? 0

# the User's Guide: every example assembles (without warnings) and is printed in docs/GUIDE.md as it is
for f in docs/examples/*; do
  b=$(basename $f)
  case $b in
    *.za) $X -Werror -o $T/ex.o $f 2>$T/err; check "example $b assembles" $? 0 ;;
  esac
  awk -v m="<!-- example: $b -->" '$0==m{f=1;next} f&&/^```/{if(inb){exit}else{inb=1;next}} inb{print}' docs/GUIDE.md > $T/ex.txt
  cmp -s $T/ex.txt $f; check "docs/GUIDE.md shows $b" $? 0
done

# the build variants of docs/examples/config.za (guide 15.3)
for l in 0 1 2; do $X -gz0 -DLEVEL=$l -o $T/c$l.o docs/examples/config.za; check "config.za LEVEL=$l assembles" $? 0; done
$X -DLEVEL=3 -o $T/c3.o docs/examples/config.za 2>/dev/null; check "config.za LEVEL=3 is rejected" $? 1
n0=$($X -dump $T/c0.o | grep TXT | head -1 | sed 's/.*len=\([0-9]*\).*/\1/'); n2=$($X -dump $T/c2.o | grep TXT | head -1 | sed 's/.*len=\([0-9]*\).*/\1/')
[ "$n0" = 20 ] && [ "$n2" = 48 ]; check "config.za code sizes 20 and 48 (guide 15.3)" $? 0

# guide 15.4: unpack and reassemble from inside the directory gives the same object
mkdir $T/rt && cp docs/examples/config.za $T/rt/ && (cd $T/rt && $P/$X -gz2 -Werror -DLEVEL=2 -o config.o config.za && $P/$X -unpack config.o -o src >/dev/null && cd src && $P/$X -gz2 -Werror -DLEVEL=2 -o ../again.o config.za && cmp ../config.o ../again.o)
check "guide 15.4 round trip gives the same object" $? 0

rm -rf $T
[ $fail = 0 ] && echo "tests/run_cli_tests.sh: ok"
exit $fail
