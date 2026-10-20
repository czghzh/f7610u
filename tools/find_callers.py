#!/usr/bin/env python3
"""在 vmlinux 里扫 bl 指令，找指定函数的所有调用者 + 上下文"""
import struct, sys, re

BASE = 0xffffffc000080000
img = open("/home/wen/WorkBuddy/zx/f7610u/work/k1_vmlinux.bin", "rb").read()

def bl_callers(target, limit=0x2000000):
    out = []
    for off in range(0, min(len(img), limit), 4):
        w = struct.unpack_from("<I", img, off)[0]
        if (w & 0xFC000000) != 0x94000000:
            continue
        imm = w & 0x03FFFFFF
        if imm & 0x02000000:
            imm -= 0x04000000
        pc = BASE + off
        if pc + imm * 4 == target:
            out.append(pc)
    return out

targets = {
    "OpticalI2cRead":      0xffffffc000462548,
    "optical_i2c_register": 0xffffffc000462620,
    "CalcPonTxOpticalPower": 0xffffffc000462660,
    "CalcPonRxOpticalPower": 0xffffffc000462768,
    "GetOpticalTemperature": 0xffffffc0004627e0,
    "testOpticalI2c":       0xffffffc000462b50,
}
if len(sys.argv) > 1:
    targets = {sys.argv[1]: int(sys.argv[2], 16)}

for name, addr in targets.items():
    cs = bl_callers(addr)
    print(f"{name:24s} {addr:#x} <- {len(cs)} callers")
    for c in cs[:8]:
        print(f"    {c:#x}")
