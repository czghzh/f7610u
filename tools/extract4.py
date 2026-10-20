#!/usr/bin/env python3
# extract4.py — boss 区代码→字符串引用全映射 + 关键串导出 + 跳表解码
import re, struct

WS = "/home/wen/WorkBuddy/zx/f7610u"
lines = open(f"{WS}/work/uboot_full.dis", errors="replace").read().splitlines()
data = open(f"{WS}/backup/mtd1-uboot.bin", "rb").read()
UB = 0x84000000

def cur_str(addr, maxlen=100):
    if not (0 <= addr - UB < len(data)):
        return "??"
    i = addr - UB
    j = data.find(b"\x00", i)
    if j < 0:
        j = i + maxlen
    raw = data[i:min(j, i + maxlen)]
    return "".join(chr(c) if 32 <= c < 127 else "." for c in raw)

adrp_re = re.compile(r"^\s*([0-9a-f]{8}):\s+[0-9a-f]{8}\s+adrp\s+(\w+),\s*(0x[0-9a-f]+)")
add_re = re.compile(r"^\s*([0-9a-f]{8}):\s+[0-9a-f]{8}\s+add\s+(\w+),\s*(\w+),\s*#(0x[0-9a-fA-F]+|\d+)(?:,\s*lsl\s*#(\d+))?")

adrps = []
for i, ln in enumerate(lines):
    m = adrp_re.match(ln)
    if m:
        adrps.append((i, m.group(2), int(m.group(3), 16)))

refs = []
for i, reg, base in adrps:
    got = None
    for j in range(i + 1, min(i + 13, len(lines))):
        ln = lines[j]
        if "adrp" in ln:
            m2 = adrp_re.match(ln)
            if m2 and m2.group(2) == reg:
                break
        m = add_re.match(ln)
        if not m:
            continue
        pc = int(m.group(1), 16)
        rd, rn = m.group(2), m.group(3)
        imm = int(m.group(4), 0)
        if m.group(5):
            imm <<= int(m.group(5))
        if rn == reg:
            got = (pc, base + imm)
            break
    if got and 0x8403c000 <= got[1] < 0x8403f000 and 0x84006c00 <= got[0] < 0x84008700:
        refs.append(got)

print("== boss 区 (0x84006c00-0x84008700) 字符串引用 ==")
for pc, tgt in sorted(refs):
    print(f"{pc:#010x} -> {tgt:#010x}  {cur_str(tgt)!r}")

print()
print("== 关键字符串区 0x8403e120-0x8403e200 ==")
seg = data[0x3e120:0x3e200]
curstart = None
for i, c in enumerate(seg):
    if curstart is None and 32 <= c < 127:
        curstart = i
    if curstart is not None and not (32 <= c < 127):
        s = bytes(seg[curstart:i]).decode()
        if len(s) >= 2:
            print(f"  {0x8403e120+curstart:#x}: {s!r}")
        curstart = None
print()
print("== 关键字符串区 0x8403e8c0-0x8403eb80 ==")
seg = data[0x3e8c0:0x3eb80]
curstart = None
for i, c in enumerate(seg):
    if curstart is None and 32 <= c < 127:
        curstart = i
    if curstart is not None and not (32 <= c < 127):
        s = bytes(seg[curstart:i]).decode()
        if len(s) >= 2:
            print(f"  {0x8403e8c0+curstart:#x}: {s!r}")
        curstart = None
print()
print("== 跳表 @0x8404705ac ==")
tbl = data[0x4705ac:0x4705ac + 16]
print("bytes:", tbl.hex())
for k in range(8):
    off = struct.unpack_from("<h", tbl, k * 2)[0]
    print(f"  state={k+1}: offset={off} -> target={0x8400840c + off*4:#x}")
print()
print("== 文件 0x8d40 处字节 ==")
print(data[0x8d40:0x8d50].hex())
