#!/usr/bin/env python3
"""解析 kernel 容器头（kernel1/kernel2 备份）"""
import struct, sys

def dump(name, n=0x100):
    d = open(name, "rb").read(0x400)
    print(f"===== {name} =====")
    print("--- raw 0x00-0x%x ---" % (n - 1))
    for i in range(0, n, 16):
        c = d[i:i+16]
        hx = " ".join("%02x" % b for b in c)
        tx = "".join(chr(b) if 32 <= b < 127 else "." for b in c)
        print("%04x: %-47s |%s|" % (i, hx, tx))
    print("--- u32 words 0x00-0x50 ---")
    for i in range(0, 0x50, 4):
        v = struct.unpack("<I", d[i:i+4])[0]
        print("  +%#04x: %#010x  (%d)" % (i, v, v))
    print("--- payload check @0x240 ---")
    print("  bytes:", d[0x240:0x250].hex())
    print("  magic_le:", hex(struct.unpack("<I", d[0x240:0x244])[0]))
    # 打印 0x14-0x240 之间的非零非FF区段（签名区特征）
    print("--- 0x14..0x240 region type map (per 16B) ---")
    for i in range(0x14, 0x240, 16):
        c = d[i:i+16]
        if all(b == 0xff for b in c):
            t = "FF"
        elif all(b == 0x00 for b in c):
            t = "00"
        else:
            t = "DATA"
        print("  %#06x..%#06x %s  %s" % (i, i+16, t, " ".join("%02x" % b for b in c)))
    print()

for nm in sys.argv[1:]:
    dump(nm)
