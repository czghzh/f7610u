#!/usr/bin/env python3
# analyze2.py — 当前 flash env 块重建 + CRC 校验 + 三方 diff
import re, zlib, struct

WS = "/home/wen/WorkBuddy/zx/f7610u"
txt = open(f"{WS}/work/telnet_run_20261017-082339.txt").read()
start = txt.index("hexdump -C -s 655360 -n 2048 /dev/mtd1")
seg = txt[start:start + 8000]
buf = bytearray(0x800)
for m in re.finditer(r"^000a([0-9a-f]{4})  ((?:[0-9a-f]{2} ){1,16})", seg, re.M):
    off = int(m.group(1), 16)
    bs = bytes(int(h, 16) for h in m.group(2).split())
    buf[off:off + len(bs)] = bs

crc = zlib.crc32(buf[4:]) & 0xffffffff
stored = struct.unpack("<I", buf[:4])[0]
verdict = "MATCH" if crc == stored else "MISMATCH"
print(f"重建块 CRC32(计算)={crc:#x}  头部存储={stored:#x}  {verdict}")

i = buf.find(b"bootcmd="); j = buf.find(b"\x00", i)
print("当前bootcmd:", buf[i:j].decode())
i = buf.find(b"versioninfo="); j = buf.find(b"\x00", i)
print("当前versioninfo:", buf[i:j].decode())

data = open(f"{WS}/backup/mtd1-uboot.bin", "rb").read()
d = data[0xa0000:0xa0800]
i = d.find(b"bootcmd="); j = d.find(b"\x00", i)
print("默认env bootcmd:", d[i:j].decode())
i = d.find(b"versioninfo="); j = d.find(b"\x00", i)
print("默认versioninfo:", d[i:j].decode())

def vars_of(b):
    out = {}
    for v in bytes(b[4:0x800]).split(b"\x00"):
        if b"=" in v:
            k, _, val = v.partition(b"=")
            out[k.decode(errors="replace")] = val.decode(errors="replace")
    return out

cur = vars_of(buf)
our = vars_of(open(f"{WS}/work/env_our_block.bin", "rb").read())
old = vars_of(open(f"{WS}/work/env_old_block.bin", "rb").read())
print()
print("== 变量级 diff: 当前 vs 我们写进去的 ==")
for k in sorted(set(cur) | set(our)):
    a, b2 = our.get(k), cur.get(k)
    if a != b2:
        print(f"  [{k}]")
        print(f"    我们: {a!r}")
        print(f"    当前: {b2!r}")
print()
print("== 变量级 diff: 当前 vs 原厂 ==")
for k in sorted(set(cur) | set(old)):
    a, b2 = old.get(k), cur.get(k)
    if a != b2:
        print(f"  [{k}]")
        print(f"    原厂: {a!r}")
        print(f"    当前: {b2!r}")
