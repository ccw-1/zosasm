#!/bin/sh
# Run generated CBLOCK code as an ELF program under qemu-s390x.
# Requires s390x-linux-gnu-ld and qemu-s390x; skips when they are absent.
set -u
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
z=$root/build/zosasm
ld=${S390X_LD:-s390x-linux-gnu-ld}
qemu=${QEMU_S390X:-qemu-s390x}
command -v "$ld" >/dev/null 2>&1 || { echo "test_cblock_runtime: $ld not found, skip"; exit 0; }
command -v "$qemu" >/dev/null 2>&1 || { echo "test_cblock_runtime: $qemu not found, skip"; exit 0; }
tmp=$(mktemp -d "${TMPDIR:-/tmp}/cblock.XXXXXX")
trap 'rm -rf "$tmp"' EXIT INT TERM
fail=0
case_n=0

# run_case NAME SETUP CIN COUT BODY EXPECTED
run_case() {
    name=$1; setup=$2; cin=$3; cout=$4; body=$5; want=$6
    case_n=$((case_n + 1))
    cat > "$tmp/$name.za" <<EOS
_START CSECT
         LGR   %r12,%r15
         AGHI  %r12,-128
$setup
#pragma cblock $name
$cin
$cout
#pragma cscratch %r0, %r1, %r5, %r6
#pragma ctemp %r12, 128, down
#pragma cbegin
$body
#pragma cend
         LR    %r2,%r4
         LGHI  %r1,1
         SVC   1
         END
EOS
    if ! "$z" -elf -o "$tmp/$name.o" "$tmp/$name.za" >"$tmp/$name.zerr" 2>&1; then
        echo "FAIL $name: zosasm: $(head -1 "$tmp/$name.zerr")"
        fail=$((fail + 1))
        return
    fi
    if ! "$ld" -e _START -o "$tmp/$name.elf" "$tmp/$name.o" >"$tmp/$name.ld" 2>&1; then
        echo "FAIL $name: ld: $(head -1 "$tmp/$name.ld")"
        fail=$((fail + 1))
        return
    fi
    "$qemu" "$tmp/$name.elf" >/dev/null 2>&1
    got=$?
    if [ "$got" -ne "$want" ]; then
        echo "FAIL $name: exit $got, want $want"
        fail=$((fail + 1))
    else
        echo "ok   $name: $got"
    fi
}

run_case add '         LHI   %r2,20
         LHI   %r3,22' \
    '#pragma cin int32_t a=%r2, int32_t b=%r3' '#pragma cout int32_t c=%r4' \
    'c = a + b;' 42
run_case mulshift '         LHI   %r2,3
         LHI   %r3,4' \
    '#pragma cin int32_t a=%r2, int32_t b=%r3' '#pragma cout int32_t c=%r4' \
    'c = (a << 2) + b;' 16
run_case divs '         LHI   %r2,-7
         LHI   %r3,2' \
    '#pragma cin int32_t a=%r2, int32_t b=%r3' '#pragma cout int32_t c=%r4' \
    'c = a / b;' 253
run_case mods '         LHI   %r2,-7
         LHI   %r3,2' \
    '#pragma cin int32_t a=%r2, int32_t b=%r3' '#pragma cout int32_t c=%r4' \
    'c = a % b;' 255
run_case divu '         LHI   %r2,100
         LHI   %r3,9' \
    '#pragma cin uint32_t a=%r2, uint32_t b=%r3' '#pragma cout uint32_t c=%r4' \
    'c = a / b;' 11
run_case cmp '         LHI   %r2,-1
         LHI   %r3,0' \
    '#pragma cin int32_t a=%r2, int32_t b=%r3' '#pragma cout int32_t c=%r4' \
    'c = a < b;' 1
run_case local '         LHI   %r2,5' \
    '#pragma cin int32_t a=%r2' '#pragma cout int32_t c=%r4' \
    'int32_t t = a * 3; t += 1; c = t;' 16
run_case cast '         LHI   %r2,-5' \
    '#pragma cin int32_t a=%r2' '#pragma cout int64_t c=%r4' \
    'c = (int64_t)a;' 251
run_case wide '         LGHI  %r2,100
         LGHI  %r3,9' \
    '#pragma cin int64_t a=%r2, int64_t b=%r3' '#pragma cout int64_t c=%r4' \
    'c = a / b;' 11
run_case cmp64 '         LGHI  %r2,100
         LGHI  %r3,9' \
    '#pragma cin int64_t a=%r2, int64_t b=%r3' '#pragma cout int32_t c=%r4' \
    'c = a < b;' 0

run_case ifelse '         LHI   %r2,3
         LHI   %r3,7' \
    '#pragma cin int32_t a=%r2, int32_t b=%r3' '#pragma cout int32_t c=%r4' \
    'int32_t t; if (a < b) { t = a; } else { t = b; } c = t + 1;' 4
run_case whileloop '' '' \
    '#pragma cout int32_t c=%r4' \
    'int32_t i = 0; int32_t t = 0; while (i < 4) { t += i; i++; } c = t;' 6
run_case forloop '' '' \
    '#pragma cout int32_t c=%r4' \
    'int32_t t = 0; for (int32_t i = 0; i < 4; i++) { t += i; } c = t;' 6
run_case dowhile '' '' \
    '#pragma cout int32_t c=%r4' \
    'int32_t i = 0; int32_t t = 0; do { t++; i++; } while (i < 3); c = t;' 3
run_case brkcont '' '' \
    '#pragma cout int32_t c=%r4' \
    'int32_t t = 0; for (int32_t i = 0; i < 10; i++) { if (i == 2) continue; if (i == 5) break; t++; } c = t;' 4
