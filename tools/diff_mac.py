#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""diff_mac.py —— 对比 /tmp/now_dump.txt（两行: 0x17058000/2048 + 0x17000000/1024）
与 work/o5_regs.json 的 O5 活体快照。仅打印差异。"""
import json

rows = [l.strip() for l in open("/home/wen/WorkBuddy/zx/f7610u/work/now_dump.txt") if l.strip()]
win = [(0x17058000, rows[0]), (0x17000000, rows[1])]
o5 = json.load(open("/home/wen/WorkBuddy/zx/f7610u/work/o5_regs.json"))

diff = []
for base, line in win:
    vals = [int(x, 16) for x in line.split(",")]
    for i, v in enumerate(vals):
        a = base + i * 4
        k = "0x%08x" % a
        if k not in o5:
            continue
        o = o5[k]
        if o != v:
            diff.append((a, v, o))

print("差异总数:", len(diff))
# 分组：0x1705 与 0x1700
for a, v, o in diff:
    print("0x%08x  ours=%08x  o5=%08x  xor=%08x" % (a, v, o, v ^ o))
