#!/bin/sh
# Command line tests of build/zosasm.
X=build/zosasm
T=build/cli_tmp
fail=0
P=$(pwd)
rm -rf "$T"
mkdir -p "$T"
check() { if [ "$2" != "$3" ]; then echo "FAIL cli: $1 (got '$2', want '$3')"; fail=1; fi; }

$X -o $T/hello.o testdata/hello.za 2>$T/err; check "hello exit" $? 0
[ -s $T/hello.o ] || { echo "FAIL cli: no object"; fail=1; }
$X -dump $T/hello.o | grep -q "ESD" ; check "dump" $? 0
$X -dump-readable $T/hello.o > $T/readable.txt; check "-dump-readable" $? 0
grep -q "GOFF module" $T/readable.txt; check "readable dump heading" $? 0
$X -dump-readable $T/hello.o -insn | grep -q "STM" ; check "-dump-readable -insn" $? 0
$X -dump -insn $T/hello.o | grep -q "STM" ; check "-dump -insn (before file)" $? 0
$X -dump $T/hello.o -insn | grep -q "STM" ; check "-dump -insn (after file)" $? 0

printf 'T CSECT\n LA 1,UNDEF\n' > $T/bad.za
$X -o $T/bad.o $T/bad.za 2>$T/err; check "error exit" $? 1
grep -q ZA1011 $T/err; check "error code printed" $? 0
[ -e $T/bad.o ] && { echo "FAIL cli: object written after errors"; fail=1; }

printf 'T CSECT\n DROP 5\n BR 14\n' > $T/warn.za
$X -o $T/warn.o $T/warn.za 2>/dev/null; check "warning only" $? 0
$X -Werror -o $T/warn.o $T/warn.za 2>/dev/null; check "-Werror" $? 1

cp $T/warn.za $T/dflt.za; rm -f $T/dflt.o
$X $T/dflt.za 2>/dev/null; [ -s $T/dflt.o ]; check "default output name" $? 0

$X -diag json -o $T/bad.o $T/bad.za 2>$T/err; check "-diag json exit" $? 1
grep -q '^{"diagnostics":\[' $T/err; check "-diag json header" $? 0
grep -q '"code":"ZA1011","message":"undefined symbol' $T/err; check "-diag json code" $? 0
grep -q '"errors":1,"warnings":0}' $T/err; check "-diag json counts" $? 0
$X -diag json -o $T/hello.o testdata/hello.za 2>$T/err; check "-diag json ok exit" $? 0
grep -q '"diagnostics":\[\],"errors":0' $T/err; check "-diag json empty" $? 0
# a message with a byte >= 0x80 must produce valid JSON; UTF-8 stays UTF-8, invalid bytes are escaped
printf 'T CSECT\n BADOP\303\251 1\n' > $T/hi.za
$X -diag json -o /dev/null $T/hi.za 2>$T/err; check "-diag json: high byte exit" $? 1
python3 -c 'import json,sys; json.load(open(sys.argv[1]))' $T/err; check "-diag json: high byte is valid json" $? 0
grep -Eq '\\u00e9|é' $T/err; check "-diag json: high byte preserved as UTF-8" $? 0
# the source can come from standard input
printf 'T CSECT\n LR 1,2\n END\n' | $X -E - > $T/stdin.out; check "standard input: exit" $? 0
grep -q 'LR *1,2' $T/stdin.out; check "standard input: preprocessed" $? 0
$X -diag xml testdata/hello.za 2>/dev/null; check "-diag bad value" $? 2
$X -l - -lbrief -o $T/bad.o $T/bad.za 2>/dev/null | grep -q '^ *\^\^\^\^\^$'; check "listing carets" $? 0

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
# __AMODE__ and __RMODE__ reach the object: 0,0 before the statements, 31,24 after
printf 'T CSECT\n DC A(__AMODE__,__RMODE__)\nT AMODE 31\nT RMODE 24\n DC A(__AMODE__,__RMODE__)\n END\n' > $T/am.za
$X -o $T/am.o $T/am.za 2>/dev/null; check "__AMODE__ assembles" $? 0
$X -dump $T/am.o | grep -q 'len=16 00 00 00 00 00 00 00 00 00 00 00 1F 00 00 00 18'; check "__AMODE__ and __RMODE__ values in the object" $? 0
# docs/examples/showargs.za picks its code with __AMODE__ (testdata/showargs.za is a link to it)
for m in 0 1; do
    $X -DTEST31=$m -o $T/sa$m.o docs/examples/showargs.za 2>/dev/null; check "showargs TEST31=$m assembles" $? 0
done
$X -DTEST31=0 -E docs/examples/showargs.za 2>/dev/null | grep -q "DC *C'AMODE 64'"; check "showargs AMODE 64 banner" $? 0
$X -DTEST31=1 -E docs/examples/showargs.za 2>/dev/null | grep -q "DC *C'AMODE 31'"; check "showargs AMODE 31 banner" $? 0
# the kernel service is called in the caller's AMODE, with a fullword (31) or doubleword (64) list
$X -DTEST31=1 -E docs/examples/showargs.za 2>/dev/null | grep -q 'SAM'; [ $? = 1 ]; check "showargs AMODE 31 does not switch modes" $? 0
$X -DTEST31=1 -E docs/examples/showargs.za 2>/dev/null | grep -q 'ST  *0,PARMLIST+24\|ST  *%r0,PARMLIST+24'; check "showargs AMODE 31 has a fullword parameter list" $? 0
$X -DTEST31=0 -E docs/examples/showargs.za 2>/dev/null | grep -q 'STG  *%r0,PARMLIST+48'; check "showargs AMODE 64 has a doubleword parameter list" $? 0
printf 'T CSECT\n AMODE 64\n DC A(__AMODE__)\n END\n' > $T/am2.za
$X -E $T/am2.za 2>/dev/null | grep -q 'DC *A(64)'; check "__AMODE__ 64 in -E output" $? 0
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
# DWCFI emits no code: without -g the GOFF object is the same with and without it, and with -g the code,
# the symbols of the program and the relocations match (the debug class lengths differ, the line rows too)
cat > $T/cfi_w.za <<'EOF'
MAIN     CSECT
         STMG  %r14,%r15,112(%r15)
         DWCFI SAVE,14,112
         LAY   %r15,-160(%r15)
         DWCFI CFA,15,160
         BR    %r14
         END
