#!/usr/bin/env python3
# chkmd5b.py — 用 -v dump 定版 mtd2 头，做全链 md5 对账
import re, hashlib

WS = "/home/wen/WorkBuddy/zx/f7610u"
txt = open(f"{WS}/work/telnet_run_20261017-085230.txt").read()
start = txt.index("hexdump -C -v -n 512 /dev/mtd2")
seg = txt[start:start + 6000]

head = bytearray(b"\xff" * 0x200)
n = 0
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
    n += 1
print("解析行数:", n)

h_md5 = hashlib.md5(bytes(head)).hexdigest()
print("头部 512B md5:", h_md5, "  <-> 设备 c1.bin: b37bf7354c7952479b528eba9a581619",
      "✓" if h_md5 == "b37bf7354c7952479b528eba9a581619" else "✗")

full = bytes(head) + b"\xff" * (0x20000 - 0x200)
f_md5 = hashlib.md5(full).hexdigest()
print("全块 md5:", f_md5, "  <-> 设备 m2_orig.bin: a7b7dd4a8e3306694eae0fab5804f3e2",
      "✓" if f_md5 == "a7b7dd4a8e3306694eae0fab5804f3e2" else "✗")

open(f"{WS}/httpdrop/mtd2_cur_128k.bin", "wb").write(full)

# BootImageNum=1
assert head[0x16] == 0x30
head2 = bytearray(head); head2[0x16] = 0x31
full2 = bytes(head2) + b"\xff" * (0x20000 - 0x200)
open(f"{WS}/httpdrop/mtd2_new_128k.bin", "wb").write(full2)
print("新块 md5:", hashlib.md5(full2).hexdigest())
print("新块头 512B md5:", hashlib.md5(bytes(head2)).hexdigest())
print("新头前 32B:", bytes(head2[:32]))
