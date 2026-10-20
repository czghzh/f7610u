#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""diff_o1_o5.py —— 原厂「O1（注册前）」vs「O5（注册后）」逐寄存器差异。

数据源：
  work/stock_o1.txt   O1 状态，768 个寄存器（0x17059400-0x170594ff、0x17059800-0x170599ff）
  work/o5_regs.json   O5 状态，6144 个寄存器（values 是 "0x..." 字符串）
"""
import json

o1 = {}
for ln in open("/home/wen/WorkBuddy/zx/f7610u/work/stock_o1.txt"):
    p = ln.split()
    if len(p) == 2:
        o1[int(p[0], 16)] = int(p[1], 16)

o5 = json.load(open("/home/wen/WorkBuddy/zx/f7610u/work/o5_regs.json"))


def get5(addr):
    v = o5.get("0x%08x" % addr)
    if v is None:
        v = o5.get("0x%x" % addr)
    if v is None:
        return None
    return int(v, 16) if isinstance(v, str) else int(v)


diffs = []
missing = 0
for a in sorted(o1):
    v5 = get5(a)
    if v5 is None:
        missing += 1
        continue
    if o1[a] != v5:
        diffs.append((a, o1[a], v5))

print("O1 regs=%d  missing_in_o5=%d  diffs=%d" % (len(o1), missing, len(diffs)))
print()
for a, v1, v5 in diffs:
    print("0x%08x  O1=%08x  O5=%08x  xor=%08x" % (a, v1, v5, v1 ^ v5))
