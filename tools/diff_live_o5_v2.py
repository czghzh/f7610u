#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""diff_live_o5_v2.py -- 用「原厂 O1->O5 状态变化集」把状态噪声剔掉，
只留下「原厂 O5 有、我们没有」的真配置差异。"""
import json, sys

W = "/home/wen/WorkBuddy/zx/f7610u/work/"

o5 = {}
for k, v in json.load(open(W + "o5_regs.json")).items():
    o5[int(k, 16)] = int(v, 16) if isinstance(v, str) else int(v)

o1 = {}
for ln in open(W + "stock_o1.txt"):
    p = ln.split()
    if len(p) == 2:
        try:
            o1[int(p[0], 16)] = int(p[1], 16)
        except ValueError:
            pass

live = {}
for ln in open(W + "live_now.txt"):
    p = ln.split()
    if len(p) == 2:
        idx, val = int(p[0], 16), int(p[1], 16)
        # idx 是 vendor 风格下标：bit16 表示 0x17040000 之后再加
        addr = 0x17000000 + (0x40000 if idx & 0x10000 else 0) + (idx & 0xffff) * 4
        live[addr] = val

# 状态变化集：原厂 O1 与 O5 不同的地址
state = {a for a in o1 if a in o5 and o1[a] != o5[a]}

rows = []
for a in sorted(set(o5) & set(live)):
    if o5[a] == live[a]:
        continue
    if a in state:
        continue
    rows.append((a, o5[a], live[a]))

print("共同地址 %d，O1<->O5 状态变化 %d 个" % (len(set(o5) & set(live)), len(state)))
print("剔除状态噪声后仍不同：%d 个\n" % len(rows))
for a, v5, vl in rows:
    idx = (a - 0x17000000) >> 2
    print("0x%08x idx=0x%05x  原厂=%08x  我们=%08x" % (a, idx, v5, vl))
