#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""diff_ours_o1.py —— 我们固件 vs 原厂 O1 的逐寄存器差异，并生成对齐写入命令。

数据源：
  work/our_768.txt    /tmp/zxreg d 的两行 CSV（0x17059400 x256、0x17059800 x512）
  work/stock_o1.txt   原厂 O1 快照（addr value 文本）
"""
import sys

our = {}
rows = [l.strip() for l in open("/home/wen/WorkBuddy/zx/f7610u/work/our_768.txt")
        if "," in l and not l.startswith("Warning")]
for i, v in enumerate(rows[0].split(",")):
    our[0x17059400 + i * 4] = int(v, 16)
for i, v in enumerate(rows[1].split(",")):
    our[0x17059800 + i * 4] = int(v, 16)

o1 = {}
for ln in open("/home/wen/WorkBuddy/zx/f7610u/work/stock_o1.txt"):
    p = ln.split()
    if len(p) == 2:
        o1[int(p[0], 16)] = int(p[1], 16)

diffs = [(a, o1[a], our.get(a)) for a in sorted(o1)
         if a in our and o1[a] != our[a]]

print("# ours regs=%d  o1 regs=%d  diffs=%d" % (len(our), len(o1), len(diffs)))
for a, v1, vo in diffs:
    print("0x%08x  ours=%08x  O1=%08x  xor=%08x" % (a, vo, v1, v1 ^ vo))

if len(sys.argv) > 1 and sys.argv[1] == "cmds":
    with open("/home/wen/WorkBuddy/zx/f7610u/work/align_o1.txt", "w") as f:
        for a, v1, vo in diffs:
            f.write("0x%08x 0x%08x\n" % (a, v1))
    print("# wrote work/align_o1.txt (%d cmds)" % len(diffs))
