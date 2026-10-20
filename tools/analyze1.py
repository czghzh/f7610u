#!/usr/bin/env python3
# analyze1.py — 反汇编交叉引用修正扫描 + 环境块分析 + 流水线区段导出
import re, zlib, struct

WS = "/home/wen/WorkBuddy/zx/f7610u"
DIS = f"{WS}/work/uboot_full.dis"
BIN = f"{WS}/backup/mtd1-uboot.bin"
UB = 0x84000000

targets = {
    "main_loop_fmt": 0x8403ec88,
    "bootcmd": 0x8403d980,
    "versioninfo": 0x8403e340,
    "saveenv_str": 0x8403e2b8,
    "verify_vmlinuz": 0x8403e428,
    "RSA_OK": 0x8403e418,
    "RSA_ERR": 0x8403e448,
    "zboot_init": 0x8403e488,
    "select_fmt": 0x8403e368,
    "BootImageNum": 0x8403e378,
    "printenv_bootargs": 0x8403d968,
    "Failture": 0x8403d988,
    "bootzpon": 0x8403d9a0,
    "no_avail_ver": 0x8403e2a0,
    "cmdline_fmt": 0x8403e2f8,
    "lseek_fmt": 0x8403e2c0,
    "Hit_any_key": 0x8403ecb0,
    "bootdelay": 0x8403ec78,
    "bootargs_tmpl": 0x8403e1ef,
    "search_fmt": 0x8403e388,
    "tmp_value": 0x8403e358,
    "nofs": 0x8403dd9c,
    "setbootargs_ubi10": 0x8403e11f,
    "help_bootk": 0x8403da10,
    "downver": 0x8403d948,
}

lines = open(DIS, errors="replace").read().splitlines()
adrp_re = re.compile(r"^\s*([0-9a-f]{8}):\s+[0-9a-f]{8}\s+adrp\s+(\w+),\s*(0x[0-9a-f]+)")
add_re = re.compile(r"^\s*([0-9a-f]{8}):\s+[0-9a-f]{8}\s+add\s+(\w+),\s*(\w+),\s*#(0x[0-9a-fA-F]+|\d+)(?:,\s*lsl\s*#(\d+))?")

# Phase1: collect (lineidx, reg, base) for every adrp
adrps = []
for i, ln in enumerate(lines):
    m = adrp_re.match(ln)
    if m:
        adrps.append((i, m.group(2), int(m.group(3), 16)))

# Phase2: for each adrp, scan forward for add on same dest-reg with rn==reg
xrefs = {}
for i, reg, base in adrps:
    for j in range(i + 1, min(i + 13, len(lines))):
        ln = lines[j]
        if "adrp" in ln:
            m2 = adrp_re.match(ln)
            if m2 and m2.group(2) == reg:
                break  # reg reloaded by new adrp
        m = add_re.match(ln)
        if not m:
            continue
        pc = int(m.group(1), 16)
        rd, rn = m.group(2), m.group(3)
        imm = int(m.group(4), 0)
        if m.group(5):
            imm <<= int(m.group(5))
        if rn == reg:
            tgt = base + imm
            xrefs.setdefault(tgt, []).append(pc)
            break

print("== xrefs (adrp+add) for target strings ==")
for name, a in targets.items():
    hits = xrefs.get(a, [])
    print(f"{name:20s} {a:#x}: " + (", ".join(hex(h) for h in hits) if hits else "NONE"))

print()
print("== contexts for found xrefs ==")
line_of = {}
for i, ln in enumerate(lines):
    m = re.match(r"^\s*([0-9a-f]{8}):", ln)
    if m:
        line_of[int(m.group(1), 16)] = i
for name, a in targets.items():
    for pc in xrefs.get(a, []):
        li = line_of.get(pc)
        if li is None:
            continue
        print(f"--- {name} @{a:#x} ref pc={pc:#x} ---")
        for k in range(max(0, li - 4), min(len(lines), li + 4)):
            print(lines[k])

print()
print("== 流水线区段 0x84006f00-0x84007700 ==")
want = [(0x84006f00, 0x84007700)]
for a0, a1 in want:
    print(f"---- {a0:#x} .. {a1:#x} ----")
    for ln in lines:
        m = re.match(r"^\s*([0-9a-f]{8}):", ln)
        if m:
            v = int(m.group(1), 16)
            if a0 <= v < a1:
                print(ln)
