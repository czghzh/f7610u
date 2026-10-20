#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从反汇编里抽取 Hal_Read/WriteXmacReg 的寄存器下标，换算成物理地址。
约定：fpga/xmac 下标 -> 物理 = 0x17000000 + idx*4
"""
import re, sys, collections

def parse(path):
    lines = open(path, encoding='utf-8', errors='replace').read().splitlines()
    out = []          # (func, addr, rw, phys)
    func = '?'
    for i, ln in enumerate(lines):
        m = re.match(r'^[0-9a-f]+ <([^>]+)>:', ln)
        if m:
            func = m.group(1)
            continue
        m = re.match(r'^\s*([0-9a-f]+):\s+([0-9a-f]{8})\s+(.*)$', ln)
        if not m:
            continue
        addr, ins, rest = m.group(1), m.group(2), m.group(3)
        if 'Hal_ReadXmacReg' in rest or 'Hal_WriteXmacReg' in rest:
            rw = 'R' if 'ReadXmacReg' in rest else 'W'
            # 往回找 mov/movk w0
            imm = None
            for j in range(i-1, max(i-12, -1), -1):
                l2 = lines[j]
                m2 = re.match(r'^\s*[0-9a-f]+:\s+[0-9a-f]{8}\s+(.*)$', l2)
                if not m2:
                    continue
                t = m2.group(1)
                mm = re.match(r'mov\s+w0, #(0x[0-9a-f]+)', t)
                if mm:
                    if imm is None:
                        imm = int(mm.group(1), 16)
                    else:
                        imm |= int(mm.group(1), 16)
                    continue
                mm = re.match(r'movk\s+w0, #(0x[0-9a-f]+), lsl #16', t)
                if mm:
                    imm = (imm or 0) | (int(mm.group(1), 16) << 16)
                    continue
                if re.match(r'(bl|b)\s', t):
                    break
            if imm is None:
                out.append((func, addr, rw, None))
            else:
                out.append((func, addr, rw, 0x17000000 + (imm & 0x3fffffff) * 4))
    return out

for p in sys.argv[1:]:
    res = parse(p)
    print('==== %s : %d 处调用' % (p, len(res)))
    rd = collections.defaultdict(set)
    wr = collections.defaultdict(set)
    for func, addr, rw, phys in res:
        (rd if rw == 'R' else wr)[phys].add(func)
    for phys in sorted(set(list(rd) + list(wr)), key=lambda x: (x is None, x)):
        if phys is None:
            print('  ????  (未能解析立即数)')
            continue
        idx = (phys - 0x17000000) >> 2
        print('  %08x  idx=%-6d %s%s  %s' % (
            phys, idx,
            ('R' if phys in rd else ' '),
            ('W' if phys in wr else ' '),
            ','.join(sorted((rd.get(phys, set()) | wr.get(phys, set())))[:3])))
