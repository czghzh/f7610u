#!/usr/bin/env python3
# extract3.py — boss 阶段反汇编 + 错误消息字符串
import re

WS = "/home/wen/WorkBuddy/zx/f7610u"
lines = open(f"{WS}/work/uboot_full.dis", errors="replace").read().splitlines()
data = open(f"{WS}/backup/mtd1-uboot.bin", "rb").read()
UB = 0x84000000

def dump(a0, a1, title):
    print(f"======== {title} {a0:#x}..{a1:#x} ========")
    for ln in lines:
        m = re.match(r"^\s*([0-9a-f]{8}):", ln)
        if m:
            v = int(m.group(1), 16)
            if a0 <= v < a1:
                print(ln)

dump(0x84008000, 0x84008600, "boss-stage")
print()
print("======== strings 0x8403d3a0..0x8403d6c0 ========")
seg = data[0x3d3a0:0x3d6c0]
curstart = None
for i, c in enumerate(seg):
    if curstart is None and 32 <= c < 127:
        curstart = i
    if curstart is not None and not (32 <= c < 127):
        s = bytes(seg[curstart:i]).decode()
        if len(s) >= 2:
            print(f"  {0x8403d3a0+curstart:#x}: {s!r}")
        curstart = None
print()
print("======== strings 0x8403da60..0x8403dc00 ========")
seg = data[0x3da60:0x3dc00]
curstart = None
for i, c in enumerate(seg):
    if curstart is None and 32 <= c < 127:
        curstart = i
    if curstart is not None and not (32 <= c < 127):
        s = bytes(seg[curstart:i]).decode()
        if len(s) >= 2:
            print(f"  {0x8403da60+curstart:#x}: {s!r}")
        curstart = None
