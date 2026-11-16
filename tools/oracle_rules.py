#!/usr/bin/env python3
"""Derive the operand rules that HLASM applies beyond the field width from the
probe runs of tools/oracle_probe.py (reg and range) and write them as
isa/oracle_rules.json, which tools/pop2json.py merges into isa/isa.json.

    oracle_rules.py [PROBEDIR [OUTFILE]]        PROBEDIR defaults to /tmp/lt/pr

Rules, one string of space separated tokens per "mnemonic format":
    e<k>        GPR operand k must be even (register pair)
    f<k>        FPR operand k must start a pair (0,1,4,5,8,9,12,13)
    s<k> n<k>   immediate k: signed / unsigned window; a value outside the window
                is accepted with warning ASMA320W and truncated to the field
    u<k>=HEX    mask k: values 0..15 whose bit is set in HEX are rejected
    L<k>=N      length k: largest accepted length
    E<k>        length k must be even
    D<k>        length k must be shorter than the previous length (dp, mp)
    r<k>=N      immediate k must be 0..N (after the truncation of an s/n rule)
Operand numbers k count the assembler operands from 0, as isa_slots() does.
"""
import collections
import json
import os
import re
import subprocess
import sys

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.dirname(here)
probe = sys.argv[1] if len(sys.argv) > 1 else '/tmp/lt/pr'
outfile = sys.argv[2] if len(sys.argv) > 2 else os.path.join(root, 'isa', 'oracle_rules.json')
gen = os.path.join(root, 'build', 'oracle_gen')


def load(mode, chunk):
    idx = subprocess.run([gen, '-i', '-p', mode], check=True, capture_output=True,
                         text=True).stdout.strip().split('\n')
    for c in range(0, len(idx), chunk):
        h = {}
        for l in open(os.path.join(probe, '%s_%03d.tsv' % (mode, c // chunk)), encoding='latin-1'):
            n, obj, msgs, s = l.rstrip('\n').split('\t')
            h[int(n)] = (obj, msgs)
        for k, l in enumerate(idx[c:c + chunk]):
            p = l.split()
            obj, msgs = h.get(k + 3, ('', ''))
            err = bool(re.search(r'ASMA\d+[ESC]', msgs))
            warn = 'ASMA320W' in msgs
            yield p[2], p[3], p[4], p[5], err, warn


def slot(kind):
    return int(kind[1:])


rules = collections.defaultdict(list)

# register pairs
acc = collections.defaultdict(set)
rej = collections.defaultdict(set)
for mn, fmt, kind, val, err, warn in load('reg', 300):
    if kind[0] != 'R':
        continue
    try:
        v = int(val)
    except ValueError:
        continue
    (rej if err else acc)[(mn, fmt, kind)].add(v)
for key in sorted(acc.keys() | rej.keys()):
    a, r = acc[key], rej[key]
    mn, fmt, kind = key
    k = slot(kind)
    vals = a | r
    if 2 in vals:  # an FPR probe: 1 2 3 5 15 16
        if {2, 3, 15} <= r and {1, 5} <= a:
            rules[(mn, fmt)].append('f%d' % k)
    elif 5 in vals and {5, 15} <= r:  # a GPR probe: 5 15 16
        rules[(mn, fmt)].append('e%d' % k)

# immediates, masks and lengths
st = collections.defaultdict(lambda: {'clean': set(), 'warn': set(), 'err': set()})
bits = {}
for mn, fmt, kind, val, err, warn in load('range', 250):
    try:
        v = int(val)
    except ValueError:
        continue
    st[(mn, fmt, kind)]['err' if err else 'warn' if warn else 'clean'].add(v)

for key in sorted(st):
    mn, fmt, kind = key
    d = st[key]
    if mn in ('brc', 'cijh', 'clijh'):  # *+14 is noise in the probe statement stream
        continue
    k = slot(kind)
    if kind[0] == 'I':
        if d['warn']:
            lo = min(d['clean']) if d['clean'] else 0
            rules[(mn, fmt)].append(('s%d' if lo < 0 else 'n%d') % k)
        elif d['err'] and (-1 in d['err'] or -8 in d['err']) and 0 in d['clean'] and \
                32 in d['clean'] | d['err']:
            # a 4-bit field HLASM reads as an unsigned mask: the probes give the largest value
            # accepted
            rules[(mn, fmt)].append('r%d=%d' % (k, max(d['clean'])))
        elif d['err'] and max(d['clean'] | d['err']) <= 16:
            bad = 0
            for v in d['err']:
                if 0 <= v <= 15:
                    bad |= 1 << v
            if bad:
                rules[(mn, fmt)].append('u%d=%X' % (k, bad))
    elif kind[0] == 'M':
        a = sorted(d['clean'] | d['warn'])
        if not a or 0 not in set(a) | d['err']:  # M also probes index and base registers
            continue
        if mn in ('dp', 'mp'):
            rules[(mn, fmt)].append('D%d' % k)
            continue
        top = max(a)
        full = 256 if max(set(a) | d['err']) >= 128 else 16
        if top != full:
            rules[(mn, fmt)].append('L%d=%d' % (k, top))
        if all(v % 2 == 0 for v in a) and any(v % 2 == 1 for v in d['err']):
            rules[(mn, fmt)].append('E%d' % k)

# found with single statements (the probes only try 0 and 15 there)
rules[('srp', 'SS-c')] = [t for t in rules[('srp', 'SS-c')] if not t.startswith('r2')] + ['r2=9']
# MC: the mask is the low byte, HLASM truncates (warning) and wants 0-15 of it
rules[('mc', 'SI')] += ['n1', 'r1=15']

out = {'%s %s' % k: ' '.join(sorted(v)) for k, v in sorted(rules.items())}
with open(outfile, 'w') as f:
    json.dump(out, f, indent=0, sort_keys=True)
    f.write('\n')
print('%d instructions with rules -> %s' % (len(out), outfile))
