#!/usr/bin/env python3
"""按 zx279131-pon.c gpon_build_sn_frame() 构造 48B 上行 SN 帧 (msgid 01)
   frame = 03 ff 01 00 | SN(8) | delay(4,大端) | 24x00 ; MIC@40 = CMAC(0x55*16, 02||frame[0:40])
"""
import subprocess, sys

SN = bytes.fromhex("534b5957a8d40ba7")          # SKYW a8d40ba7
delay = int(sys.argv[1], 0) if len(sys.argv) > 1 else 0x0001cde3

f = bytearray(48)
f[0], f[1], f[2], f[3] = 0x03, 0xff, 0x01, 0x00
f[4:12] = SN
f[12:16] = delay.to_bytes(4, "big")

data = bytes([0x02]) + bytes(f[0:40])
out = subprocess.check_output(
    ["openssl", "mac", "-macopt", "cipher:AES-128-CBC",
     "-macopt", "hexkey:" + "55" * 16, "CMAC"], input=data)
f[40:48] = bytes.fromhex(out.decode().strip())

print("delay=0x%08x" % delay)
print("frame=" + f.hex())
print("word0=0x" + f[0:4].hex() + " word1=0x" + f[4:8].hex() + " mic=0x" + f[40:48].hex())
