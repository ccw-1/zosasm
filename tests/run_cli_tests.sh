#!/bin/sh
# Command line tests of build/zosasm.
X=build/zosasm
T=build/cli_tmp
fail=0
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

rm -rf $T
[ $fail = 0 ] && echo "tests/run_cli_tests.sh: ok"
exit $fail
