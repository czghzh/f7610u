#!/usr/bin/env python3
"""定位容器头里的 CRC32 字段：对比 头里的候选字段 vs 实算 CRC32(payload)"""
import struct, zlib

def probe(name):
    d = open(name, "rb").read()
    h = d[:0x240]
    w = lambda o: struct.unpack("<I", h[o:o+4])[0]
    print(f"===== {name} =====")
    # 候选字段
    cands = {}
    for off in range(0x140, 0x240, 4):
        v = w(off)
        if v != 0 and v != 0xffffffff:
            cands[off] = v
    # FIT
    fit_magic = d[0x240:0x244]
    fit_total = struct.unpack(">I", d[0x244:0x248])[0]
    print(f"  FIT magic={fit_magic.hex()} totalsize={fit_total:#x} ({fit_total})")
    # 实算多种 CRC
    payload = d[0x240:0x240+fit_total]
    tests = {
        "crc32(payload)": zlib.crc32(payload) & 0xffffffff,
        "crc32(hdr[0:0x240]+payload)": zlib.crc32(d[:0x240+fit_total]) & 0xffffffff,
        "crc32(hdr[0x14:])": zlib.crc32(h[0x14:]) & 0xffffffff,
        "crc32(payload+FF tail to len_field)": None,
    }
    # 用头里声明的长度算（0x148 字段）
    ln = w(0x148)
    if ln and ln < len(d):
        tests["crc32(payload[:len@0x148])"] = zlib.crc32(d[0x240:0x240+ln]) & 0xffffffff
    # 找匹配
    for tname, tv in tests.items():
        if tv is None: continue
        hit = [f"{o:#x}={cands[o]:#010x}" for o in cands if cands[o] == tv]
        print(f"  {tname} = {tv:#010x}   -> fields: {hit if hit else 'no match'}")
    # 打印全部候选字段
    print("  header fields (0x140-0x23f):")
    for o in sorted(cands):
        print(f"    +{o:#05x}: {cands[o]:#010x} ({cands[o]})")

probe("backup/mtd7-kernel1.bin")
probe("backup/mtd8-kernel2.bin")
