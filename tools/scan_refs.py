#!/usr/bin/env python3
# scan_refs.py — 在 uboot_full.dis 中找目标字符串地址的 adrp+add 引用
# 用法: python3 work/scan_refs.py 0x8403e418=RSA_OK 0x8403e428=VM_OK ...
import re, sys
targets = {}
for arg in sys.argv[1:]:
    a, n = arg.split('=', 1)
    targets[int(a, 16)] = n
lines = open('/home/wen/WorkBuddy/zx/f7610u/work/uboot_full.dis', encoding='utf-8', errors='replace').read().splitlines()
adrp_re = re.compile(r'^\s*([0-9a-f]+):\s+[0-9a-f]+\s+adrp\s+(x\d+),\s+0x([0-9a-f]+)')
hits = []
for i, line in enumerate(lines):
    m = adrp_re.match(line)
    if not m:
        continue
    addr, reg, page = int(m.group(1), 16), m.group(2), int(m.group(3), 16)
    for j in range(i + 1, min(i + 5, len(lines))):
        m2 = re.match(r'^\s*[0-9a-f]+:\s+[0-9a-f]+\s+add\s+' + reg + r',\s+' + reg + r',\s+#0x([0-9a-f]+)', lines[j])
        if m2:
            target = page + int(m2.group(1), 16)
            if target in targets:
                hits.append((addr, target, targets[target]))
            break
        if 'adrp' in lines[j]:
            break
for addr, target, name in hits:
    print(f"code@0x{addr:08x} -> 0x{target:08x} : {name}")
