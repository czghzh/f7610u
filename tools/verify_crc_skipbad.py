#!/usr/bin/env python3
"""验证：跳过坏块后 crc32(payload) == hdr[0x150] ?"""
import struct, zlib

def check(name, bad_offs_in_mtd):
    """bad_offs_in_mtd: mtd 分区内的坏块偏移列表（相对分区起点）"""
    d = bytearray(open(name, "rb").read())
    h = bytes(d[:0x240])
    plen = struct.unpack("<I", h[0x148:0x14c])[0]
    poff = struct.unpack("<I", h[0x14c:0x150])[0]
    exp  = struct.unpack("<I", h[0x150:0x154])[0]
    print(f"===== {name} =====")
    print(f"  plen={plen:#x} poff={poff:#x} expected(crc@0x150)={exp:#010x}")
    # 原始（不跳块）
    pay_raw = bytes(d[poff:poff+plen])
    print(f"  crc32(payload raw)          = {zlib.crc32(pay_raw)&0xffffffff:#010x}")
    # 跳块：从 mtd 起点删除坏块 0x20000
    if bad_offs_in_mtd:
        buf = bytearray()
        pos = 0
        for bo in sorted(bad_offs_in_mtd):
            buf += d[pos:bo]
            pos = bo + 0x20000
        buf += d[pos:]
        d2 = bytes(buf)
        pay2 = d2[poff:poff+plen]
        print(f"  crc32(payload skip-bad)     = {zlib.crc32(pay2)&0xffffffff:#010x}   {'<<< MATCH' if (zlib.crc32(pay2)&0xffffffff)==exp else ''}")
        # 也试试 hdr 不变但 payload 跳块
    # 另一个假设：payload 从 0x240 起但在坏块处截断
    print()

# kernel1: mtd7, 坏块在 flash 0x1000000 → mtd7 内 = 0x1000000 - 0x680000 = 0x980000
check("backup/mtd7-kernel1.bin", [0x980000])
# kernel2: mtd8, 坏块在 flash 0x4140000 → mtd8 内 = 0x4140000 - 0x3680000 = 0xAC0000
check("backup/mtd8-kernel2.bin", [0xAC0000])
