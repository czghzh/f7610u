#!/usr/bin/env python3
"""C2b: 改 payload len/crc/readlen + 重算 header crc(0x1e0) + 我们的 FIT"""
import struct, zlib, hashlib

hdr = bytearray(open("backup/mtd8-kernel2.bin", "rb").read(0x240))
fit = open("tftp/f7610u-net-v8.8c-rw.itb", "rb").read()

plen = len(fit)
crc = zlib.crc32(fit) & 0xffffffff
readlen = 0x240 + plen + 1

struct.pack_into("<I", hdr, 0x148, plen)
struct.pack_into("<I", hdr, 0x150, crc)
struct.pack_into("<I", hdr, 0x158, readlen)
struct.pack_into("<I", hdr, 0x1b4, readlen)

# 重算 header crc
hcrc = zlib.crc32(bytes(hdr[0x114:0x1e0])) & 0xffffffff
struct.pack_into("<I", hdr, 0x1e0, hcrc)

print(f"plen={plen:#x} payload_crc={crc:#010x} readlen={readlen:#x}")
print(f"header_crc(0x1e0) = {hcrc:#010x}")
print(f"verify: crc32(hdr[0x114:0x1e0]) = {zlib.crc32(bytes(hdr[0x114:0x1e0]))&0xffffffff:#010x}")

out = bytes(hdr) + fit
open("httpdrop/k2_c2b.bin", "wb").write(out)
print(f"out size={len(out):#x} ({len(out)})  md5={hashlib.md5(out).hexdigest()}")

# 分段 md5（跳块感知）
print("seg1 0x0 0x400000:", hashlib.md5(out[0:0x400000]).hexdigest())
print("A3   0x800000 0x2c0000:", hashlib.md5(out[0x800000:0xac0000]).hexdigest())
print("B1   src 0xac0000 0x400000:", hashlib.md5(out[0xac0000:0xec0000]).hexdigest())
print("B2   src 0xec0000 0x400000:", hashlib.md5(out[0xec0000:0x12c0000]).hexdigest())
print("B3   src 0x12c0000 0x400000:", hashlib.md5(out[0x12c0000:0x16c0000]).hexdigest())
print("B4   src 0x16c0000 0x3b7f50:", hashlib.md5(out[0x16c0000:0x1a77f50]).hexdigest())