EOF
grep -v DWCFI $T/cfi_w.za > $T/cfi_wo.za
$X -gz0 -o $T/cfi_w0.o $T/cfi_w.za 2>/dev/null; $X -gz0 -o $T/cfi_wo0.o $T/cfi_wo.za 2>/dev/null
cmp -s $T/cfi_w0.o $T/cfi_wo0.o; check "DWCFI leaves the GOFF object unchanged (-gz0)" $? 0
mkdir -p $T/cfi_a $T/cfi_b && cp $T/cfi_w.za $T/cfi_a/x.za && cp $T/cfi_wo.za $T/cfi_b/x.za
(cd $T/cfi_a && $P/$X -gz0 -g -o x.o x.za 2>/dev/null); (cd $T/cfi_b && $P/$X -gz0 -g -o x.o x.za 2>/dev/null)
$X -dump $T/cfi_a/x.o 2>/dev/null | grep -E '^(TXT esd=2 |RLD)' > $T/cfi_a.txt
$X -dump $T/cfi_b/x.o 2>/dev/null | grep -E '^(TXT esd=2 |RLD)' > $T/cfi_b.txt
cmp -s $T/cfi_a.txt $T/cfi_b.txt; check "DWCFI leaves the code and relocations unchanged (-g)" $? 0
test -s $T/cfi_a.txt; check "the code record was dumped" $? 0
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

# macros (guide 19): the example expands as printed, library lookup with -M, step limit, diagnostics
$X -E docs/examples/cmacros.za > $T/mac.txt 2>&1; check "cmacros.za -E" $? 0
grep -q 'STG   2,20(,13)' $T/mac.txt && grep -q 'XC    BUF+512(88),BUF+512' $T/mac.txt && grep -q 'DC    A(2)' $T/mac.txt; check "cmacros.za expands" $? 0
mkdir -p $T/mlib && printf '#macro MYLIB(r) {\n    LR    &r,&r\n#}\n' > $T/mlib/mylib.zmac && printf '#macro OLDM() {\n    NOPR  0\n#}\n' > $T/mlib/oldm.mac
printf 'T CSECT\n    MYLIB 1\n    OLDM\n' > $T/usemac.za
$X -E -M $T/mlib $T/usemac.za 2>&1 | grep -q 'LR    1,1'; check "-M finds name.zmac" $? 0
$X -E -M $T/mlib $T/usemac.za 2>&1 | grep -q 'NOPR  0'; check "-M finds name.mac" $? 0
$X -E -I $T/mlib $T/usemac.za 2>&1 | grep -q 'LR    1,1'; check "-I is searched for macros too" $? 0
printf '#macro LOOPM() {\n    #while (1) {\n    #}\n#}\n    LOOPM\n' > $T/loop.za
$X -E -macro-steps 500 $T/loop.za 2>$T/err >/dev/null; check "-macro-steps stops a loop" $? 1
grep -q ZA4121 $T/err; check "the error is ZA4121" $? 0
printf '#macro BADM() {\n    #error "no good"\n#}\n    BADM\n' > $T/bad2.za
$X -E $T/bad2.za 2>$T/err >/dev/null; check "#error in a macro fails" $? 1
grep -q 'ZA4050' $T/err && grep -q 'in expansion of macro' $T/err; check "#error names the macro" $? 0
$X -l - -o $T/mac.o docs/examples/cmacros.za 2>&1 | grep -q ' [0-9][0-9]* +  *STG'; check "listing marks generated lines" $? 0

# strict registers (guide 6.1)
$X -strict-registers -Werror -o $T/strict.o docs/examples/strict.za 2>/dev/null; check "strict.za assembles with -strict-registers" $? 0
$X -Werror -o $T/strict2.o docs/examples/strict.za 2>/dev/null; check "strict.za assembles without it" $? 0
cmp -s $T/strict.o $T/strict2.o; check "strict and relaxed objects are the same" $? 0
$X -strict-registers -o $T/h3.o docs/examples/hello.za 2>$T/err; check "numbers are rejected with -strict-registers" $? 1
grep -q 'ZA2067' $T/err; check "the error is ZA2067" $? 0
grep -q 'write %r14' $T/err; check "the error says what to write" $? 0
$X -disasm $T/strict.o >$T/strict.disasm 2>$T/err; check "-disasm of a strict object" $? 0
[ ! -s $T/err ]; check "-disasm of a strict object: no warnings" $? 0
$X -disasm $T/hello.o >$T/plain.za 2>$T/err; check "-disasm without the option" $? 0
grep -q 'STM   14,12,12(13)' $T/plain.za; check "-disasm without the option keeps the numbers" $? 0
$X -strict-registers -disasm $T/hello.o >$T/strict.za 2>$T/err; check "-disasm -strict-registers" $? 0
[ ! -s $T/err ]; check "-disasm -strict-registers: no warnings" $? 0
grep -q 'STM   %r14,%r12,12(%r13)' $T/strict.za; check "-disasm -strict-registers writes %r" $? 0
grep -q 'USING HELLO,%r12' $T/strict.za; check "-disasm -strict-registers: USING" $? 0
grep -q 'LA    %r2,MSG' $T/strict.za; check "-disasm -strict-registers: base register" $? 0
$X -gz0 -o $T/hello0s.o testdata/hello.za 2>/dev/null
$X -strict-registers -gz0 -Werror -o $T/strict3.o $T/strict.za 2>/dev/null; check "the strict source assembles with -strict-registers" $? 0
cmp -s $T/strict3.o $T/hello0s.o; check "and gives the same object" $? 0
$X -strict-registers -disasm $T/strict.o 2>&1 | grep -q 'BASE'; check "-disasm -strict-registers keeps EQU %r names" $? 0

