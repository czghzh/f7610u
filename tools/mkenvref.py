#!/usr/bin/env python3
# mkenvref.py — 生成 env 块参照值（原厂 / 我们的） + bootcmd 串出现位置
import re, os, hashlib
os.chdir('/home/wen/WorkBuddy/zx/f7610u')
def md5b(b): return hashlib.md5(b).hexdigest()

ub = open('backup/mtd1-uboot.bin','rb').read()
old = ub[0xa0000:0xc0000]
open('work/env_old_block.bin','wb').write(old)
print("OLD env block:")
print("  full128K md5:", md5b(old))
print("  head1K  md5:", md5b(old[:1024]))
print("  head64  md5:", md5b(old[:64]))
print("  head64 hex:", old[:64].hex())

def parse(path):
    out = {}
    try: txt = open(path,'rb').read().decode('utf-8','replace')
    except FileNotFoundError: return out
    for line in txt.splitlines():
        m = re.match(r'\s*([0-9a-f]{8}): ((?:[0-9a-f]{2} ){0,15}[0-9a-f]{2})(?:\s)', line)
        if m: out[int(m.group(1),16)] = bytes(int(h,16) for h in m.group(2).split())
    return out
buf = bytearray(0x20000)
for i in range(1,5):
    for addr,b in parse(f'work/s2_last_dump{i}.bin').items():
        off = addr-0x8c000000
        if 0 <= off < 0x20000: buf[off:off+len(b)] = b
our = bytes(buf)
open('work/env_our_block.bin','wb').write(our)
print("OUR env block (重建):")
print("  full128K md5:", md5b(our))
print("  head1K  md5:", md5b(our[:1024]))
print("  head64  md5:", md5b(our[:64]))
print("  head64 hex:", our[:64].hex())

print("bootcmd 在 uboot bin 里出现位置:")
for m in re.finditer(b'bootcmd', ub):
    o = m.start()
    print(f"  0x{o:x}: ctx={ub[o-6:o+20]!r}")
