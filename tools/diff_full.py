#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""diff_full.py —— 我们固件 vs 原厂 O5，整个 0x17000000 窗口 6144 个寄存器。

重点看「我们为 0、原厂非 0」的寄存器：那多半是原厂写了而我们没写的配置。
"""
import json

rows = [l.strip() for l in open("/home/wen/WorkBuddy/zx/f7610u/work/our_full.txt")
        if l.strip() and not l.startswith("Warning")]
vals = [int(x, 16) for x in rows[0].split(",")]
our = {0x17000000 + i * 4: v for i, v in enumerate(vals)}

o5 = json.load(open("/home/wen/WorkBuddy/zx/f7610u/work/o5_regs.json"))
ref = {}
for k, v in o5.items():
    ref[int(k, 16)] = int(v, 16) if isinstance(v, str) else int(v)

print("ours=%d ref=%d" % (len(our), len(ref)))

zero_to_val = []
both_diff = []
val_to_zero = []
for a in sorted(ref):
    if a not in our:
        continue
    o, r = our[a], ref[a]
    if o == r:
        continue
    if o == 0 and r != 0:
        zero_to_val.append((a, o, r))
    elif o != 0 and r == 0:
        val_to_zero.append((a, o, r))
    else:
        both_diff.append((a, o, r))

print("\n=== 我们=0 原厂!=0  (%d) ===" % len(zero_to_val))
for a, o, r in zero_to_val:
    print("0x%08x  ours=%08x  O5=%08x" % (a, o, r))

print("\n=== 我们!=0 原厂=0  (%d) ===" % len(val_to_zero))
for a, o, r in val_to_zero[:40]:
    print("0x%08x  ours=%08x  O5=%08x" % (a, o, r))

print("\n=== 两边都非0但不同  (%d) ===" % len(both_diff))
for a, o, r in both_diff[:60]:
    print("0x%08x  ours=%08x  O5=%08x  xor=%08x" % (a, o, r, o ^ r))
