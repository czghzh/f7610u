#!/usr/bin/env python3
"""Compare live TX registers against the vendor O5 snapshot."""
import json

o5 = json.load(open("/home/wen/WorkBuddy/zx/f7610u/work/o5_regs.json"))
live = {}
for line in open("/home/wen/WorkBuddy/zx/f7610u/work/live_tx_regs.txt"):
    p = line.strip().split()
    if len(p) == 3 and p[0].startswith("0x") and p[1] == "=":
        live[p[0]] = int(p[2], 16)

# registers we care about beyond the TX dump range, read from O5 snapshot only
watch = ["0x17059400", "0x17059404", "0x17059408", "0x1705940c",
         "0x17059410", "0x17059414", "0x17059418", "0x1705941c",
         "0x17059420", "0x17059424", "0x17059428", "0x1705942c",
         "0x17059430", "0x17059434", "0x17059438", "0x1705943c",
         "0x17059440", "0x17059444", "0x17059448", "0x1705944c",
         "0x17059450", "0x17059454", "0x17059458", "0x1705945c",
         "0x17059460", "0x17059464", "0x17059468", "0x1705946c",
         "0x17059470", "0x17059474", "0x17059478", "0x1705947c",
         "0x17059480", "0x17059484", "0x17059488", "0x1705948c",
         "0x17059490", "0x17059494", "0x17059498", "0x1705949c",
         "0x170594a0", "0x170594a4", "0x170594a8", "0x170594ac",
         "0x170594b0", "0x170594b4", "0x170594b8", "0x170594bc",
         "0x170594c0", "0x170594c4", "0x170594c8", "0x170594cc",
         "0x170594d0", "0x170594d4", "0x170594d8", "0x170594dc",
         "0x170594e0", "0x170594e4", "0x170594e8", "0x170594ec",
         "0x170594f0", "0x170594f4", "0x170594f8", "0x170594fc"]
print("== state/BWmap region (O5 snapshot) ==")
for a in watch:
    v = o5.get(a)
    if v is not None and v != 0:
        print(f"{a}: o5={hex(v)}")

print()
print("== full TX-region diff summary (live vs O5) ==")
n = 0
for a, v in sorted(live.items()):
    ov = o5.get(a)
    if ov is None:
        continue
    if ov != v:
        n += 1
print(f"diffs in 0x17059800-0x17059c00: {n} / {len(live)}")

# specifically: which diffs have live==0 but o5!=0 (missing values)
print()
print("== live==0 but o5!=0 (candidate missing writes) ==")
for a, v in sorted(live.items()):
    ov = o5.get(a)
    if ov is not None and v == 0 and ov != 0:
        print(f"{a}: live=0 o5={hex(ov)}")
