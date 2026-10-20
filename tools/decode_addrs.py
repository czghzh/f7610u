#!/usr/bin/env python3
"""Decode AArch64 mov/movk address chains in v3dis dumps -> physical addresses.
Alias rule (calibrated): virtual = physical + 0xFFFFFFD000000000."""
import re, glob, os, sys

ALIAS = 0xFFFFFFD000000000

mov_re = re.compile(r'^\s*([0-9a-f]+):\s+([0-9a-f]{8})\s+(mov|movk)\s+(x\d+|w\d+),\s+#(0x[0-9a-f]+|\d+)(?:,\s*lsl\s+#(\d+))?')
add_re = re.compile(r'^\s*([0-9a-f]+):\s+([0-9a-f]{8})\s+add\s+(x\d+),\s+(x\d+),\s+#(0x[0-9a-f]+|\d+)')
strldr_re = re.compile(r'^\s*([0-9a-f]+):\s+([0-9a-f]{8})\s+(str|ldr)\w*\s+(w\d+|x\d+),\s+\[(x\d+)(?:,\s*#(0x[0-9a-f]+|\d+))?\]')

def parse_imm(s):
    return int(s, 16) if s.startswith('0x') else int(s)

results = []
for path in sorted(glob.glob('/home/wen/WorkBuddy/zx/f7610u/work/v3dis/*.txt')):
    fname = os.path.basename(path)
    regs = {}  # reg -> current value (only track x regs built from mov/movk)
    pending = {}  # reg -> (funcname, value)
    cur_func = None
    with open(path) as f:
        for line in f:
            m = re.match(r'^0+([0-9a-f]+) <([^>]+)>:', line)
            if m:
                cur_func = m.group(2)
                regs = {}
                continue
            m = mov_re.match(line)
            if m:
                addr, enc, op, reg, imm, shift = m.groups()
                v = parse_imm(imm)
                shift = int(shift) if shift else 0
                if op == 'mov':
                    # mov xN, #imm : MOVN or MOVZ; disasm prints resolved value
                    regs[reg] = v & 0xFFFFFFFFFFFFFFFF
                else:  # movk
                    if reg in regs:
                        mask = 0xFFFF << shift
                        regs[reg] = (regs[reg] & ~mask) | ((v & 0xFFFF) << shift)
                    else:
                        regs[reg] = (v & 0xFFFF) << shift
                continue
            m = add_re.match(line)
            if m:
                addr, enc, dst, src, imm = m.groups()
                if src in regs:
                    regs[dst] = (regs[src] + parse_imm(imm)) & 0xFFFFFFFFFFFFFFFF
                continue
            m = strldr_re.match(line)
            if m:
                addr, enc, op, val, base, off = m.groups()
                if base in regs and off is None:
                    v = regs[base]
                    if v > ALIAS:
                        phys = v - ALIAS
                        results.append((fname, cur_func, int(addr, 16), op, val, phys))
                elif base in regs and off is not None:
                    v = regs[base] + parse_imm(off)
                    if v > ALIAS:
                        phys = v - ALIAS
                        results.append((fname, cur_func, int(addr, 16), op, val, phys))

for fname, func, addr, op, val, phys in results:
    print(f"{fname:42s} {func or '?':38s} {addr:06x} {op} {val:>5s} -> phys 0x{phys:x}")
