#!/usr/bin/env python3
# analyze2b.py — 当前 flash env 块重建 v2（全文件解析）+ CRC + diff
import re, zlib, struct

WS = "/home/wen/WorkBuddy/zx/f7610u"
txt = open(f"{WS}/work/telnet_run_20261017-082339.txt").read()

# 调试信息
print("marker count:", txt.count("hexdump -C -s 655360"))
print("total ^000a lines:", len(re.findall(r"(?m)^000a", txt)))

ms = list(re.finditer(r"(?m)^000a([0-9a-f]{4})  (.+?)  \|", txt))
print("regex matches:", len(ms))
buf = bytearray(0x800)
n = 0
for m in ms:
    off = int(m.group(1), 16)
    hexes = re.findall(r"[0-9a-f]{2}", m.group(2))
    bs = bytes(int(h, 16) for h in hexes)
    if off + len(bs) <= 0x800 and off < 0x800:
        buf[off:off + len(bs)] = bs
        n += 1
print("applied lines:", n)

crc = zlib.crc32(b"\x00" * 4 + buf[4:]) & 0xffffffff
crc2 = zlib.crc32(buf[4:]) & 0xffffffff
stored = struct.unpack("<I", buf[:4])[0]
print(f"stored={stored:#x} crc(buf[4:])={crc2:#x}")

i = buf.find(b"bootcmd="); j = buf.find(b"\x00", i)
print("当前bootcmd:", buf[i:j].decode() if i >= 0 else "(NOT FOUND)")
i = buf.find(b"versioninfo="); j = buf.find(b"\x00", i)
print("当前versioninfo:", buf[i:j].decode() if i >= 0 else "(NOT FOUND)")
open(f"{WS}/work/env_cur_block.bin", "wb").write(bytes(buf))
print("saved env_cur_block.bin (0x800)")

def vars_of(b):
    out = {}
    for v in bytes(b[4:0x800]).split(b"\x00"):
        if b"=" in v:
            k, _, val = v.partition(b"=")
            out[k.decode(errors="replace")] = val.decode(errors="replace")
    return out

cur = vars_of(buf)
old = vars_of(open(f"{WS}/work/env_old_block.bin", "rb").read())
our = vars_of(open(f"{WS}/work/env_our_block.bin", "rb").read())
print()
print("== 变量 diff: 原厂 -> 当前 ==")
for k in sorted(set(cur) | set(old)):
    a, b2 = old.get(k), cur.get(k)
    if a != b2:
        print(f"  [{k}] 原厂={a!r} -> 当前={b2!r}")
print()
print("== 变量 diff: 我们当时写的 -> 当前 ==")
for k in sorted(set(cur) | set(our)):
    a, b2 = our.get(k), cur.get(k)
    if a != b2:
        print(f"  [{k}] 我们={a!r} -> 当前={b2!r}")
