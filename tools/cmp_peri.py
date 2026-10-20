#!/usr/bin/env python3
# 对比 ours_pon_peri.txt / stock_crm.txt（SYSCTRL 0x10e00000、CRM 0x10e10000，各 64 字）
import sys

def load(path, hexaddr=True):
    d = {}
    for ln in open(path):
        p = ln.split()
        if len(p) != 2 or p[0].startswith("##"):
            continue
        try:
            a = int(p[0], 16)
            v = int(p[1], 16)
        except ValueError:
            continue
        d[a] = v
    return d

ours = load(sys.argv[1])
stock = load(sys.argv[2])
print("ours=%d stock=%d" % (len(ours), len(stock)))
for name, lo, hi in [("SYSCTRL", 0x10e00000, 0x10e00100),
                     ("CRM", 0x10e10000, 0x10e10100)]:
    rows = []
    for a in range(lo, hi, 4):
        o, s = ours.get(a), stock.get(a)
        if o is None or s is None:
            continue
        if o != s:
            rows.append((a, o, s))
    print("\n=== %s  diff %d/%d ===" % (name, len(rows), (hi - lo) // 4))
    for a, o, s in rows:
        tag = ""
        if s and not o:
            tag = "  <-- 原厂有我们没有"
        elif o and not s:
            tag = "  <-- 我们多置了"
        print("  %08x  ours=%08x  stock=%08x%s" % (a, o, s, tag))
