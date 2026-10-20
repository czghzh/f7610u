#!/usr/bin/env python3
"""Compare vendor (working PON MAC) register snapshots against ours."""
import re, sys


def load(path):
    regs = {}
    for line in open(path, errors="replace"):
        for m in re.finditer(r"0x([0-9a-fA-F]{8})\s*=\s*0x([0-9a-fA-F]{1,8})", line):
            regs[int(m.group(1), 16)] = int(m.group(2), 16)
    return regs


v = load("/tmp/vendor_mac.txt")
o = load("/tmp/ours_mac.txt")

print("vendor regs: %d   ours regs: %d" % (len(v), len(o)))


def window(addr):
    if addr < 0x17100000:
        return "0x17000000"
    if addr < 0x17200000:
        return "0x171C0000"
    return "0x17200000"


diff = [a for a in sorted(v) if a in o and v[a] != o[a]]
same = [a for a in sorted(v) if a in o and v[a] == o[a]]
print("相同 %d 个，不同 %d 个\n" % (len(same), len(diff)))

for w in ("0x17000000", "0x171C0000", "0x17200000"):
    rows = [a for a in diff if window(a) == w]
    print("=" * 62)
    print("窗口 %s  不同 %d 个" % (w, len(rows)))
    print("=" * 62)
    for a in rows:
        print("  %#010x  vendor=%#010x  ours=%#010x" % (a, v[a], o[a]))
    print()
