#!/usr/bin/env python3
# extract5.py — 验证函数调用者 + main_loop 候选区 + 关键检查
import re

WS = "/home/wen/WorkBuddy/zx/f7610u"
lines = open(f"{WS}/work/uboot_full.dis", errors="replace").read().splitlines()
data = open(f"{WS}/backup/mtd1-uboot.bin", "rb").read()
UB = 0x84000000

def line_of(addr):
    for i, ln in enumerate(lines):
        m = re.match(r"^\s*([0-9a-f]{8}):", ln)
        if m and int(m.group(1), 16) == addr:
            return i
    return None

print("== callers of 0x84007310 (verify) ==")
for i, ln in enumerate(lines):
    if re.search(r"bl\s+0x84007310\b", ln):
        print(" ", ln.strip())
print()
print("== callers of 0x84006fe0 (others parse) ==")
for i, ln in enumerate(lines):
    if re.search(r"bl\s+0x84006fe0\b", ln):
        print(" ", ln.strip())
print()
print("== callers of 0x84007290 ==")
for i, ln in enumerate(lines):
    if re.search(r"bl\s+0x84007290\b", ln):
        print(" ", ln.strip())
print()
print("== refs to default-env 0x840a0000 area ==")
for i, ln in enumerate(lines):
    if "0x840a0000" in ln or "0x840a1000" in ln:
        print(" ", ln.strip())
print()
print("== search 'search->result' in binary ==")
for t in [b"search->result", b"search->", b"->result", b"entry=%x", b"offset=%x"]:
    offs = []
    s = 0
    while True:
        k = data.find(t, s)
        if k < 0:
            break
        offs.append(k)
        s = k + 1
    print(f"  {t!r}: {[hex(o) for o in offs[:10]]}")
print()
print("== prologue candidates 0x84009000-0x8400a600 ==")
for ln in lines:
    m = re.match(r"^\s*([0-9a-f]{8}):\s+[0-9a-f]{8}\s+(stp\s+x29|sub\s+sp)", ln)
    if m:
        v = int(m.group(1), 16)
        if 0x84009000 <= v < 0x8400a600:
            print(" ", ln.strip())
print()
print("== jump table @0x475ac ==")
tbl = data[0x475ac:0x475ac + 16]
print("bytes:", tbl.hex())
for k in range(8):
    off = int.from_bytes(tbl[k * 2:k * 2 + 2], "little", signed=True)
    print(f"  state={k+1}: {off} -> {0x8400840c + off*4:#x}")
print()
print("== 文件 0x8d40 处字节 ==")
print(data[0x8d40:0x8d50].hex())
print()
print("== 0x8403e900/0x8403e908/0x8403e180 周边精确字节 ==")
for a in [0x8403e8f8, 0x8403e178]:
    seg = data[a - UB:a - UB + 8]
    print(f"  {a:#x}: {seg.hex()}  {seg}")
