#!/usr/bin/env python3
# extract8.py — 终极字节级验证：ref 目标 vs 字符串实际位置
import re

WS = "/home/wen/WorkBuddy/zx/f7610u"
data = open(f"{WS}/backup/mtd1-uboot.bin", "rb").read()
lines = open(f"{WS}/work/uboot_full.dis", errors="replace").read().splitlines()

def hexat(a, n):
    return data[a:a+n].hex()

print("== file 0x8c9c-0x8cb0 (select caller) ==")
print(hexat(0x8c9c, 0x14))
print("== file 0x875c-0x8770 ==")
print(hexat(0x875c, 0x14))
print("== file 0x8ca4-0x8cb8 ==")
print(hexat(0x8ca4, 0x14))
print()
print("== 关键 key 串查找 ==")
for t in [b"BootImageNum", b"DownloadState", b"LastBooting", b"O7State",
          b"Image1Status", b"Image0Status", b"USERNAME", b"USERPASSWD",
          b"erase other failed", b"write other failed", b"invalid tmp"]:
    offs = []
    s = 0
    while True:
        k = data.find(t, s)
        if k < 0: break
        offs.append(k); s = k + 1
    print(f"  {t!r}: {[hex(o) for o in offs]}")
print()
print("== file 0x3db78 / 0x3eb78 / 0x3e378 raw ==")
for a in [0x3db78, 0x3eb78, 0x3e378]:
    print(f"  {a:#x}: {hexat(a, 24)}  |" + "".join(chr(c) if 32 <= c < 127 else "." for c in data[a:a+24]) + "|")
print()
print("== dis 里所有 'add.*#0x378' ==")
for ln in lines:
    if re.search(r"add\s+\w+,\s*\w+,\s*#0x378\b", ln):
        print(" ", ln.strip())
print("== dis 里所有 'add.*#0xb78' ==")
for ln in lines:
    if re.search(r"add\s+\w+,\s*\w+,\s*#0xb78\b", ln):
        print(" ", ln.strip())
print()
print("== parser 0x84006fe0 函数体 ==")
for ln in lines:
    m = re.match(r"^\s*([0-9a-f]{8}):", ln)
    if m:
        v = int(m.group(1), 16)
        if 0x84006fe0 <= v < 0x84007010:
            print(ln)
