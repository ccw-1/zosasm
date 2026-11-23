#!/usr/bin/env python3
"""Compare the object bytes per statement of a HLASM listing and a zosasm
listing (zosasm -ldata -l).  Both assemble the same source, so the statement
number is the source line number.

    oracle_cmp.py hlasm.lst|hlasm.tsv zosasm.lst
    oracle_cmp.py --digest cp1047 hlasm.lst > hlasm.tsv   (on the z/OS host)

Prints one line per difference and a summary.  A statement for which HLASM
printed an error (ASMAnnnE/S) is listed as 'hlasm-error' with the messages.
"""
import re
import sys


def read_hlasm(path, enc='latin-1'):
    if path.endswith('.tsv'):
        return read_digest(path)
    stmts = {}  # n -> (bytes_hex, [messages], source)
    cur = None
    started = False
    with open(path, encoding=enc) as f:
        for line in f:
            line = line.rstrip('\n')
            if 'Source Statement' in line:
                started = True
                continue
            if started and re.search(r'(Cross Reference|Dictionary|Diagnostic Cross)', line[:80]):
                break
            if not started:
                continue
            if re.match(r'^. ?\*\*? ASMA', line) or line[:4] == '0** ':
                m = re.match(r'^.\s?\*\*\s(ASMA\d+[A-Z]) (.*?)\s*$', line)
                if m and cur is not None and not m.group(1).startswith('ASMA435'):
                    stmts[cur][1].append(m.group(1) + ' ' + m.group(2))
                continue
            if not re.match(r'^.([0-9A-F]{6,8}|\s{8}) ', line) or line[9] != ' ':
                continue
            m2 = re.match(r'^\s*(\d+)\s+(\S.*?)\s*$', line[42:])
            if not m2:
                # continuation of the object code (8 bytes per listing line)
                obj = line[10:27].replace(' ', '')
                if cur is not None and obj and re.fullmatch(r'[0-9A-F]+', obj) and not line[42:].strip():
                    stmts[cur][0] += obj
                continue
            n = int(m2.group(1))
            obj = line[10:27].replace(' ', '')
            if not re.fullmatch(r'[0-9A-F]*', obj):
                obj = ''
            stmts[n] = [obj, [], m2.group(2)]
            cur = n
    return stmts


def read_digest(path):
    stmts = {}
    for line in open(path, encoding='latin-1'):
        n, obj, msgs, src = line.rstrip('\n').split('\t')
        stmts[int(n)] = [obj, [m for m in msgs.split(' | ') if m], src]
    return stmts


def write_digest(h):
    for n in sorted(h):
        obj, msgs, src = h[n]
        print('%d\t%s\t%s\t%s' % (n, obj, ' | '.join(msgs), src))


def read_zosasm(path):
    """statement number (source line) -> (object hex, source text)"""
    stmts = {}
    last = None
    with open(path, encoding='latin-1') as f:
        for line in f:
            m = re.match(r'^([0-9A-F]{6})\s+([0-9A-F]+(?: [0-9A-F]+)*?)\s{2,}(\d+)\s+(\S.*?)\s*$', line)
            if m:
                stmts[int(m.group(3))] = (m.group(2).replace(' ', ''), m.group(4))
                last = int(m.group(3))
                continue
            m = re.match(r'^[0-9A-F]{6}\s+(\d{1,5})\s+(\S.*?)\s*$', line) or re.match(r'^\s{20,}\s*(\d+)\s+(\S.*?)\s*$', line)
            if m:
                stmts.setdefault(int(m.group(1)), ('', m.group(2)))
                last = int(m.group(1))
                continue
            m = re.match(r'^[0-9A-F]{6}\s+([0-9A-F]+(?: [0-9A-F]+)*)\s*$', line)
            if m and last in stmts:
                stmts[last] = (stmts[last][0] + m.group(1).replace(' ', ''), stmts[last][1])
    return stmts


def norm(t):
    return re.sub(r'\s+', ' ', t.strip().lower())


def main():
    args = sys.argv[1:]
    if args and args[0] == '--digest':
        # run where the listing is (EBCDIC): oracle_cmp.py --digest ENCODING x.lst > x.tsv
        write_digest(read_hlasm(args[2], args[1]))
        return
    h = read_hlasm(args[0])
    z = read_zosasm(args[1])
    # HLASM statement numbers drift (it numbers more than the source lines),
    # so pair the statements in order and check the source text
    hl = [h[n] for n in sorted(h)]
    zl = [z[n] for n in sorted(z)]
    drop = lambda e: re.match(r'(T\s+CSECT|DS\s+0H|END|PRINT.*)$', e[-1] if len(e) == 2 else e[2], re.I)
    hl = [e for e in hl if not drop(e)]
    zl = [e for e in zl if not drop(e)]
    if len(hl) != len(zl):
        print('statement counts differ: hlasm %d zosasm %d' % (len(hl), len(zl)))
    same = diff = herr = 0
    for (obj, msgs, src), (zo, zsrc) in zip(hl, zl):
        if norm(src) != norm(zsrc):
            print('SOURCE MISMATCH %r / %r' % (src, zsrc))
            break
        errs = [m for m in msgs if re.match(r'ASMA\d+[ESC]', m)]
        if errs:
            herr += 1
            print('hlasm-error  %s   [zosasm %s]  %s' % (src, zo, '; '.join(errs)))
        elif zo != obj and not (obj.startswith(zo) and not obj[len(zo):].strip('0')):
            diff += 1
            print('DIFF  %s   hlasm=%s zosasm=%s %s' % (src, obj, zo, '; '.join(msgs)))
        else:
            same += 1
            if msgs:
                print('warn  %s  %s' % (src, '; '.join(msgs)))
    print('same %d, different %d, hlasm-error %d' % (same, diff, herr))
    sys.exit(1 if diff or herr else 0)


main()
