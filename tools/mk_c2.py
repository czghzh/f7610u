#!/usr/bin/env python3
"""构造实验 C2 镜像：原厂 kernel2 头（修正长度+CRC）+ 我们的 FIT"""
import struct, zlib, hashlib

hdr = bytearray(open("backup/mtd8-kernel2.bin", "rb").read(0x240))
fit = open("tftp/f7610u-net-v8.8c-rw.itb", "rb").read()

plen = len(fit)                          # FIT totalsize
crc = zlib.crc32(fit) & 0xffffffff
readlen = 0x240 + plen + 1

print(f"FIT len = {plen:#x} ({plen})")
print(f"CRC32(FIT) = {crc:#010x}")
print(f"readlen = {readlen:#x}")

old = [struct.unpack_from("<I", hdr, o)[0] for o in (0x148, 0x150, 0x158, 0x1b4)]
print(f"old fields: 0x148={old[0]:#x} 0x150={old[1]:#x} 0x158={old[2]:#x} 0x1b4={old[3]:#x}")

struct.pack_into("<I", hdr, 0x148, plen)
struct.pack_into("<I", hdr, 0x150, crc)
struct.pack_into("<I", hdr, 0x158, readlen)
struct.pack_into("<I", hdr, 0x1b4, readlen)

new = [struct.unpack_from("<I", hdr, o)[0] for o in (0x148, 0x150, 0x158, 0x1b4)]
print(f"new fields: 0x148={new[0]:#x} 0x150={new[1]:#x} 0x158={new[2]:#x} 0x1b4={new[3]:#x}")

# 校验：重算
assert zlib.crc32(fit) & 0xffffffff == struct.unpack_from("<I", hdr, 0x150)[0]

out = bytes(hdr) + fit
open("httpdrop/k2_c2.bin", "wb").write(out)
print(f"out size = {len(out):#x} ({len(out)})  md5 = {hashlib.md5(out).hexdigest()}")

# 打印头差异摘要
orig = open("backup/mtd8-kernel2.bin", "rb").read(0x240)
diff = [(i, orig[i], bytes(hdr)[i]) for i in range(0x240) if orig[i] != bytes(hdr)[i]]
print(f"header diff bytes: {[(hex(i), hex(a), hex(b)) for i, a, b in diff]}")
