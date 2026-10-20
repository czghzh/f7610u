#!/usr/bin/env python3
"""穷举 header crc 的覆盖范围与字段位置"""
import struct, zlib

def brute(name):
    d = open(name, "rb").read(0x240)
    print(f"===== {name} =====")
    # 收集头部所有 u32 字段
    fields = {}
    for o in range(0, 0x240, 4):
        fields[o] = struct.unpack_from("<I", d, o)[0]
    cands = {0x1e0: "0x1e0", 0x23c: "0x23c", 0x144: "0x144", 0x150: "0x150", 0x1ac: "0x1ac"}
    starts = [0x0, 0x10, 0x14, 0x114, 0x120]
    ends = [0x1dc, 0x1e0, 0x238, 0x23c, 0x240]
    for c_off, c_name in cands.items():
        exp = fields[c_off]
        if exp in (0, 0xffffffff):
            continue
        for s in starts:
            for e in ends:
                if s >= e or s > c_off or e <= c_off and c_off < e:
                    pass
                if not (s <= c_off and e >= c_off):
                    # CRC 不能覆盖自身字段
                    pass
                calc = zlib.crc32(d[s:e]) & 0xffffffff
                if calc == exp:
                    print(f"  MATCH: crc32(hdr[{s:#x}:{e:#x}]) = {calc:#010x} == field {c_name}")
                # 也试 crc32c
    # 反向：假设是 crc32 覆盖 [0:s]，检查所有 s
    for s in range(0, 0x240, 4):
        calc = zlib.crc32(d[0:s]) & 0xffffffff
        for o, v in fields.items():
            if v == calc and o >= s:
                print(f"  MATCH(crc32 hdr[0:{s:#x}]) = {calc:#010x} -> field {o:#x}")
    print("  done")

brute("backup/mtd7-kernel1.bin")
brute("backup/mtd8-kernel2.bin")
