#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""diff_mac.py —— 对比 now_dump.txt（0x17058000/2048 + 0x17000000/1024）与 O5 快照，
过滤两张 256 项 alloc 表和易变寄存器后输出语义差异。"""
import json

rows = [l.strip() for l in open("/home/wen/WorkBuddy/zx/f7610u/work/now_dump.txt") if l.strip()]
win = [(0x17058000, rows[0]), (0x17000000, rows[1])]
o5 = json.load(open("/home/wen/WorkBuddy/zx/f7610u/work/o5_regs.json"))

SKIP = [
    (0x17058044, 0x170583fc),   # alloc 表1（广播铺开，注册态不同）
    (0x17059204, 0x170595fc),   # alloc 表2
    (0x17058514, 0x1705851c),   # PLOAM 计数/标志
    (0x17058520, 0x1705856c),   # PLOAM 报文窗口
    (0x17058010, 0x17058010),   # irq 状态（易变）
]

def skipped(a):
    return any(lo <= a <= hi for lo, hi in SKIP)

diff = []
for base, line in win:
    vals = [int(x, 16) for x in line.split(",")]
    for i, v in enumerate(vals):
        a = base + i * 4
        k = "0x%08x" % a
        if k not in o5 or skipped(a):
            continue
        o = o5[k]
        if o != v:
            diff.append((a, v, o))

print("语义差异:", len(diff))
for a, v, o in diff:
    print("0x%08x  ours=%08x  o5=%08x  xor=%08x" % (a, v, o, v ^ o))