# the listing (-l)
$X -l $T/hello.lst -o $T/hl.o docs/examples/hello.za 2>/dev/null; check "-l writes a listing" $? 0
grep -q 'Symbol cross reference' $T/hello.lst; check "listing has the symbol table" $? 0
grep -q '^000020  00000001 ' $T/hello.lst; check "listing shows the constant" $? 0
printf 'T CSECT\n STM 14,12,12(13)\n LR 12,15\n L 2,0(,1)\n BR 14\n DC F%s\n' "'1'" > $T/nopad.za
$X -l $T/nopad.lst -o $T/hl.o $T/nopad.za 2>/dev/null
lb=$(grep -E '^[0-9A-F]{6}  [0-9A-F]' $T/nopad.lst | cut -c9-26 | tr -d ' \n')
ob=$($X -dump $T/hl.o | grep '^TXT esd=2' | sed 's/^TXT esd=2 off=0 len=[0-9]* //' | tr -d ' \n')
[ -n "$lb" ] && [ "$lb" = "$ob" ]; check "listing bytes are the object bytes" $? 0
$X -l - -lbrief -o $T/hl2.o docs/examples/hello.za 2>/dev/null | grep -q 'Symbol cross reference'; [ $? = 1 ]; check "-lbrief has no tables" $? 0
$X -l $T/hello2.lst -o $T/hl2.o docs/examples/hello.za 2>/dev/null; cmp -s $T/hello.lst $T/hello2.lst; check "the listing is reproducible" $? 0
$X -l $T/bad.lst -o $T/bad3.o $T/bad.za 2>/dev/null; check "-l with errors: exit 1" $? 1
grep -q '^\*\* ZA1011 error' $T/bad.lst; check "-l with errors: the diagnostic is in the listing" $? 0
[ -e $T/bad3.o ] && { echo "FAIL cli: object written after errors with -l"; fail=1; }
$X -l $T/prep.lst -Idocs/examples -o $T/pl.o docs/examples/prep.za 2>/dev/null; check "-l with includes" $? 0
grep -q '^1 *docs/examples/macros.inc' $T/prep.lst; check "the include file has an id" $? 0
grep -q ' + *XR    5,5' $T/prep.lst; check "expanded lines are marked" $? 0
$X -l $T/pg.lst -lpage 10 -lbrief -o $T/pl.o docs/examples/prep.za -Idocs/examples 2>/dev/null
[ "$(tr -cd '\f' < $T/pg.lst | wc -c)" -ge 2 ]; check "-lpage makes pages" $? 0
printf 'K EQU 5\nZ DS F\n' > $T/inc/sym.inc
printf 'T CSECT\n#include "sym.inc"\n LA 1,K\n LA 2,K\n END\n' > $T/symi.za
$X -l - -I$T/inc -o $T/si.o $T/symi.za 2>/dev/null | grep -q '^K  *equ  *1  *5  *1:1  *3 4$'; check "cross reference of a symbol of an include file" $? 0
# the listings printed in the guide (11.2); the first line has the version
$X -l - -lbrief -o $T/g1.o docs/examples/hello.za 2>/dev/null | tail -n +2 > $T/g1.txt
awk '/^<!-- listing: hello -->$/{f=1;next} f&&/^```/{if(inb){exit}else{inb=1;next}} inb{print}' docs/GUIDE.md | tail -n +2 > $T/g1.guide
cmp -s $T/g1.txt $T/g1.guide; check "docs/GUIDE.md shows the hello.za listing" $? 0
$X -l - -o $T/g2.o docs/examples/using.za 2>/dev/null | sed -n '/^Source files/,$p' > $T/g2.txt
awk '/^<!-- listing: tables -->$/{f=1;next} f&&/^```/{if(inb){exit}else{inb=1;next}} inb{print}' docs/GUIDE.md > $T/g2.guide
cmp -s $T/g2.txt $T/g2.guide; check "docs/GUIDE.md shows the using.za tables" $? 0
$X -l $T/nodir/x.lst -o $T/hl.o docs/examples/hello.za 2>/dev/null; check "-l to an unwritable file" $? 1

