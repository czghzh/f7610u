#!/usr/bin/env python3
# 对比 ours_crm.txt / stock_crm.txt（两块 PON 外围：SYSCTRL 0x10e00000、CRM 0x10e10000）
import sys

def load_ours(path):
    d = {}
    for ln in open(path):
        p = ln.split()
        if len(p) != 2 or not p[0].startswith("0x"):
            continue
        try:
            a = int(p[0][2:], 10)      # "0x" + 十进制
            v = int(p[1], 16)
        except ValueError:
            continue
        d[a] = v
    return d

def load_stock(path):
    d = {}
    for ln in open(path):
        p = ln.split()
        if len(p) != 2 or p[0] == "got":
            continue
        try:
            a = int(p[0], 16)
            v = int(p[1], 16)
        except ValueError:
            continue
        d[a] = v
    return d

ours = load_ours(sys.argv[1])
stock = load_stock(sys.argv[2])
print("ours=%d stock=%d" % (len(ours), len(stock)))
for name, lo, hi in [("SYSCTRL", 0x10e00000, 0x10e00100),
                     ("CRM", 0x10e10000, 0x10e10100)]:
    n = 0
    rows = []
    for a in range(lo, hi, 4):
        o, s = ours.get(a), stock.get(a)
        if o is None or s is None:
            continue
        if o != s:
            rows.append((a, o, s))
            n += 1
    print("\n=== %s  diff %d/%d ===" % (name, n, (hi - lo) // 4))
    for a, o, s in rows:
        print("  %08x  ours=%08x  stock=%08x" % (a, o, s))
