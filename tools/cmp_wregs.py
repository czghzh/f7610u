#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""只比「厂商驱动会写」的寄存器：原厂 O5 vs 我们现场。"""
import json

W = "/home/wen/WorkBuddy/zx/f7610u/work/"

o5 = {}
for k, v in json.load(open(W + "o5_regs.json")).items():
    o5[int(k, 16)] = int(v, 16) if isinstance(v, str) else int(v)

live = {}
for ln in open(W + "live_now.txt"):
    p = ln.split()
    if len(p) == 2:
        idx, val = int(p[0], 16), int(p[1], 16)
        addr = 0x17000000 + (0x40000 if idx & 0x10000 else 0) + (idx & 0xffff) * 4
        live[addr] = val

# xmap_xgpon.txt 里标了 W 的地址
want = []
for ln in open(W + "xmap_xgpon.txt"):
    p = ln.split()
    if len(p) >= 4 and p[0].startswith("17") and p[2].startswith("W"):
        want.append(int(p[0], 16))

print("%-10s %-10s %-10s %s" % ("addr", "vendor-O5", "ours", "same?"))
for a in want:
    v5 = o5.get(a)
    vl = live.get(a)
    if v5 is None:
        continue
    flag = "" if v5 == vl else "  <<<"
    print("0x%08x %-10s %-10s%s" % (a,
          ("%08x" % v5) if v5 is not None else "-",
          ("%08x" % vl) if vl is not None else "-", flag))
