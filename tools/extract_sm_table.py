#!/usr/bin/env python3
"""Extract the p_operation_state_machine handler table from zte_xgpon.ko.

Table: .rodata + 0xB28, 7 rows (state) x 29 entries (event), 8 bytes each.
Each entry is a relocated function pointer in .rela.rodata.
"""
import re
import subprocess

KO = "/home/wen/WorkBuddy/zx/f7610u/tftp/zte_xgpon.ko"

out = subprocess.run(["readelf", "-r", KO], capture_output=True, text=True).stdout
# relocation lines: offset  info  type  symval  symname + addend
pat = re.compile(r"^([0-9a-f]{16})\s+\S+\s+R_AARCH64_ABS64\s+([0-9a-f]+)\s+(\S+)")
reloc = {}
for line in out.splitlines():
    m = pat.match(line)
    if m:
        off = int(m.group(1), 16)
        sym = m.group(3)
        addend = int(m.group(2), 16)
        reloc[off] = (sym, addend)

BASE = 0xB68
ROWS, COLS = 7, 29
print("state row: event -> handler (non-null only)")
for st in range(ROWS):
    handlers = []
    for ev in range(COLS):
        off = BASE + (st * COLS + ev) * 8
        r = reloc.get(off)
        if r:
            sym, add = r
            name = sym.split("+")[0].strip()
            if add:
                name = f"{name}+{add:#x}"
            handlers.append((ev, name))
    if handlers:
        print(f"--- state {st} ---")
        for ev, name in handlers:
            print(f"   ev{ev:2d}: {name}")
