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
$H - < testdata/hlasm/prog.asm 2>/dev/null | cmp -s - testdata/hlasm/prog.za; check "hlasm2za from standard input" $? 0
$H -o - docs/examples/hlasm-demo.asm 2>/dev/null | cmp -s - docs/examples/hlasm-demo.za; check "docs/examples/hlasm-demo.za is the conversion of hlasm-demo.asm" $? 0
$X -strict-registers -Werror -o $T/demo.o docs/examples/hlasm-demo.za 2>/dev/null; check "hlasm-demo.za assembles with -strict-registers" $? 0
$H -o $T/nc.za testdata/hlasm/notconv.asm 2>$T/err; check "hlasm2za with unconvertible statements: exit 0" $? 0
grep -q 'notconv.asm:3: warning: macro definition commented out' $T/err; check "hlasm2za reports the macro" $? 0
grep -q 'notconv.asm:9: warning: AIF is conditional assembly' $T/err; check "hlasm2za reports AIF" $? 0
grep -q '^#include "myregs.inc"$' $T/nc.za; check "COPY becomes #include" $? 0
$H -q -o $T/nc.za testdata/hlasm/notconv.asm 2>$T/err; [ ! -s $T/err ]; check "hlasm2za -q" $? 0
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

rm -rf $T
[ $fail = 0 ] && echo "tests/run_cli_tests.sh: ok"
exit $fail
