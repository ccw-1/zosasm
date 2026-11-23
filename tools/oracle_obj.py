#!/usr/bin/env python3
"""oracle_obj.py HLASM.dump ZOSASM.dump - compare two `zosasm -dump` listings
structurally: ESD records (by type, name and parent name), text per ED and
RLDs by name. Known differences are ignored: the ED B_IDRL, empty B_PRV, the
ZSYM classes C_ADATA*, ED offsets (the text and RLD offsets are taken
relative to the ED), HDR and END counts. Prints one line per difference and
exits 1 when there are any."""
import re, sys


def parse(path):
    esd, txt, rld, other = {}, {}, [], []
    for line in open(path, errors="replace"):
        line = line.rstrip("\n")
        if line.startswith("ESD"):
            m = re.match(r'ESD (\d+)\s+id=(\d+) parent=(\d+) off=(-?\d+) len=(\d+) (.*) "(.*)"$', line)
            if not m:
                other.append(line)
                continue
            typ, i, par, off, ln, rest, name = m.groups()
            attrs = dict(a.split("=", 1) for a in rest.split())
            esd[int(i)] = dict(typ=int(typ), id=int(i), par=int(par), off=int(off),
                               len=int(ln), name=name, attrs=attrs)
        elif line.startswith("TXT"):
            m = re.match(r"TXT esd=(\d+) off=(\d+) len=(\d+)\s*(.*)", line)
            e, off, ln, hx = m.groups()
            txt.setdefault(int(e), []).append((int(off), bytes(int(x, 16) for x in hx.split())))
        elif line.startswith("RLD"):
            m = re.match(r"RLD r=(\d+) p=(\d+) off=(\d+) (.*)", line)
            rld.append((int(m.group(1)), int(m.group(2)), int(m.group(3)), m.group(4)))
        elif line.startswith("END") or line.startswith("HDR"):
            other.append(line)
    return esd, txt, rld, other


def label(esd, i):
    e = esd.get(i)
    if not e:
        return "esd%d" % i
    t = {0: "SD", 1: "ED", 2: "LD", 3: "PR", 4: "ER"}.get(e["typ"], str(e["typ"]))
    p = esd.get(e["par"])
    if not p:
        return "%s:%s" % (t, e["name"])
    return "%s:%s@%s" % (t, e["name"], label(esd, e["par"]))


def ignored(e):
    return e["name"] in ("B_IDRL",) or e["name"].startswith("C_ADATA")


def norm(path):
    esd, txt, rld, _ = parse(path)
    out = {"esd": {}, "txt": {}, "rld": []}
    for i, e in esd.items():
        if ignored(e):
            continue
        e = dict(e)
        p = esd.get(e["par"])
        if e["typ"] in (2, 3) and p and p["typ"] == 1:
            e["off"] -= p["off"]
        out["esd"][label(esd, i)] = e
    for i, parts in txt.items():
        e = esd.get(i)
        if not e or ignored(e):
            continue
        n = e["len"]
        buf = bytearray(max(n, max(o - e["off"] + len(b) for o, b in parts)))
        for o, b in parts:
            buf[o - e["off"]:o - e["off"] + len(b)] = b
        for r, p, o, rest in rld:  # HLASM stores the ED offset of the referent in the text
            re_ = esd.get(r)
            m = re.search(r"len=(\d+)", rest)
            if p == i and re_ and re_["typ"] == 1 and re_["off"] and m and "store=false" in rest:
                n_ = int(m.group(1))
                q = o - e["off"]
                v = int.from_bytes(buf[q:q + n_], "big") - re_["off"]
                buf[q:q + n_] = (v & ((1 << (8 * n_)) - 1)).to_bytes(n_, "big")
        for r, p, o, rest in rld:  # relative immediates: HLASM adds the ED offsets, the binder accepts both
            m = re.search(r"ref=(\d+).*len=(\d+)", rest)
            if p == i and m and m.group(1) == "6":
                q = o - e["off"]
                buf[q:q + int(m.group(2))] = bytes(int(m.group(2)))
        out["txt"][label(esd, i)] = bytes(buf)
    for r, p, o, rest in rld:
        pe = esd.get(p)
        if not pe or ignored(pe) or (esd.get(r) and ignored(esd[r])):
            continue
        out["rld"].append((label(esd, p), o - pe["off"], label(esd, r), rest))
    out["rld"].sort()
    return out


def main():
    a, b = norm(sys.argv[1]), norm(sys.argv[2])
    nd = 0
    if not a["esd"] and not a["txt"]:
        print("oracle_obj: no ESD or TXT records parsed from %s; -dump format may have drifted" % sys.argv[1])
        sys.exit(1)
    if not b["esd"] and not b["txt"]:
        print("oracle_obj: no ESD or TXT records parsed from %s; -dump format may have drifted" % sys.argv[2])
        sys.exit(1)
    # empty B_PRV is a HLASM habit
    for k in list(a["esd"]):
        if k.startswith("ED:B_PRV") and a["esd"][k]["len"] == 0 and k not in b["esd"]:
            del a["esd"][k]
    for k in sorted(set(a["esd"]) | set(b["esd"])):
        x, y = a["esd"].get(k), b["esd"].get(k)
        if not x or not y:
            print("ESD %s only in %s" % (k, "zosasm" if y else "HLASM"))
            nd += 1
            continue
        for f in ("len", "off"):
            if x[f] != y[f] and not (f == "off" and x["typ"] == 1):
                print("ESD %s %s: HLASM %s zosasm %s" % (k, f, x[f], y[f]))
                nd += 1
        for f in sorted(set(x["attrs"]) | set(y["attrs"])):
            if x["attrs"].get(f) != y["attrs"].get(f):
                print("ESD %s %s: HLASM %s zosasm %s" % (k, f, x["attrs"].get(f), y["attrs"].get(f)))
                nd += 1
    for k in sorted(set(a["txt"]) | set(b["txt"])):
        x, y = a["txt"].get(k), b["txt"].get(k)
        if x is None or y is None:
            print("TXT %s only in %s" % (k, "zosasm" if y is not None else "HLASM"))
            nd += 1
        elif x.rstrip(b"\0") != y.rstrip(b"\0"):
            i = next((j for j in range(min(len(x), len(y))) if x[j] != y[j]), min(len(x), len(y)))
            print("TXT %s differs at %d: HLASM %s zosasm %s" % (k, i, x[i:i + 8].hex(), y[i:i + 8].hex()))
            nd += 1
    ra, rb = a["rld"], b["rld"]
    for r in ra:
        if r not in rb:
            print("RLD only in HLASM: %s +%d -> %s %s" % r)
            nd += 1
    for r in rb:
        if r not in ra:
            print("RLD only in zosasm: %s +%d -> %s %s" % r)
            nd += 1
    print("%d difference(s)" % nd)
    sys.exit(1 if nd else 0)


main()
