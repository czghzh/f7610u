#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""全量 6144 对比：原厂 O5 vs 我们现场（0x17000000-0x172007fc）"""
import json, collections

W = "/home/wen/WorkBuddy/zx/f7610u/work/"
o5 = {int(k, 16): (int(v, 16) if isinstance(v, str) else int(v))
      for k, v in json.load(open(W + "o5_regs.json")).items()}

live = {}
for ln in open(W + "live_now.txt"):
    p = ln.split()
    if len(p) == 2:
        idx, val = int(p[0], 16), int(p[1], 16)
        live[0x17000000 + (0x40000 if idx & 0x10000 else 0) + (idx & 0xffff) * 4] = val
for ln in open(W + "live_out.txt"):
    p = ln.split()
    if len(p) == 2:
        live[int(p[0], 16)] = int(p[1], 16)

diff = [(a, o5[a], live[a]) for a in sorted(o5) if a in live and o5[a] != live[a]]
print("快照 %d，现场命中 %d，不同 %d 个" % (len(o5), len(set(o5) & set(live)), len(diff)))

buckets = collections.Counter((a >> 12) << 12 for a, _, _ in diff)
print("\n按 4KB 块分布：")
for base, n in sorted(buckets.items()):
    print("  0x%08x  %3d" % (base, n))

print("\n明细：")
for a, v5, vl in diff:
    print("0x%08x  原厂=%08x  我们=%08x" % (a, v5, vl))
