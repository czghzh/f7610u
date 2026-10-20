#!/usr/bin/env python3
"""深入解析容器头 0x100-0x250 区域 + 双 kernel 对比"""
import struct, sys

def region_map(d, a, b, step=16):
    for i in range(a, b, step):
        c = d[i:i+step]
        if all(x == 0xff for x in c):
            t = "FF  "
        elif all(x == 0x00 for x in c):
            t = "00  "
        else:
            t = "DATA"
        print("  %#06x %s %s" % (i, t, " ".join("%02x" % x for x in c)))

def main():
    k1 = open("backup/mtd7-kernel1.bin", "rb").read(0x400)
    k2 = open("backup/mtd8-kernel2.bin", "rb").read(0x400)
    print("=== kernel1 0x0f0-0x250 ===")
    region_map(k1, 0x0f0, 0x250)
    print()
    print("=== kernel2 0x0000-0x0100 ===")
    region_map(k2, 0x0000, 0x0100)
    print()
    print("=== kernel2 0x0100-0x250 ===")
    region_map(k2, 0x100, 0x250)
    print()
    print("=== kernel2 u32 @0x00-0x20 ===")
    for i in range(0, 0x20, 4):
        print("  +%#04x: %#010x" % (i, struct.unpack("<I", k2[i:i+4])[0]))
    # 找 kernel2 里的 FIT 魔数位置
    for name, d in (("k1", k1), ("k2", k2)):
        idx = []
        for i in range(0, 0x400 - 4):
            if d[i:i+4] == b"\xd0\x0d\xfe\xed":
                idx.append(i)
        print("%s FIT magic at: %s" % (name, [hex(i) for i in idx]))
    # 找 kernel2 的容器 magic
    print("kernel2 head hex:", k2[:32].hex())

main()
