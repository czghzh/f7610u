#!/usr/bin/env python3
# extract2.py — 错误分支/阶段函数反汇编 + 精确字符串表
import re

WS = "/home/wen/WorkBuddy/zx/f7610u"
lines = open(f"{WS}/work/uboot_full.dis", errors="replace").read().splitlines()

def dump(a0, a1, title):
    print(f"======== {title} {a0:#x}..{a1:#x} ========")
    for ln in lines:
        m = re.match(r"^\s*([0-9a-f]{8}):", ln)
        if m:
            v = int(m.group(1), 16)
            if a0 <= v < a1:
                print(ln)

dump(0x840079c0, 0x84007c00, "verify-error-branches")
dump(0x84007c00, 0x84008000, "mid")
