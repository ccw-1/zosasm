#!/usr/bin/env python3
"""Run the probe sources of tools/oracle_gen.c through the real HLASM and
zosasm and list where they disagree.

    oracle_probe.py MODE CHUNK [OUTDIR]       MODE = base | reg | range

The generated source is cut into chunks of CHUNK statements (HLASM stops after
500 errors), each chunk is assembled with tools/oracle.sh, and every statement
is compared: accepted or rejected, and the object bytes.  Writes OUTDIR/MODE.dis
(one line per disagreement) and OUTDIR/MODE.all (one line per statement:
line index mnemonic kind value hlasm zosasm).
"""
import os
import re
import subprocess
import sys

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.dirname(here)


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, **kw)


def zosasm_errors(path):
    errs = {}
    cur = None
    for line in open(path, encoding='latin-1'):
        m = re.match(r'(error|warning)\[(ZA\d+)\]: (.*)', line)
        if m:
            cur = (m.group(1), m.group(2), m.group(3))
            continue
        m = re.match(r'\s*--> .*:(\d+):\d+', line)
        if m and cur:
            if cur[0] == 'error':
                errs.setdefault(int(m.group(1)), []).append(cur[1] + ' ' + cur[2])
            cur = None
    return errs


def zosasm_objects(path):
    objs = {}
    for line in open(path, encoding='latin-1'):
        m = re.match(r'^([0-9A-F]{6})\s+([0-9A-F]+(?: [0-9A-F]+)*?)\s{2,}(\d+)\s+(\S.*?)\s*$', line)
        if m:
            objs[int(m.group(3))] = m.group(2).replace(' ', '')
    return objs


def main():
    mode, chunk = sys.argv[1], int(sys.argv[2])
    out = sys.argv[3] if len(sys.argv) > 3 else '/tmp/lt/or'
    gen = os.path.join(root, 'build', 'oracle_gen')
    args = [] if mode == 'base' else ['-p', mode]
    asm = subprocess.run([gen] + args, check=True, capture_output=True, text=True).stdout.split('\n')
    idx = subprocess.run([gen, '-i'] + args, check=True, capture_output=True, text=True).stdout.strip().split('\n')
    body = asm[2:-2]            # statements between "DS 0H" and END (and the trailing empty line)
    assert len(body) == len(idx), (len(body), len(idx))
    os.makedirs(out, exist_ok=True)
    allf = open(os.path.join(out, mode + '.all'), 'w')
    disf = open(os.path.join(out, mode + '.dis'), 'w')
    nstat = ndis = 0
    for c in range(0, len(body), chunk):
        lines = body[c:c + chunk]
        name = '%s_%03d' % (mode, c // chunk)
        src = os.path.join(out, name + '.asm')
        with open(src, 'w') as f:
            f.write('T        CSECT\n         DS    0H\n' + '\n'.join(lines) + '\n         END\n')
        env = dict(os.environ, ZORACLE_OUT=out)
        run([os.path.join(here, 'oracle.sh'), src], env=env, stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL)
        h = {}
        for l in open(os.path.join(out, name + '.tsv'), encoding='latin-1'):
            n, obj, msgs, s = l.rstrip('\n').split('\t')
            h[int(n)] = (obj, [m for m in msgs.split(' | ') if m], s)
        zerr = zosasm_errors(os.path.join(out, name + '.zerr'))
        zobj = zosasm_objects(os.path.join(out, name + '.zlst'))
        for k, l in enumerate(idx[c:c + chunk]):
            p = l.split()
            n = k + 3
            obj, msgs, s = h.get(n, ('', ['missing from listing'], ''))
            herr = [m for m in msgs if re.match(r'ASMA\d+[ESCU]', m)]
            zer = zerr.get(n, [])
            zo = zobj.get(n, '')
            hs = 'ERR' if herr else 'ok'
            zs = 'ERR' if zer else 'ok'
            nstat += 1
            rec = '%s %s %s %s %s %s | %s | %s' % (p[2], p[3], p[4], p[5], hs, zs, ';'.join(herr), ';'.join(zer))
            allf.write(rec + ' | ' + s + '\n')
            if hs != zs or (hs == 'ok' and obj != zo):
                ndis += 1
                disf.write('%s | hlasm=%s zosasm=%s | %s\n' % (rec, obj, zo, s))
        sys.stderr.write('chunk %d done\n' % (c // chunk))
    print('%d statements, %d disagreements' % (nstat, ndis))


main()