# hlasm2za: HLASM source to zosasm source
H=build/hlasm2za
$H -o $T/prog.za testdata/hlasm/prog.asm 2>$T/err; check "hlasm2za exit" $? 0
[ ! -s $T/err ]; check "hlasm2za: no warnings for plain code" $? 0
cmp -s $T/prog.za testdata/hlasm/prog.za; check "hlasm2za output is testdata/hlasm/prog.za" $? 0
$X -strict-registers -Werror -o $T/prog.o $T/prog.za 2>$T/err; check "the converted program assembles with -strict-registers" $? 0
$H -relaxed -o $T/progr.za testdata/hlasm/prog.asm 2>/dev/null
$X -gz0 -Werror -o $T/progr.o $T/progr.za 2>/dev/null; check "-relaxed output assembles" $? 0
$X -gz0 -strict-registers -o $T/prog0.o $T/prog.za 2>/dev/null
cmp -s $T/prog0.o $T/progr.o; check "strict and relaxed conversions give the same object" $? 0
cp testdata/hlasm/prog.asm $T/cp.asm; $H $T/cp.asm 2>/dev/null; [ -s $T/cp.za ]; check "hlasm2za default output name" $? 0
$H -o - testdata/hlasm/prog.asm 2>/dev/null | cmp -s - testdata/hlasm/prog.za; check "hlasm2za -o -" $? 0
$H -macro-path "$T/macros" -o "$T/mp.za" testdata/hlasm/prog.asm 2>/dev/null
grep -q "^#pragma macro_path \"$T/macros\"$" "$T/mp.za"; check "hlasm2za -macro-path writes the pragma" $? 0
$H - < testdata/hlasm/prog.asm 2>/dev/null | cmp -s - testdata/hlasm/prog.za; check "hlasm2za from standard input" $? 0
$H -o - docs/examples/hlasm-demo.asm 2>/dev/null | cmp -s - docs/examples/hlasm-demo.za; check "docs/examples/hlasm-demo.za is the conversion of hlasm-demo.asm" $? 0
$X -strict-registers -Werror -o $T/demo.o docs/examples/hlasm-demo.za 2>/dev/null; check "hlasm-demo.za assembles with -strict-registers" $? 0
$H -comment-macros -o $T/nc.za testdata/hlasm/notconv.asm 2>$T/err; check "hlasm2za with unconvertible statements: exit 0" $? 0
grep -q 'notconv.asm:3: warning: macro definition commented out' $T/err; check "hlasm2za reports the macro" $? 0
grep -q 'notconv.asm:9: warning: &X is not declared' $T/err; check "hlasm2za converts AIF in open code and reports the undeclared variable" $? 0
grep -q '^#macro OPENCODE() {' $T/nc.za; check "hlasm2za wraps open code conditional assembly in OPENCODE" $? 0
grep -q '^#include "myregs.inc"$' $T/nc.za; check "COPY becomes #include" $? 0
$H -q -comment-macros -o $T/nc.za testdata/hlasm/notconv.asm 2>$T/err; [ ! -s $T/err ]; check "hlasm2za -q" $? 0
# macros: converted by default, MACRO..MEND to #macro
$H -o $T/nc2.za testdata/hlasm/notconv.asm 2>$T/err; check "hlasm2za converts the macro: exit" $? 0
grep -q '^#macro SETUP(a) {' $T/nc2.za; check "hlasm2za writes #macro" $? 0
grep -q 'macro definition commented out' $T/err; [ $? = 1 ]; check "hlasm2za: no commented out macro" $? 0
# macro library members
$H -m -o $T/COUNT.zmac testdata/hlasm/lib/COUNT.mac 2>/dev/null; check "hlasm2za -m exit" $? 0
grep -q '^#macro COUNT(n) {' $T/COUNT.zmac; check "hlasm2za -m: macro" $? 0
grep -q '#while (i < n) {' $T/COUNT.zmac; check "hlasm2za -m: loop recovered" $? 0
grep -q '#goto' $T/COUNT.zmac; [ $? = 1 ]; check "hlasm2za -m: no #goto" $? 0
grep -q '^\* A macro library member' $T/COUNT.zmac; check "hlasm2za -m: leading comment kept" $? 0
$H -m -flat -o $T/COUNTF.zmac testdata/hlasm/lib/COUNT.mac 2>/dev/null
grep -q '#goto loop' $T/COUNTF.zmac; check "hlasm2za -flat: #goto" $? 0
printf 'T CSECT\n#include "COUNT.zmac"\n COUNT 3\n END\n' > $T/cnt.za
$X -E -I$T $T/cnt.za 2>&1 | grep -c "DC *F'" | grep -q '^3$'; check "the converted macro expands" $? 0
mkdir -p $T/zm; $H -macro-dir testdata/hlasm/lib -o $T/zm > $T/sum 2>&1; check "hlasm2za -macro-dir exit" $? 0
[ -s $T/zm/COUNT.zmac ] && [ -s $T/zm/FIND.zmac ]; check "hlasm2za -macro-dir writes every member" $? 0
grep -q '2 members' $T/sum; check "hlasm2za -macro-dir summary" $? 0
# a member that cannot be written fails the batch (a directory in the way)
mkdir -p $T/zm2/COUNT.zmac; $H -macro-dir testdata/hlasm/lib -o $T/zm2 >/dev/null 2>&1
check "hlasm2za -macro-dir write failure is an error" $? 1
printf 'T CSECT\n#include "FIND.zmac"\n FIND 2\n END\n' > $T/fnd.za
$X -E -I$T/zm $T/fnd.za 2>&1 | grep -q "DC *F'2'"; check "the converted search loop expands" $? 0
printf 'T CSECT\n FIND 2\n END\n' > $T/fnd2.za
$X -E -M$T/zm $T/fnd2.za 2>&1 | grep -q "DC *F'2'"; check "-M finds FIND.zmac by the macro name (upper case file)" $? 0
# control flow of converted macros: #switch, dispatch loop, -keep-goto
$H -m -o $T/SW.zmac testdata/hlasm/lib2/SW.mac 2>/dev/null; check "hlasm2za SW exit" $? 0
grep -q '#switch (t) {' $T/SW.zmac; check "hlasm2za: arms that join become #switch" $? 0
$H -m -o $T/DSP.zmac testdata/hlasm/lib2/DSP.mac 2>/dev/null; check "hlasm2za DSP exit" $? 0
grep -q '#switch (pc) {' $T/DSP.zmac; check "hlasm2za: dispatch loop" $? 0
grep -q '#goto' $T/DSP.zmac; [ $? = 1 ]; check "hlasm2za: dispatch loop has no #goto" $? 0
$H -m -keep-goto -o $T/DSPK.zmac testdata/hlasm/lib2/DSP.mac 2>/dev/null; check "hlasm2za -keep-goto exit" $? 0
grep -q '#goto' $T/DSPK.zmac; check "hlasm2za -keep-goto: #goto" $? 0
grep -q '#switch (pc)' $T/DSPK.zmac; [ $? = 1 ]; check "hlasm2za -keep-goto: no dispatch loop" $? 0
$H -m -flat -o $T/DSPF.zmac testdata/hlasm/lib2/DSP.mac 2>/dev/null
for n in 1 3 9; do
    for v in DSP DSPK DSPF; do
        printf 'T CSECT\n#include "%s.zmac"\n DSP %s\n END\n' $v $n > $T/d.za
        $X -E -I$T $T/d.za 2>&1 | grep 'DC *A(' | tr -d ' ' > $T/d_$v.txt
    done
    cmp -s $T/d_DSP.txt $T/d_DSPF.txt && cmp -s $T/d_DSPK.txt $T/d_DSPF.txt; check "dispatch, keep-goto and flat expand alike (DSP $n)" $? 0
