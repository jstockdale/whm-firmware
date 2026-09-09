#!/usr/bin/env python3
"""whm_oracle_diff - align two serial captures, find first divergence.
Usage: python3 whm_oracle_diff.py one.log two.log
Parses [W] ledger lines and ORACLE confessions; aligns by step;
reports the first step where sx/cam/st disagree, plus all oracle
divergences with their hash pairs. Automates the UTC-alignment
forensics done by hand across the 0.60.x hunts."""
import re, sys
W = re.compile(r"\[(\d\d:\d\d:\d\d\.\d+)\] \[W\] step=(\d+) .*?st=(\w+) .*?sx=([+-][\d.]+) cam=([\d.]+) .*?or=(\d+)")
O = re.compile(r"\[(\d\d:\d\d:\d\d\.\d+)\] walker: ORACLE DIVERGENCE @(\d+) core (\w+) vs (\w+) soft (\w+) vs (\w+)")
def parse(path):
    led, orc = {}, []
    for ln in open(path, errors="replace"):
        m = W.search(ln)
        if m: led[int(m.group(2))] = (m.group(1), m.group(3), float(m.group(4)), float(m.group(5)), int(m.group(6)))
        m = O.search(ln)
        if m: orc.append((m.group(1), int(m.group(2)), m.group(3), m.group(4), m.group(5), m.group(6)))
    return led, orc
def main(a, b):
    la, oa = parse(a); lb, ob = parse(b)
    common = sorted(set(la) & set(lb))
    if not common:
        print("no common ledger steps"); return
    print(f"common ledger steps: {len(common)} ({common[0]}..{common[-1]})")
    first = None
    for s in common:
        ta, sa, xa, ca, _ = la[s]; tb, sb2, xb, cb, _ = lb[s]
        dc = abs(ca - cb); dx = abs(xa - xb)
        if sa != sb2 or dc > 0.15 or dx > 1.0:
            first = (s, ta, tb, sa, sb2, xa, xb, ca, cb, dc, dx); break
    if first:
        s, ta, tb, sa, sb2, xa, xb, ca, cb, dc, dx = first
        print(f"FIRST DIVERGENCE @step {s} ({ta} / {tb}):")
        print(f"  st {sa} vs {sb2} | sx {xa:+.1f} vs {xb:+.1f} (d={dx:.2f}) | cam {ca:.1f} vs {cb:.1f} (d={dc:.2f})")
    else:
        s = common[-1]
        print(f"NO divergence across common range; last step {s}: dcam={abs(la[s][3]-lb[s][3]):.2f}")
    for tag, orc in (("A", oa), ("B", ob)):
        for t, st, c1, c2, s1, s2 in orc:
            print(f"[{tag}] ORACLE @{st} {t} core {c1} vs {c2} soft {s1} vs {s2}")
    print(f"oracle confessions: A={len(oa)} B={len(ob)}")
if __name__ == "__main__":
    if len(sys.argv) != 3: print(__doc__); sys.exit(1)
    main(sys.argv[1], sys.argv[2])
