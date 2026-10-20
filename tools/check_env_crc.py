#!/usr/bin/env python3
# check_env_crc.py — 验证 env 块 CRC（原厂 vs saveenv 后）
import re, zlib, os
os.chdir('/home/wen/WorkBuddy/zx/f7610u')

def crc_env(buf):
    stored = int.from_bytes(buf[:4], 'little')
    calc = zlib.crc32(buf[4:4+0x1FFFC]) & 0xffffffff
    return stored, calc

blk = open('backup/mtd1-uboot.bin','rb').read()[0xa0000:0xc0000]
s, c = crc_env(blk)
print(f"[原厂env] stored=0x{s:08x} calc=0x{c:08x} match={s==c}")

def parse(path):
    out = {}
    try:
        txt = open(path,'rb').read().decode('utf-8','replace')
    except FileNotFoundError:
        print(f"  (缺 {path})"); return out
    for line in txt.splitlines():
        m = re.match(r'\s*([0-9a-f]{8}): ((?:[0-9a-f]{2} ){0,15}[0-9a-f]{2})(?:\s)', line)
        if m:
            out[int(m.group(1),16)] = bytes(int(h,16) for h in m.group(2).split())
    return out

buf = bytearray(0x20000)
uniq = set()
for i in range(1,5):
    d = parse(f'work/s2_last_dump{i}.bin')
    n = sum(len(b) for b in d.values())
    print(f"  dump{i}: {len(d)} 行 / {n} 字节")
    for addr, b in d.items():
        off = addr - 0x8c000000
        if 0 <= off < 0x20000:
            buf[off:off+len(b)] = b
            uniq |= set(range(off, off+len(b)))
print(f"[新env] 覆盖 {len(uniq)} 字节 (期望 0x800)")
print(f"[新env] 头64B repr: {bytes(buf[:64])!r}")
s2, c2 = crc_env(bytes(buf))
print(f"[新env] stored=0x{s2:08x} calc=0x{c2:08x} match={s2==c2}")