done
$H -o $T/none.za $T/missing.asm 2>/dev/null; check "hlasm2za missing file" $? 1
$H 2>/dev/null; check "hlasm2za without input" $? 2
$H -bogus x 2>/dev/null; check "hlasm2za bad option" $? 2
$H -version | grep -q '^hlasm2za 0'; check "hlasm2za -version" $? 0
printf '          lr 1,2\n' | $H -ictl 10,71,16 - 2>/dev/null | grep -q 'LR    %r1,%r2'; check "hlasm2za -ictl" $? 0

# ELF output and the character set
$X -elf -o $T/lh.o docs/examples/linux-hello.za 2>$T/err; check "-elf exit" $? 0
[ "$(od -An -tx1 -N6 $T/lh.o | tr -d ' ')" = "7f454c460202" ]; check "-elf writes an ELF64 big endian object" $? 0
grep -q 'Hello from s390x' $T/lh.o; check "-elf implies ASCII text" $? 0
$X -elf -charset ebcdic -o $T/lh2.o docs/examples/linux-hello.za 2>/dev/null; grep -q 'Hello from s390x' $T/lh2.o; [ $? = 1 ]; check "-charset ebcdic overrides" $? 0
$X -ascii -o $T/lh3.o docs/examples/linux-hello.za 2>/dev/null; check "-ascii exit" $? 0
$X -dump $T/lh3.o | grep -q ESD; check "-ascii still writes GOFF" $? 0
$X -g -gside -o $T/cbg.o docs/examples/cblock.za 2>$T/err; check "-g CBLOCK object exit" $? 0
if command -v readelf >/dev/null 2>&1; then
    readelf --debug-dump=info $T/cbg.dbg 2>/dev/null | grep -q 'DW_AT_name.*: x'; check "CBLOCK local in DWARF" $? 0
    readelf --debug-dump=info $T/cbg.dbg 2>/dev/null | grep -q 'DW_OP_breg'; check "CBLOCK local has a breg location" $? 0
fi
$X -charset latin -o $T/x.o docs/examples/linux-hello.za 2>/dev/null; check "-charset bad value" $? 2
printf '#ifdef __ELF__\nE\n#endif\n#ifdef __GOFF__\nG\n#endif\n#ifdef __ASCII__\nA\n#endif\n#ifdef __EBCDIC__\nB\n#endif\n' > $T/pd.za
[ "$($X -E $T/pd.za | tr -d '\n')" = "GB" ]; check "predefined macros: GOFF, EBCDIC" $? 0
[ "$($X -E -elf $T/pd.za | tr -d '\n')" = "EA" ]; check "predefined macros: ELF, ASCII" $? 0
[ "$($X -E -elf -charset ebcdic $T/pd.za | tr -d '\n')" = "EB" ]; check "predefined macros: ELF, EBCDIC" $? 0
[ "$($X -E -ascii $T/pd.za | tr -d '\n')" = "GA" ]; check "predefined macros: GOFF, ASCII" $? 0
printf 'T CSECT\n DC AD(A-B)\n EXTRN A,B\n END\n' > $T/sub.za
$X -elf -o $T/sub.o $T/sub.za 2>$T/err; check "-elf rejects a subtracted relocation" $? 1
grep -q 'no ELF form' $T/err; check "-elf says why" $? 0

# MCP server (build/zosasm-mcp): JSON-RPC lines on stdin and stdout
M=build/zosasm-mcp
mcp() { printf '%s\n' "$1" | $M 2>/dev/null; }
mcp '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' > $T/m.out; check "mcp initialize exit" $? 0
grep -q '"serverInfo":{"name":"zosasm-mcp"' $T/m.out; check "mcp initialize server name" $? 0
grep -q 'docs/GUIDE.md' $T/m.out; check "mcp instructions point to the guide" $? 0
mcp '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' > $T/m.out
for tl in zosasm_assemble zosasm_preprocess zosasm_dump zosasm_disasm zosasm_convert_hlasm zosasm_guide; do
    grep -q "\"name\":\"$tl\"" $T/m.out; check "mcp tools/list has $tl" $? 0
