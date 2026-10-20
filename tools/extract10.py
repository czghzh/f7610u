#!/usr/bin/env python3
# extract10.py — 容忍 ±0x800 偏移的引用扫描：锁定失败路径/调试路径字符串的调用点
import re

WS = "/home/wen/WorkBuddy/zx/f7610u"
data = open(f"{WS}/backup/mtd1-uboot.bin", "rb").read()
lines = open(f"{WS}/work/uboot_full.dis", errors="replace").read().splitlines()

def at(a, n=48):
    seg = data[a:a+n]
    return "".join(chr(c) if 32 <= c < 127 else "." for c in seg)

targets = {
    "Failture:boot kernel": 0x8403d988,
    "bootzpon": 0x8403d9a0,
    "printenv bootargs": 0x8403d968,
    "bootcmd(standalone)": 0x8403d980,
    "select=0x%x": 0x8403e368,
    "tmp=%s,value=%d": 0x8403db10,
    "pKeyName": 0x8403db28,
    "no available version": 0x8403e2a0,
    "search=0x%x": 0x8403e388,
    "saveenv": 0x8403e2b8,
    "main_loop fmt": 0x8403ec88,
    "hit any key": 0x8403ecb0,
    "bootdelay": 0x8403ec78,
    "downver version": 0x8403d948,
    "reset(short)": 0x8403d958,
    "debug(short)": 0x8403d960,
    "help bootk": 0x8403da10,
    "name too long": 0x8403da20,
    "zboot init done": 0x8403e488,
    "verify vmlinuz": 0x8403e428,
    "BootImageNum": 0x8403e378,
}

adrp_re = re.compile(r"^\s*([0-9a-f]{8}):\s+[0-9a-f]{8}\s+adrp\s+(\w+),\s*(0x[0-9a-f]+)")
add_re = re.compile(r"^\s*([0-9a-f]{8}):\s+[0-9a-f]{8}\s+add\s+(\w+),\s*(\w+),\s*#(0x[0-9a-fA-F]+|\d+)")

# 收集每个 pc 上每个寄存器的 adrp 基址状态（不限窗口，遇到寄存器被重写则失效）
WRITE_RE = re.compile(r"^\s*[0-9a-f]{8}:\s+[0-9a-f]{8}\s+(?:mov|ldr|ldrb|ldrh|ldp|movz|movk|adr|adrp|sub|and|orr|eor|add|mvn|csel|cset|csinv|neg|ubfx|sxtw|uxtw|mul|madd)\s+(\w+),")

refs = {}
for i, ln in enumerate(lines):
    m = adrp_re.match(ln)
    if m:
        pc = int(m.group(1), 16)
        regs = refs.setdefault(pc, {})
        regs[m.group(2)] = int(m.group(3), 16)
        continue
    m = add_re.match(ln)
    if m:
        pc = int(m.group(1), 16)
        rd, rn = m.group(2), m.group(3)
        imm = int(m.group(4), 0)
        for base_pc in range(pc, pc - 4000, -4):
            st = refs.get(base_pc)
            if st and rn in st:
                tgt = st[rn] + imm
                for name, S in targets.items():
                    for d, tag in [(0, "exact"), (8, "+8"), (0x10, "+0x10"),
                                   (0x800, "+0x800"), (-0x800, "-0x800"), (0x80, "+0x80")]:
                        if tgt == S + d:
                            print(f"{name:22s} {tag:7s} pc={pc:#010x} tgt={tgt:#x}  [str@S]={at(S)[:36]!r}")
                break
print("done. (scan complete)")
