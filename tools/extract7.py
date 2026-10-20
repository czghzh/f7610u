#!/usr/bin/env python3
# extract7.py — 双容器布局验证：字符串出现次数 + 容器头 hexdump
import re

WS = "/home/wen/WorkBuddy/zx/f7610u"
data = open(f"{WS}/backup/mtd1-uboot.bin", "rb").read()
UB = 0x84000000

print(f"file size: {len(data):#x}")
print()
print("== 关键字符串出现次数/位置 ==")
for t in [b"main_loop entered", b"V1.0.1P1N6", b"Hit any key", b"RSA Verify OK",
          b"verify vmlinuz success", b"zboot info init done", b"bootcmd",
          b"saveenv", b"search->result", b"BootImageNum", b"U-Boot 2016.01-rc3",
          b"(Aug 29 2022", b"13120220829", b"%s=0x%08x,"]:
    offs = []
    s = 0
    while True:
        k = data.find(t, s)
        if k < 0:
            break
        offs.append(k)
        s = k + 1
    print(f"  {t!r}: {[hex(o) for o in offs]}")

print()
print("== container1 @0x0 头部 96B ==")
for i in range(0, 96, 16):
    chunk = data[i:i+16]
    print(f"  {i:#06x}: {chunk.hex()}  |" + "".join(chr(c) if 32 <= c < 127 else "." for c in chunk) + "|")

print()
print("== container2 @0x5e800 头部 256B ==")
for i in range(0x5e800, 0x5e900, 16):
    chunk = data[i:i+16]
    print(f"  {i:#06x}: {chunk.hex()}  |" + "".join(chr(c) if 32 <= c < 127 else "." for c in chunk) + "|")

print()
print("== 搜索 container2 区域内 ARM64 代码特征 (stp x29,x30 / mov x29,sp 序列) ==")
# ARM64: stp x29,x30,[sp,#-N]! = 0xa9bx7bfd ; mov x29, sp = 0x910003fd
import struct
found = []
for off in range(0x5e800, 0xc0000 - 8, 4):
    w1 = data[off:off+4]
    if w1 in (b"\xfd\x7b\xbd\xa9", b"\xfd\x7b\xbf\xa9", b"\xfd\x7b\xbe\xa9", b"\xfd\x7b\xb8\xa9"):
        w2 = data[off+4:off+8]
        if w2 == b"\xfd\x03\x00\x91":
            found.append(off)
print(f"  stp+mov x29,sp 计数: {len(found)}; 前40个: {[hex(f) for f in found[:40]]}")