done
grep -q '"object_base64"' $T/m.out; check "mcp tools/list documents the output fields" $? 0
mcp '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"zosasm_assemble","arguments":{"source":"T CSECT\n LR 1,2\n","listing":true}}}' > $T/m.out
grep -q '"success":true' $T/m.out; check "mcp assemble ok" $? 0
grep -q '"object_size":' $T/m.out; check "mcp assemble object size" $? 0
grep -q 'Loc     Object' $T/m.out; check "mcp assemble listing" $? 0
mcp '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"zosasm_assemble","arguments":{"source":"T CSECT\n LA 1,UNDEF\n"}}}' > $T/m.out
grep -q '"success":false' $T/m.out; check "mcp assemble failure" $? 0
grep -q '"isError":true' $T/m.out; check "mcp assemble isError" $? 0
grep -q 'ZA1011' $T/m.out; check "mcp assemble diagnostics" $? 0
mcp '{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"zosasm_assemble","arguments":{"source":"#include \"i.h\"\n","files":{"i.h":"T CSECT\n LR 1,2\n"},"format":"elf","object_base64":true}}}' > $T/m.out
grep -q '"format":"elf"' $T/m.out; check "mcp assemble elf with files" $? 0
grep -q '"object_base64":"f0VMRg' $T/m.out; check "mcp assemble object_base64 is an ELF" $? 0
mcp '{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"zosasm_preprocess","arguments":{"source":"#define X 5\n LA 1,X\n"}}}' > $T/m.out
grep -q 'LA 1,5' $T/m.out; check "mcp preprocess" $? 0
mcp '{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"zosasm_guide","arguments":{"section":"13"}}}' > $T/m.out
grep -q 'Command reference' $T/m.out; check "mcp guide section" $? 0
mcp '{"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"zosasm_guide","arguments":{"search":"strict-registers"}}}' > $T/m.out
grep -q 'line [0-9]* ' $T/m.out; check "mcp guide search" $? 0
mcp '{"jsonrpc":"2.0","id":9,"method":"resources/read","params":{"uri":"zosasm://examples/hello.za"}}' > $T/m.out
grep -q 'HELLO    CSECT' $T/m.out; check "mcp resource example" $? 0
mcp '{"jsonrpc":"2.0","id":10,"method":"nosuch"}' | grep -q '"code":-32601'; check "mcp unknown method" $? 0
printf 'not json\n' | $M 2>/dev/null | grep -q '"code":-32700'; check "mcp parse error" $? 0
# round trip: assemble to base64, then dump and disassemble through the server
mcp '{"jsonrpc":"2.0","id":11,"method":"tools/call","params":{"name":"zosasm_assemble","arguments":{"source":"T CSECT\n LR 1,2\n","object_base64":true}}}' > $T/m.out
b64=$(sed 's/.*"object_base64":"\([^"]*\)".*/\1/' $T/m.out | head -1)
[ -n "$b64" ]; check "mcp base64 extracted" $? 0
mcp '{"jsonrpc":"2.0","id":12,"method":"tools/call","params":{"name":"zosasm_dump","arguments":{"object_base64":"'"$b64"'"}}}' | grep -q 'ESD'; check "mcp dump" $? 0
mcp '{"jsonrpc":"2.0","id":13,"method":"tools/call","params":{"name":"zosasm_disasm","arguments":{"object_base64":"'"$b64"'"}}}' | grep -q 'LR'; check "mcp disasm" $? 0

mcp '{"jsonrpc":"2.0","id":14,"method":"tools/call","params":{"name":"zosasm_convert_hlasm","arguments":{"macros":true,"source":"         MACRO\n         M1    &A\n         AIF   (\u0027&A\u0027 EQ \u0027\u0027).E\n         DC    C\u0027&A\u0027\n         MEXIT\n.E       DC    C\u0027-\u0027\n         MEND\n"}}}' > $T/m.out
grep -q '"success":true' $T/m.out; check "mcp convert macros: success" $? 0
grep -q '#macro M1(a)' $T/m.out; check "mcp convert macros: #macro" $? 0
mcp '{"jsonrpc":"2.0","id":15,"method":"tools/call","params":{"name":"zosasm_convert_hlasm","arguments":{"macros":true,"flat":true,"source":"         MACRO\n         M1    &A\n         AIF   (\u0027&A\u0027 EQ \u0027\u0027).E\n         DC    C\u0027&A\u0027\n         MEXIT\n.E       DC    C\u0027-\u0027\n         MEND\n"}}}' > $T/m.out
grep -q '#goto e' $T/m.out; check "mcp convert macros: flat" $? 0
cp -r testdata/hlasm/lib2 $T/lib2
mcp '{"jsonrpc":"2.0","id":16,"method":"tools/call","params":{"name":"zosasm_convert_hlasm","arguments":{"macro_dir":"cli_tmp/lib2","output_dir":"cli_tmp/mz","keep_goto":true}}}' > $T/m.out
grep -q '"success":true' $T/m.out; check "mcp convert macro_dir: success" $? 0
[ -s $T/mz/SW.zmac ] && [ -s $T/mz/DSP.zmac ]; check "mcp convert macro_dir: files written" $? 0
mcp '{"jsonrpc":"2.0","id":17,"method":"tools/call","params":{"name":"zosasm_convert_hlasm","arguments":{"macro_dir":"cli_tmp/lib2"}}}' > $T/m.out
grep -q '"isError":true' $T/m.out; check "mcp convert macro_dir needs output_dir" $? 0
mcp '{"jsonrpc":"2.0","id":18,"method":"tools/call","params":{"name":"zosasm_preprocess","arguments":{"source":"T CSECT\n DSP 1\n","macro_dirs":["cli_tmp/mz"],"sysparm":"X","macro_steps":5000}}}' > $T/m.out
grep -q 'A(4)' $T/m.out; check "mcp preprocess with macro_dirs" $? 0
mcp '{"jsonrpc":"2.0","id":19,"method":"tools/call","params":{"name":"zosasm_assemble","arguments":{"source":"T CSECT\n GREET\n","files":{"GREET.zmac":"#macro GREET() {\n #emit \"         DC    C\u0027\" + sys.parm + \"\u0027\"\n#}\n"},"sysparm":"HI"}}}' > $T/m.out
grep -q '"success":true' $T/m.out; check "mcp assemble: macro from files and sysparm" $? 0
mcp '{"jsonrpc":"2.0","id":20,"method":"tools/call","params":{"name":"zosasm_assemble","arguments":{"source":"T CSECT\n","macro_steps":0}}}' > $T/m.out
grep -q '"isError":true' $T/m.out && grep -q "'macro_steps' must be a positive integer" $T/m.out; check "mcp assemble: bad macro_steps" $? 0

