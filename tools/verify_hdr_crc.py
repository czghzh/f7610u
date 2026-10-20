#!/usr/bin/env python3
import struct, zlib

for name in ("backup/mtd7-kernel1.bin", "backup/mtd8-kernel2.bin"):
    d = open(name, "rb").read(0x240)
    calc = zlib.crc32(d[0x114:0x1e0]) & 0xffffffff
    exp = struct.unpack_from("<I", d, 0x1e0)[0]
    ok = "MATCH" if calc == exp else "NO"
    print(f"{name}")
    print(f"  crc32(hdr[0x114:0x1e0]) = {calc:#010x}   field[0x1e0] = {exp:#010x}   {ok}")
    for s, e in [(0, 0x23c), (0x10, 0x23c), (0x14, 0x23c), (0, 0x240), (0x1e0, 0x240)]:
        print(f"  crc32(hdr[{s:#x}:{e:#x}]) = {zlib.crc32(d[s:e]) & 0xffffffff:#010x}")
