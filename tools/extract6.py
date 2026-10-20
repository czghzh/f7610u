#!/usr/bin/env python3
# extract6.py — 库函数体 + search->result/bootargs模板引用 + parse调用点上下文
import re, struct

WS = "/home/wen/WorkBuddy/zx/f7610u"
lines = open(f"{WS}/work/uboot_full.dis", errors="replace").read().splitlines()
data = open(f"{WS}/backup/mtd1-uboot.bin", "rb").read()
UB = 0x84000000

def dump(a0, a1, title, maxln=100000):
    print(f"======== {title} {a0:#x}..{a1:#x} ========")
    n = 0
    for ln in lines:
        m = re.match(r"^\s*([0-9a-f]{8}):", ln)
        if m:
            v = int(m.group(1), 16)
            if a0 <= v < a1 and n < maxln:
                print(ln); n += 1

dump(0x84033b50, 0x84033c40, "func 0x84033b50")
dump(0x840325c0, 0x84032660, "func 0x840325c0")
print()
print("== refs: #0xf58 (search->result) ==")
for ln in lines:
    if re.search(r"add\s+\w+,\s*\w+,\s*#0xf58\b", ln):
        print(" ", ln.strip())
print("== refs: #0x1ef (bootargs tmpl) ==")
for ln in lines:
    if re.search(r"add\s+\w+,\s*\w+,\s*#0x1ef\b", ln):
        print(" ", ln.strip())
print()
print("== 字符串: 0x8403df40-0x8403dfc0 ==")
seg = data[0x3df40:0x3dfc0]
curstart = None
for i, c in enumerate(seg):
    if curstart is None and 32 <= c < 127: curstart = i
    if curstart is not None and not (32 <= c < 127):
        s = bytes(seg[curstart:i]).decode()
        if len(s) >= 2: print(f"  {0x8403df40+curstart:#x}: {s!r}")
        curstart = None
print()
print("== 字符串: 0x8403db60-0x8403dbc0 raw ==")
for i in range(0x8403db60, 0x8403dbc0, 16):
    chunk = data[i-UB:i-UB+16]
    print(f"  {i:#x}: {chunk.hex()}  |" + "".join(chr(c) if 32 <= c < 127 else "." for c in chunk) + "|")
print()
print("== 字符串: 0x8403e1e0-0x8403e300 ==")
seg = data[0x3e1e0:0x3e300]
curstart = None
for i, c in enumerate(seg):
    if curstart is None and 32 <= c < 127: curstart = i
    if curstart is not None and not (32 <= c < 127):
        s = bytes(seg[curstart:i]).decode()
        if len(s) >= 2: print(f"  {0x8403e1e0+curstart:#x}: {s!r}")
        curstart = None
print()
print("== 指针扫描 0x84007310 / 0x84007100-0x84007200 ==")
for a in [0x84007310, 0x84007150, 0x84007120, 0x84007290, 0x840072d0, 0x84007000]:
    pat = struct.pack("<Q", a)
    hits = []
    s = 0
    while True:
        k = data.find(pat, s)
        if k < 0: break
        hits.append(k); s = k + 1
    print(f"  {a:#x}: {[hex(h) for h in hits]}")
print()
print("== 0x84008600-0x84008a00 (parse caller#1 上下文) ==")
dump(0x84008600, 0x84008a00, "ctx")
print()
print("== 0x84007000-0x84007220 prologues ==")
for ln in lines:
    m = re.match(r"^\s*([0-9a-f]{8}):\s+[0-9a-f]{8}\s+(stp\s+x29|sub\s+sp)", ln)
    if m:
        v = int(m.group(1), 16)
        if 0x84006c00 <= v < 0x84007400:
            print(" ", ln.strip())
