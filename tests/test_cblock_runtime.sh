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

echo "test_cblock_runtime: $((case_n - fail))/$case_n passed"
[ "$fail" -eq 0 ]