# zafmt: the source formatter
F=build/zafmt
printf 'loop lr 1,2 r\n\n\n\n\n#macro M() {\n LR 1,2\n#}\n' > $T/f1.za
$F $T/f1.za > $T/f1.out; check "zafmt exit" $? 0
grep -q '^loop     lr    1,2' $T/f1.out; check "zafmt puts the operation in column 10" $? 0
grep -q '^    LR    1,2$' $T/f1.out; check "zafmt indents a block" $? 0
$F -check $T/f1.za > $T/f1.chk; check "zafmt -check: unformatted file" $? 1
grep -q f1.za $T/f1.chk; check "zafmt -check names the file" $? 0
cp $T/f1.za $T/f2.za; $F -i $T/f2.za; check "zafmt -i" $? 0
cmp -s $T/f2.za $T/f1.out; check "zafmt -i writes the formatted text" $? 0
$F -check $T/f2.za; check "zafmt -check: formatted file" $? 0
$F < $T/f1.za | cmp -s - $T/f1.out; check "zafmt from standard input" $? 0
$F -o $T/f3.za $T/f1.za; cmp -s $T/f3.za $T/f1.out; check "zafmt -o" $? 0
$F -op 12 $T/f1.za | grep -q '^loop       lr    1,2'; check "zafmt -op" $? 0
$F -bad $T/f1.za 2>/dev/null; check "zafmt unknown option" $? 2
$F -check docs/examples/linkage.za docs/examples/table.za docs/examples/hello.za; check "zafmt: the examples are formatted" $? 0

# -g: debug information (the DWARF in the object; -gside also writes the .dbg)
mkdir -p $T/g
printf 'PB CSECT\n USING *,15\nL1 LR 1,1\n BR 14\nD1 DC F'"'"'1'"'"'\n END\n' > $T/g/pb.za
$X -gside -gmap $P/$T/g=/u/me/w -o $T/g/pb.o $T/g/pb.za 2>/dev/null; check "-gside exit" $? 0
[ -s $T/g/pb.o ] && [ -s $T/g/pb.dbg ]; check "-gside writes the object and FILE.dbg" $? 0
$X -g -gmap $P/$T/g=/u/me/w -o $T/g/nd.o $T/g/pb.za 2>/dev/null; check "-g exit" $? 0
[ -s $T/g/nd.o ] && [ ! -e $T/g/nd.dbg ]; check "-g writes the object and no .dbg" $? 0
$X -dump $T/g/nd.o | grep -q 'C_ADATAE9C3'; check "-g: the object holds the DWARF in C_ADATAE9C3" $? 0
head -c 4 $T/g/pb.dbg | od -An -c | grep -q 'E   L   F'; check "-g: the .dbg is an ELF file" $? 0
$X -dump $T/g/pb.o | grep -q '=== module 2'; check "-g: the object holds a second module" $? 0
$X -dump $T/g/pb.o | grep -q 'D_@@ADBG_DBG'; check "-g: the link module has D_@@ADBG_DBG" $? 0
strings -a $T/g/pb.dbg | grep -q '^/u/me/w/pb.za$'; check "-g -gmap: the source path is the z/OS path" $? 0
$X -disasm $T/g/pb.o | grep -q 'PB       CSECT'; check "-g: -disasm reads the first module" $? 0
$X -g -gdwarf $T/g/x.dbg -o $T/g/x.o $T/g/pb.za 2>/dev/null; [ -s $T/g/x.dbg ]; check "-gdwarf FILE" $? 0
$X -gdir /u/me -gside -o $T/g/y.o $T/g/pb.za 2>/dev/null; [ -s $T/g/y.dbg ]; check "-gdir with -gside writes FILE.dbg" $? 0
$X -gside -elf -o $T/g/z.o $T/g/pb.za 2>/dev/null; check "-gside with -elf is an error" $? 2
$X -g -elf -o $T/g/z.o $T/g/pb.za 2>/dev/null; check "-g with -elf exit" $? 0
[ -s $T/g/z.o ] && [ ! -e $T/g/z.dbg ] && grep -q '\.debug_info' $T/g/z.o && grep -q '\.rela\.debug_info' $T/g/z.o; check "-g -elf: DWARF sections in the object, no .dbg" $? 0
$X -g -gdbgpath /u/me/$(printf 'x%.0s' $(seq 1 210)).dbg -o $T/g/w.o $T/g/pb.za 2>/dev/null; check "-gdbgpath too long" $? 1
cp $T/g/pb.o $T/g/a.o; cp $T/g/pb.dbg $T/g/a.dbg
$X -gside -gmap $P/$T/g=/u/me/w -o $T/g/b.o $T/g/pb.za 2>/dev/null; cmp -s $T/g/a.dbg $T/g/b.dbg; check "-gside is reproducible" $? 0

