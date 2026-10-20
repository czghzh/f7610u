#!/usr/bin/env python3
"""扫描 adrp+add 引用：定位 vmlinuz crc 校验代码"""
import re, sys

lines = open("uboot_full.dis", errors="replace").read().splitlines()

adrp_re = re.compile(r"^\s*([0-9a-f]{8}):\s+([0-9a-f]{8})\s+adrp\s+(x\d+),\s+0x([0-9a-f]+)")
add_re  = re.compile(r"^\s*([0-9a-f]{8}):\s+[0-9a-f]{8}\s+add\s+(x\d+),\s+(x\d+),\s+#(0x[0-9a-f]+|\d+)")

targets = {
    "vmlinuz_crc_error": 0x8403e3f8,
    "header_crc_error": 0x8403e3b8,
    "invalid_kernel_crc": 0x8403dcb8,
    "invalid_header_crc": 0x8403dc98,
    "verify_vmlinuz_ok": 0x8403e428,
    "fs_crc_error": 0x8403e350,
}

# 收集 adrp
adrps = []
for ln in lines:
    m = adrp_re.match(ln)
    if m:
        adrps.append((int(m.group(1), 16), m.group(3), int(m.group(4), 16)))

hits = []
for i, (pc, reg, base) in enumerate(adrps):
    # 向后找同一寄存器的 add
    tgt = None
    for j in range(i + 1, min(i + 12, len(lines))):
        ln = lines[j]
        m = add_re.match(ln)
        if not m: 
            # 如果遇到新的 adrp 同寄存器则放弃
            m2 = adrp_re.match(ln)
            if m2 and m2.group(3) == reg: break
            continue
        rd, rn = m.group(2), m.group(3)
        imm = int(m.group(4), 0)
        if rn == reg:
            tgt = base + imm
            break
    if tgt is None: continue
    for name, T in targets.items():
        for delta in (0, -0x800, 0x800, -8, 8):
            if tgt == T + delta:
                hits.append((pc, name, T, tgt, delta))

print("=== references ===")
for pc, name, T, tgt, delta in sorted(hits):
    print(f"  code {pc:#010x} -> {name} (target {T:#x}, actual {tgt:#x}, delta {delta:+#x})")

# 输出每个命中点前后 30 行
seen = set()
for pc, name, T, tgt, delta in sorted(hits):
    if pc in seen: continue
    seen.add(pc)
    print(f"\n===== code @{pc:#010x} ({name}) context =====")
    # 找行号
    idx = None
    for k, ln in enumerate(lines):
        m = re.match(r"^\s*([0-9a-f]{8}):", ln)
        if m and int(m.group(1), 16) == pc:
            idx = k
            break
    if idx is None: continue
    for ln in lines[max(0, idx - 12):idx + 20]:
        print(ln)
