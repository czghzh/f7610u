#!/usr/bin/env python3
"""追踪 vmlinuz crc error 0xb2d3d355 的来源 + 头部随机字段性质"""
import zlib, struct

# --- crc32c (Castagnoli) 纯 python ---
def crc32c(data, crc=0):
    poly = 0x82F63B78
    crc ^= 0xFFFFFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (poly if crc & 1 else 0)
    return crc ^ 0xFFFFFFFF

print("=== 1) what is 0xb2d3d355? ===")
# 我们提交的 k2_c1.bin：hdr(0x240) + FIT(0x1a77d10)，读取长度按头部 0x158 字段
k2c1 = open("httpdrop/k2_c1.bin", "rb").read()
hdr = open("backup/mtd8-kernel2.bin", "rb").read(0x240)
declared_len = struct.unpack("<I", hdr[0x158:0x15c])[0]
print(f"  declared read len (hdr+0x158) = {declared_len:#x}")
end = 0x240 + declared_len  # 读的是从分区起点开始的 declared_len？
payload_decl = struct.unpack("<I", hdr[0x148:0x14c])[0]
print(f"  payload len (hdr+0x148) = {payload_decl:#x}")

# 场景A：读 [0, declared_len)（分区起点开始）
bufA = bytearray(k2c1)
if len(bufA) < declared_len:
    bufA += b"\xff" * (declared_len - len(bufA))
bufA = bytes(bufA[:declared_len])
print(f"  A: crc32(read[0:{declared_len:#x}])         = {zlib.crc32(bufA)&0xffffffff:#010x}")
print(f"  A: crc32(read[0x240:])                     = {zlib.crc32(bufA[0x240:])&0xffffffff:#010x}")

# 场景B：读 [0, 0x240+payload_decl)
end2 = 0x240 + payload_decl
bufB = bytearray(k2c1)
if len(bufB) < end2:
    bufB += b"\xff" * (end2 - len(bufB))
bufB = bytes(bufB[:end2])
print(f"  B: crc32(read[0:{end2:#x}])         = {zlib.crc32(bufB)&0xffffffff:#010x}")
print(f"  B: crc32(read[0x240:])                     = {zlib.crc32(bufB[0x240:])&0xffffffff:#010x}")

# 场景C：crc32(我们的FIT)
print(f"  C: crc32(our FIT)                          = {zlib.crc32(k2c1[0x240:])&0xffffffff:#010x}")
# 场景D: crc32c
print(f"  D: crc32c(read[0x240:]) A                  = {crc32c(bufA[0x240:]):#010x}")
print(f"  D: crc32c(our FIT)                         = {crc32c(k2c1[0x240:]):#010x}")

print()
print("=== 2) 头部随机字段 vs 原厂 payload 的各种校验 ===")
for name, fn in (("kernel1", "backup/mtd7-kernel1.bin"), ("kernel2", "backup/mtd8-kernel2.bin")):
    d = open(fn, "rb").read()
    h = d[:0x240]
    fit_total = struct.unpack(">I", d[0x244:0x248])[0]
    payload = d[0x240:0x240+fit_total]
    print(f"  {name}: fit_total={fit_total:#x}")
    print(f"    crc32(payload)      = {zlib.crc32(payload)&0xffffffff:#010x}")
    print(f"    crc32c(payload)     = {crc32c(payload):#010x}")
    print(f"    adler32(payload)    = {zlib.adler32(payload)&0xffffffff:#010x}")
    import hashlib
    md5 = hashlib.md5(payload).digest()
    sha = hashlib.sha1(payload).digest()
    print(f"    md5[:4] LE          = {struct.unpack('<I', md5[:4])[0]:#010x}   md5: {md5.hex()}")
    print(f"    sha1[:4] LE         = {struct.unpack('<I', sha[:4])[0]:#010x}")
    print(f"    hdr 0x150 = {struct.unpack('<I',h[0x150:0x154])[0]:#010x}")
    print(f"    hdr 0x1ac = {struct.unpack('<I',h[0x1ac:0x1b0])[0]:#010x}")
    print(f"    hdr 0x1e0 = {struct.unpack('<I',h[0x1e0:0x1e4])[0]:#010x}")
    print(f"    hdr 0x23c = {struct.unpack('<I',h[0x23c:0x240])[0]:#010x}")
