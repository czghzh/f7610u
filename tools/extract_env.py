#!/usr/bin/env python3
# extract_env.py — 提取原厂/我们的 env 全文（K=V 列表）+ 关键偏移
import os
os.chdir('/home/wen/WorkBuddy/zx/f7610u')

blk = open('backup/mtd1-uboot.bin','rb').read()[0xa0000:0xc0000]
print("== 原厂 env 全文 ==")
for p in blk[4:].split(b'\x00'):
    if p:
        print(" ", p.decode('utf-8','replace'))
print("  原厂头16B:", blk[:16].hex())

print()
our = open('work/env_our_block.bin','rb').read()
print("== 我们的 env 全文 ==")
for p in our[4:].split(b'\x00'):
    if p:
        print(" ", p.decode('utf-8','replace'))
print("  我们的头16B:", our[:16].hex())

print()
print("== 关注串在位 ===")
for key in [b'bootcmd=', b'run boota', b'0x88000240', b'bootzpon', b'preboot', b'cmdline']:
    io = blk.find(key)
    iu = our.find(key)
    print(f"  {key!r}: orig@{io if io>=0 else '-'} ours@{iu if iu>=0 else '-'}")
