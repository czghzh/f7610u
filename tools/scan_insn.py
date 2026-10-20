#!/usr/bin/env python3
"""指令级扫描：解码 adrp+add，找目标字符串地址的代码引用（含 ±0x800 容差）"""
import struct

data = open("../backup/mtd1-uboot.bin", "rb").read()
UB = 0x84000000
N = len(data) // 4
words = struct.unpack("<%dI" % N, data[:N*4])

def dec_adrp(w):
    if (w & 0x9F000000) != 0x90000000:
        return None
    rd = w & 0x1F
    immlo = (w >> 29) & 3
    immhi = (w >> 5) & 0x7FFFF
    imm = (immhi << 2) | immlo
    if imm & (1 << 20):
        imm -= (1 << 21)
    return rd, imm << 12

def dec_add64(w):
    if (w & 0xFFC00000) != 0x91000000:
        return None
    rd = w & 0x1F
    rn = (w >> 5) & 0x1F
    imm = (w >> 10) & 0xFFF
    return rd, rn, imm

TARGETS = {
    "vmlinuz_crc_error": 0x8403e3f8,
    "header_crc_error": 0x8403e3b8,
    "invalid_kernel_crc": 0x8403dcb8,
    "invalid_header_crc": 0x8403dc98,
    "verify_vmlinuz_ok": 0x8403e428,
    "fs_crc_error": 0x8403e350,
    "RSA_OK": 0x8403e418,
    "RSA_ERR": 0x8403e408,
}

hits = []
for i in range(N):
    a = dec_adrp(words[i])
    if not a:
        continue
    rd, imm = a
    pc = UB + i * 4
    page = (pc & ~0xFFF) + imm
    # 后续 16 条指令内找 add rd, rd, #imm2
    for j in range(i + 1, min(i + 16, N)):
        if dec_adrp(words[j]):
            break
        ad = dec_add64(words[j])
        if not ad:
            continue
        ard, arn, aimm = ad
        if arn != rd:
            continue
        tgt = page + aimm
        for name, T in TARGETS.items():
            for delta in (0, -0x800, 0x800, -8, 8, -0x10, 0x10):
                if tgt == T + delta:
                    hits.append((pc, UB + j * 4, name, T, tgt, delta))
        break

print("=== adrp+add hits ===")
for pc, apc, name, T, tgt, delta in hits:
    print(f"  {pc:#010x} (add@{apc:#x}) -> {name:<22} target={tgt:#x} T={T:#x} d={delta:+#x}")

# 上下文
import re
lines = open("uboot_full.dis", errors="replace").read().splitlines()
seen = set()
for pc, apc, name, T, tgt, delta in hits:
    if pc in seen: continue
    seen.add(pc)
    print(f"\n===== code @{pc:#010x} ({name}) =====")
    idx = None
    for k, ln in enumerate(lines):
        m = re.match(r"^\s*([0-9a-f]{8}):", ln)
        if m and int(m.group(1), 16) == pc:
            idx = k; break
    if idx is None:
        print("  (dis lines not found)")
        continue
    for ln in lines[max(0, idx - 10):idx + 24]:
        print(ln)
