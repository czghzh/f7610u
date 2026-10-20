#!/usr/bin/env python3
# strscan.py — 在 mtd1-uboot.bin 里定位关键字符串 + 在 uboot_full.dis 里找 adrp/add 代码引用
import re, sys
BIN = '/home/wen/WorkBuddy/zx/f7610u/backup/mtd1-uboot.bin'
DIS = '/home/wen/WorkBuddy/zx/f7610u/work/uboot_full.dis'
UB = 0x84000000
data = open(BIN, 'rb').read()

targets = [b"main_loop entered", b"Hit any key", b"zboot info init done",
           b"verify vmlinuz success", b"skip bad block", b"select=0x",
           b"search=", b"lseek=", b"cmdline=", b"kernel start step",
           b"versioninfo", b"bootcmd", b"bootdelay", b"preboot",
           b"saveenv", b"Saving Environment", b"bootzpon", b"bootstatus",
           b"zboot", b"boota"]

print("== string occurrences in mtd1-uboot.bin (file off / vaddr) ==")
occ = {}
for t in targets:
    hits = []
    start = 0
    while len(hits) < 40:
        i = data.find(t, start)
        if i < 0: break
        hits.append(i); start = i + 1
    occ[t] = hits
    print(f"{t.decode()!r}: " + ", ".join(f"{h:#x}/{UB+h:#x}" for h in hits))

# 打印每个命中点的上下文
print("\n== context ==")
for t in occ:
    for h in occ[t]:
        s = max(0, h-24); e = min(len(data), h+len(t)+48)
        ctx = data[s:e]
        txt = ''.join(chr(c) if 32 <= c < 127 else '.' for c in ctx)
        print(f"@{UB+h:#x}: {txt}")

# 解析 dis 中 adrp/add 目标
print("\n== code xrefs (adrp+add) ==")
adrp_re = re.compile(r'^\s*([0-9a-f]{8}):\s+[0-9a-f]{8}\s+adrp\s+(\w+),\s*(0x[0-9a-f]+)')
add_re = re.compile(r'^\s*([0-9a-f]{8}):\s+[0-9a-f]{8}\s+add\s+(\w+),\s*(\w+),\s*(#0x[0-9a-f]+|#\d+)(,\s*lsl\s*#(\d+))?')
regs = {}
xrefs = {}
for ln in open(DIS, errors='replace'):
    m = adrp_re.match(ln)
    if m:
        regs[m.group(2)] = (int(m.group(1), 16), int(m.group(3), 16))
        continue
    m = add_re.match(ln)
    if m:
        pc = int(m.group(1), 16); rd = m.group(2); rn = m.group(3)
        imm = int(m.group(4).lstrip('#'), 0)
        if m.group(5): imm <<= int(m.group(6))
        if rn in regs:
            tgt = regs[rn][1] + imm
            xrefs.setdefault(tgt, []).append((pc, rd))

seen = set()
for t in occ:
    for h in occ[t]:
        a = UB + h
        if a in xrefs and a not in seen:
            seen.add(a)
            print(f"{t.decode()!r} @{a:#x}: " + ", ".join(f"pc={pc:#x}({rd})" for pc, rd in xrefs[a]))