run_case ternlog '         LHI   %r2,0
         LHI   %r3,5' \
    '#pragma cin int32_t a=%r2, int32_t b=%r3' '#pragma cout int32_t c=%r4' \
    'c = (a && b) ? 7 : 9; c = c + (a || b);' 10
run_case incshort '         LHI   %r2,0
         LHI   %r3,0' \
    '#pragma cin int32_t a=%r2, int32_t b=%r3' '#pragma cout int32_t c=%r4' \
    'int32_t x = a; int32_t y = x++; int32_t z = ++x; c = y + z + (a && (10 / b));' 2

run_case switchcase '         LHI   %r2,2' \
    '#pragma cin int32_t a=%r2' '#pragma cout int32_t c=%r4' \
    'switch (a) { case 1: c = 10; break; case 2: c = 20; break; default: c = 30; }' 20
run_case switchfall '         LHI   %r2,1' \
    '#pragma cin int32_t a=%r2' '#pragma cout int32_t c=%r4' \
    'int32_t t = 0; switch (a) { case 1: t = 1; case 2: t += 2; break; default: t = 9; } c = t;' 3
run_case switchloop '' '' \
    '#pragma cout int32_t c=%r4' \
    'int32_t t = 0; for (int32_t i = 0; i < 4; i++) { switch (i) { case 1: continue; case 3: break; default: t++; } t += 10; } c = t;' 32
run_case gotolabel '' '' \
    '#pragma cout int32_t c=%r4' \
    'int32_t t = 1; goto skip; t = 99; skip: t += 2; c = t;' 3

# pointer/memory intrinsic cases: exit with the byte at the returned pointer
run_ptr_case() {
    name=$1; setup=$2; cin=$3; cout=$4; body=$5; want=$6
    case_n=$((case_n + 1))
    cat > "$tmp/$name.za" <<EOS
_START CSECT
         LGR   %r12,%r15
         AGHI  %r12,-1024
$setup
#pragma cblock $name
$cin
$cout
#pragma cscratch %r0, %r1, %r5, %r6
#pragma ctemp %r12, 128, down
#pragma cbegin
$body
#pragma cend
         IC    %r2,0(%r4)
         LGHI  %r1,1
         SVC   1
         END
EOS
    if ! "$z" -elf -o "$tmp/$name.o" "$tmp/$name.za" >"$tmp/$name.zerr" 2>&1; then
        echo "FAIL $name: zosasm: $(head -1 "$tmp/$name.zerr")"
        fail=$((fail + 1))
        return
    fi
    if ! "$ld" -e _START -o "$tmp/$name.elf" "$tmp/$name.o" >"$tmp/$name.ld" 2>&1; then
        echo "FAIL $name: ld: $(head -1 "$tmp/$name.ld")"
        fail=$((fail + 1))
        return
    fi
    "$qemu" "$tmp/$name.elf" >/dev/null 2>&1
    got=$?
    if [ "$got" -ne "$want" ]; then
        echo "FAIL $name: exit $got, want $want"
        fail=$((fail + 1))
    else
        echo "ok   $name: $got"
    fi
}

run_ptr_case memconst '         LGR   %r2,%r15
         AGHI  %r2,-512
         LGR   %r3,%r15
         AGHI  %r3,-256' \
    '#pragma cin uint8_t *d=%r2, uint8_t *s=%r3' '#pragma cout uint8_t *o=%r4' \
    '__memset(d, 65, 4); __memcpy(s, d, 4); o = s;' 65
run_ptr_case memrt '         LGR   %r2,%r15
         AGHI  %r2,-512
         LGR   %r3,%r15
         AGHI  %r3,-256
         LHI   %r7,4' \
    '#pragma cin uint8_t *d=%r2, uint8_t *s=%r3, int32_t n=%r7' '#pragma cout uint8_t *o=%r4' \
    '__memset(d, 65, n); __memcpy(s, d, n); o = s;' 65
run_ptr_case memchunk '         LGR   %r2,%r15
         AGHI  %r2,-800
         LGR   %r3,%r15
         AGHI  %r3,-512
         LHI   %r7,300' \
    '#pragma cin uint8_t *d=%r2, uint8_t *s=%r3, int32_t n=%r7' '#pragma cout uint8_t *o=%r4' \
    '__memset(d, 65, n); __memcpy(s, d, n); o = s + 299;' 65
run_case ptrdiff '         LGR   %r2,%r15' \
    '#pragma cinout uint8_t *p=%r2' '#pragma cout int32_t c=%r4' \
    'uint8_t *q = p; p += 4; q = p - 2; c = (int32_t)(p - q);' 2
run_ptr_case memmoveback '         LGR   %r3,%r15
         AGHI  %r3,-256' \
    '#pragma cin uint8_t *s=%r3' '#pragma cout uint8_t *o=%r4' \
    '__memset(s, 65, 1); __memset(s + 1, 66, 1); __memset(s + 2, 67, 1); __memset(s + 3, 68, 1); __memmove(s + 1, s, 3); o = s + 3;' 67
run_ptr_case memmovefwd '         LGR   %r3,%r15
         AGHI  %r3,-256' \
    '#pragma cin uint8_t *s=%r3' '#pragma cout uint8_t *o=%r4' \
    '__memset(s, 65, 1); __memset(s + 1, 66, 1); __memset(s + 2, 67, 1); __memset(s + 3, 68, 1); __memmove(s, s + 1, 3); o = s + 1;' 67

echo "test_cblock_runtime: $((case_n - fail))/$case_n passed"
[ "$fail" -eq 0 ]
