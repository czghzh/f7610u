#!/usr/bin/env python3
"""Extract a function's instructions from zte_xgpon.dis, optionally filtered."""
import re
import sys

DIS = "/home/wen/WorkBuddy/zx/f7610u/work/zte_xgpon.dis"


def main():
    name = sys.argv[1]
    mode = sys.argv[2] if len(sys.argv) > 2 else "all"
    lines = open(DIS).read().splitlines()
    i = None
    for k, l in enumerate(lines):
        m = re.match(r"^[0-9a-f]+ <([^>]+)>:", l)
        if m and m.group(1) == name:
            i = k
            break
    if i is None:
        print("MISS", name)
        return
    j = i + 1
    while j < len(lines) and not re.match(r"^[0-9a-f]+ <[^>]+>:", lines[j]):
        j += 1
    print("len", j - i)
    pat = re.compile(r"\b(mov|movk|movz|bl|orr|and|bfi|bfxil|str|ldr|add|sub)\b")
    for k in range(i, j):
        if mode == "all" or pat.search(lines[k]):
            print(lines[k])


main()
