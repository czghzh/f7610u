#!/usr/bin/env python3
"""List Hal_Read/WriteXmacReg call sites with decoded XMAC index.

The vendor code rebuilds w0 as `mov w0,#low` + `movk w0,#high,lsl#16`
immediately before each call, so we look back a few instructions.
Physical address = 0x17000000 + idx*4.
"""
import re
import sys
from collections import Counter

DIS = "/home/wen/WorkBuddy/zx/f7610u/work/zte_xgpon.dis"

MOV = re.compile(r"\bmov\s+w0, #(0x[0-9a-f]+)")
MOVK = re.compile(r"\bmovk\s+w0, #(0x[0-9a-f]+), lsl #16")


def main():
    lines = open(DIS).read().splitlines()
    cur = "?"
    calls = []
    for i, l in enumerate(lines):
        m = re.match(r"^[0-9a-f]+ <([^>]+)>:", l)
        if m:
            cur = m.group(1)
            continue
        if "Hal_WriteXmacReg" in l or "Hal_ReadXmacReg" in l:
            lo = hi = None
            for k in range(max(0, i - 8), i):
                mm = MOV.search(lines[k])
                if mm:
                    lo = int(mm.group(1), 16)
                mm = MOVK.search(lines[k])
                if mm:
                    hi = int(mm.group(1), 16)
            if lo is None:
                continue
            idx = lo | ((hi or 0) << 16)
            kind = "W" if "Write" in l else "R"
            calls.append((cur, kind, 0x17000000 + idx * 4))

    mode = sys.argv[1] if len(sys.argv) > 1 else "writes"
    if mode == "writes":
        c = Counter(a for (f, k, a) in calls if k == "W")
        fns = {}
        for f, k, a in calls:
            if k == "W":
                fns.setdefault(a, set()).add(f)
        print("distinct XMAC write addresses:", len(c))
        for a in sorted(c):
            print("%08x x%-3d %s" % (a, c[a], ",".join(sorted(fns[a]))[:100]))
    elif mode == "fn":
        name = sys.argv[2]
        for f, k, a in calls:
            if name in f:
                print("%08x %s %s" % (a, k, f))


main()
