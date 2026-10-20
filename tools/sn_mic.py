#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""sn_mic.py —— 按原厂 Drv_PloamuMic_fake 复算 Serial_Number_ONU 报文的 MIC。

正文 40B = 03 ff 01 00 | SN(8B) | rand(4B) | 24×00
CMAC    = AES-CMAC(key=0x55*16, 0x02 || 正文40)
奇偶调整：统计正文 40B 的 0 位数 zc；
         idx = 7 - ((zc>>3)&7)（MIC 字节下标）, bit = zc & 7；
         该位为 1 则清 0，为 0 则置 1。
寄存器：0x170598f8 = rev(MIC[0:4]), 0x170598fc = rev(MIC[4:8])
"""
import subprocess, sys

SN = bytes.fromhex("5A544547") + bytes.fromhex("0D09D561")  # "ZTEG" + 0x0D09D561
RAND = bytes(4)

body = bytearray(40)
body[0] = 0x03
body[1] = 0xFF
body[2] = 0x01
body[3] = 0x00
body[4:12] = SN
body[12:16] = RAND

data = bytes([0x02]) + bytes(body)
key = "55" * 16
out = subprocess.check_output([
    "openssl", "mac", "-macopt", "cipher:AES-128-CBC",
    "-macopt", "hexkey:" + key, "CMAC"],
    input=data,
)
mic = bytearray.fromhex(out.decode().strip())

zc = 0
for b in body:
    for i in range(8):
        if ((b >> i) & 1) == 0:
            zc += 1
idx = 7 - ((zc >> 3) & 7)
bit = zc & 7
print("zero_count=%d idx=%d bit=%d mic_before=%s" % (zc, idx, bit, mic.hex()))
if (mic[idx] >> bit) & 1:
    mic[idx] &= ~(1 << bit)
else:
    mic[idx] |= (1 << bit)
print("mic_final =", mic.hex())
w0 = int.from_bytes(mic[0:4], "little")
w1 = int.from_bytes(mic[4:8], "little")
rev = lambda v: int.from_bytes(v.to_bytes(4, "little"), "big")
print("0x170598f8 = 0x%08x" % rev(w0))
print("0x170598fc = 0x%08x" % rev(w1))
