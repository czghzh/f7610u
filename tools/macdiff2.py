#!/usr/bin/env python3
"""Diff the vendor's MAC windows 3/4/6 against ours."""
import re


def load(path):
    regs = {}
    for line in open(path, errors="replace"):
        for m in re.finditer(r"0x([0-9a-fA-F]{8})\s*=\s*0x([0-9a-fA-F]{1,8})", line):
            regs[int(m.group(1), 16)] = int(m.group(2), 16)
    return regs


v = load("/home/wen/WorkBuddy/zx/f7610u/work/vendor_mac_win36.txt")
o = load("/home/wen/WorkBuddy/zx/f7610u/work/ours_mac_win36.txt")
print("vendor %d regs, ours %d regs" % (len(v), len(o)))

diff = [(a, v[a], o[a]) for a in sorted(v) if a in o and v[a] != o[a]]
print("不同 %d 个 / 共同 %d 个\n" % (len(diff), len([a for a in v if a in o])))

for a, vv, oo in diff:
    print("  %#010x  vendor=%#010x  ours=%#010x" % (a, vv, oo))
