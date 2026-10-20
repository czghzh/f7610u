#!/usr/bin/env python3
# extract9.py — 最终定位：4处#0xb78上下文 + 解析函数完整体 + 窗口化目标扫描
import re

WS = "/home/wen/WorkBuddy/zx/f7610u"
data = open(f"{WS}/backup/mtd1-uboot.bin", "rb").read()
lines = open(f"{WS}/work/uboot_full.dis", errors="replace").read().splitlines()

def dump(a0, a1, title):
    print(f"======== {title} ========")
    for ln in lines:
        m = re.match(r"^\s*([0-9a-f]{8}):", ln)
        if m:
            v = int(m.group(1), 16)
            if a0 <= v < a1:
                print(ln)

def ctx(addr, back=6, fwd=6, title=""):
    li = None
    for i, ln in enumerate(lines):
        m = re.match(r"^\s*([0-9a-f]{8}):", ln)
        if m and int(m.group(1), 16) == addr:
            li = i; break
    if li is None:
        print(f"--- {title}: addr {addr:#x} not found")
        return
    print(f"--- {title} @{addr:#x} ---")
    for k in range(max(0, li-back), min(len(lines), li+fwd)):
        print(lines[k])

ctx(0x84004224, 8, 4, "#0xb78 site A")
ctx(0x8401b3ac, 8, 4, "#0xb78 site B")
ctx(0x84008760, 6, 6, "#0xb78 site C (selector)")
ctx(0x84008ca0, 6, 6, "#0xb78 site D (settings)")

print()
dump(0x84007010, 0x84007290, "parser body 0x84006fe0 cont. (0x84007010-0x84007290)")

print()
print("== 窗口扫描: add 目标落在关键串 ±0x18 ==")
add_re = re.compile(r"^\s*([0-9a-f]{8}):\s+[0-9a-f]{8}\s+add\s+(\w+),\s*(\w+),\s*#(0x[0-9a-fA-F]+|\d+)")
adrp_re = re.compile(r"^\s*([0-9a-f]{8}):\s+[0-9a-f]{8}\s+adrp\s+(\w+),\s*(0x[0-9a-f]+)")
targets = {
    "select=0x%x": 0x8403e368, "no_avail_ver": 0x8403e2a0, "bootcmd": 0x8403d980,
    "main_loop_fmt": 0x8403ec88, "Failture": 0x8403d988, "bootzpon": 0x8403d9a0,
    "search=0x": 0x8403e388, "tmp=%s": 0x8403db10, "pKeyName": 0x8403db28,
    "invalid tmp": 0x8403db50, "erase_other": 0x8403db68, "zboot_init": 0x8403e488,
    "lseek": 0x8403e2c0, "tmpl_e1e8": 0x8403e1e8, "bootm_fmt": 0x8403e290,
    "ver_uboot": 0x8403e2d0, "BootImageNum_s": 0x8403e378,
}
adrps = []
for i, ln in enumerate(lines):
    m = adrp_re.match(ln)
    if m:
        adrps.append((i, m.group(2), int(m.group(3), 16)))
for i, reg, base in adrps:
    for j in range(i + 1, min(i + 40, len(lines))):
        ln = lines[j]
        m2 = adrp_re.match(ln)
        if m2 and m2.group(2) == reg:
            break
        if re.match(r"^\s*[0-9a-f]{8}:\s+[0-9a-f]{8}\s+(mov|ldr|add|sub|orr|movz|movk|ldrb|ldrh|ldp|adr)\s+" + reg + r",", ln):
            break
        m = add_re.match(ln)
        if not m:
            continue
        pc = int(m.group(1), 16); rd, rn = m.group(2), m.group(3); imm = int(m.group(4), 0)
        if rn == reg:
            tgt = base + imm
            for name, T in targets.items():
                if T <= tgt <= T + 0x18:
                    print(f"  {name:16s} ref pc={pc:#x} -> {tgt:#x} (offset +{tgt-T:#x})")
            break
