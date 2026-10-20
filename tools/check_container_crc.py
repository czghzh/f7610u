#!/usr/bin/env python3
"""验证容器头里的 CRC32 字段 + payload 长度"""
import struct, zlib

def check(name, hdr_crc_field_off, payload_off=0x240):
    d = open(name, "rb").read()
    h = d[:0x240]
    flag = struct.unpack("<I", h[0x140:0x144])[0]
    crc_field = struct.unpack("<I", h[0x144:0x148])[0]
    plen = struct.unpack("<I", h[0x148:0x14c])[0]
    poff = struct.unpack("<I", h[0x14c:0x150])[0]
    # FIT totalsize
    fit_magic = d[payload_off:payload_off+4]
    fit_total = struct.unpack(">I", d[payload_off+4:payload_off+8])[0] if fit_magic == b"\xd0\x0d\xfe\xed" else -1
    payload = d[payload_off:payload_off+plen]
    calc_crc = zlib.crc32(payload) & 0xffffffff
    print(f"===== {name} =====")
    print(f"  flag={flag} crc_field={crc_field:#010x} plen={plen:#010x} ({plen}) poff={poff:#x}")
    print(f"  FIT magic={fit_magic.hex()} totalsize={fit_total:#x} ({fit_total})")
    print(f"  plen == FIT totalsize? {plen == fit_total}")
    print(f"  crc32(payload[{plen}]) = {calc_crc:#010x}   match crc_field? {calc_crc == crc_field}")
    # 也试 plen 包含 0x240 的情况
    print(f"  crc32(payload) as-is match: {calc_crc == crc_field}")
    # 试 CRC32 of payload with different length interpretations
    for L, tag in ((plen, "plen"), (plen, "plen"), (fit_total, "fit_total")):
        pass
    # 试 0x14..0x114 的 256B 区是否 CRC
    sig = h[0x14:0x114]
    print(f"  crc32(sig 256B @0x14) = {zlib.crc32(sig)&0xffffffff:#010x}")

check("backup/mtd7-kernel1.bin", 0x144)
check("backup/mtd8-kernel2.bin", 0x144)