# expression depth/overflow regressions
printf 'T CSECT\n L 1,%s1%s\n END\n' "$(python3 -c "print('('*205)")" "$(python3 -c "print(')'*205)")" > $T/deep.za
$X -o $T/deep.o $T/deep.za 2>$T/deep.err; check "expr depth limit: rc" $? 1
grep -q 'ZA1004' $T/deep.err; check "expr depth limit: diagnostic" $? 0
printf "T CSECT\n DC A((X'FFFFFFFF'*X'FFFFFFFF')/2)\n END\n" > $T/mul.za
$X -o $T/mul.o $T/mul.za 2>$T/mul.err; check "expr mul overflow: rc" $? 1
grep -q 'ZA1014' $T/mul.err; check "expr mul overflow: diagnostic" $? 0
printf "T CSECT\n DC A((X'80000000'*X'80000000'*2)/-1)\n END\n" > $T/div.za
$X -o $T/div.o $T/div.za 2>$T/div.err; check "expr INT64_MIN/-1: rc" $? 1
grep -q 'ZA1014' $T/div.err; check "expr INT64_MIN/-1: diagnostic" $? 0

# ^ column anchor on continuation line
cat > $T/caret.za <<'ZA'
         CSECT
X        DC    A(1),
      ^ A(2)
         END
ZA
$X -o $T/caret.o $T/caret.za 2>$T/caret.err; check "caret continuation: rc" $? 0
$X -dump $T/caret.o | grep -q '00 00 00 01 00 00 00 02'; check "caret continuation: bytes" $? 0

mcp '{"jsonrpc":"2.0","id":21,"method":"tools/call","params":{"name":"zosasm_assemble","arguments":{"source":"T CSECT\n LR 1,1\n","dwarf":true,"dwarf_base64":true,"dwarf_dir":"/u/x"}}}' > $T/m.out
grep -q '"dwarf_size":' $T/m.out && grep -q '"dwarf_base64":"f0VMR' $T/m.out; check "mcp assemble: dwarf" $? 0
mcp '{"jsonrpc":"2.0","id":22,"method":"tools/call","params":{"name":"zosasm_assemble","arguments":{"source":"T CSECT\n","dwarf":true,"dwarf_output":"x.dbg","format":"elf"}}}' > $T/m.out
grep -q '"isError":true' $T/m.out; check "mcp assemble: dwarf_output with elf" $? 0
mcp '{"jsonrpc":"2.0","id":23,"method":"tools/call","params":{"name":"zosasm_assemble","arguments":{"source":"T CSECT\n","dwarf":true,"format":"elf","object_base64":true}}}' > $T/m.out
! grep -q '"isError":true' $T/m.out; check "mcp assemble: dwarf with elf (DWARF in the object)" $? 0

# the workspace: with no ZOSASM_UNRESTRICTED, file names are limited to the
# directory of the server binary (build/)
printf 'T CSECT\n LR 1,2\n' > $T/ws.za
mcp '{"jsonrpc":"2.0","id":28,"method":"tools/call","params":{"name":"zosasm_assemble","arguments":{"path":"/etc/passwd"}}}' > $T/m.out
grep -q '"isError":true' $T/m.out && grep -q 'outside the workspace' $T/m.out; check "mcp path outside the workspace" $? 0
mcp '{"jsonrpc":"2.0","id":24,"method":"tools/call","params":{"name":"zosasm_assemble","arguments":{"path":"cli_tmp/ws.za"}}}' > $T/m.out
grep -q '"success":true' $T/m.out; check "mcp path inside the workspace" $? 0
evil=/tmp/zosasm-mcp-evil-$$.o
mcp '{"jsonrpc":"2.0","id":29,"method":"tools/call","params":{"name":"zosasm_assemble","arguments":{"source":"T CSECT\n","output":"'$evil'"}}}' > $T/m.out
grep -q 'outside the workspace' $T/m.out; check "mcp output outside the workspace" $? 0
[ ! -e "$evil" ]; check "mcp output outside the workspace: nothing written" $? 0
mcp '{"jsonrpc":"2.0","id":26,"method":"tools/call","params":{"name":"zosasm_assemble","arguments":{"path":"../docs/GUIDE.md"}}}' > $T/m.out
grep -q 'outside the workspace' $T/m.out; check "mcp .. out of the workspace is rejected" $? 0
# ZOSASM_UNRESTRICTED lifts the limit
printf '%s\n' '{"jsonrpc":"2.0","id":27,"method":"tools/call","params":{"name":"zosasm_assemble","arguments":{"path":"'$P'/testdata/hello.za"}}}' | ZOSASM_UNRESTRICTED=1 $M 2>/dev/null > $T/m.out
grep -q '"success":true' $T/m.out; check "mcp ZOSASM_UNRESTRICTED lifts the limit" $? 0

rm -rf $T
[ $fail = 0 ] && echo "tests/run_cli_tests.sh: ok"
exit $fail
