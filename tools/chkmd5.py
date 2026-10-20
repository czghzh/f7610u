#!/usr/bin/env python3
# chkmd5.py — 用 r10 新 dump 重建 mtd2 头 512B，定位 md5 差异
import re, hashlib

WS = "/home/wen/WorkBuddy/zx/f7610u"
txt = open(f"{WS}/work/telnet_run_20261017-084836.txt").read()

# 从 r10 的三段 dump 重建 0x0-0x200
start = txt.index("hexdump -C -s 0 -n 128 /dev/mtd2")
seg = txt[start:start + 12000]
head = bytearray(0x200)
for line in seg.splitlines():
    m = re.match(r"^([0-9a-f]{8})  (.+?)  \|", line)
    if not m:
        continue
    off = int(m.group(1), 16)
    if off >= 0x200:
        continue
    hexes = re.findall(r"[0-9a-f]{2}", m.group(2))
    bs = bytes(int(h, 16) for h in hexes[:16])
    head[off:off+len(bs)] = bs

print("重建头 512B md5:", hashlib.md5(bytes(head)).hexdigest())
full = bytes(head) + b"\xff" * (0x20000 - 0x200)
print("重建全块(512+FF) md5:", hashlib.md5(full).hexdigest())
print("设备侧 m2_orig.bin md5: a7b7dd4a8e3306694eae0fab5804f3e2")
print()
print("全 FF 块 md5:", hashlib.md5(b"\xff" * 0x20000).hexdigest())
print("scratch(设备) md5: 41d2e2c0c0edfccf76fa1c3e38bc1cf2")
print()
print("头 512B hexdump:")
for i in range(0, 0x200, 16):
    row = head[i:i+16]
    print(f"{i:08x}  " + " ".join(f"{b:02x}" for b in row) + "  |" + "".join(chr(c) if 32 <= c < 127 else "." for c in row) + "|")
