#!/usr/bin/env python3
"""Figure out what the kernel's cmac(aes) actually computes."""
import sys
sys.path.insert(0, "/home/wen/WorkBuddy/zx/f7610u/work")
from gpon_mic import expand, encrypt, dbl

KEY = bytes([0x55] * 16)
# known input: SN=TESTTEST, delay=0
MSG = bytes([0x02, 0x03, 0xff, 0x01, 0x00]) + b"TESTTEST" + bytes(4) + bytes(24)
TARGET = bytes.fromhex("d72464c876d7bc0c")   # kernel output (first 8 bytes)
print("msg:", MSG.hex(), "len", len(MSG))

rk = expand(KEY)
L = encrypt(rk, bytes(16))
K1 = dbl(L)
K2 = dbl(K1)

def cbc_mac(key, msg, pad=b"\x00"):
    rk = expand(key)
    m = msg + pad * ((16 - len(msg) % 16) % 16)
    if not m:
        m = bytes(16)
    x = bytes(16)
    for i in range(0, len(m), 16):
        x = encrypt(rk, bytes(a ^ b for a, b in zip(x, m[i:i+16])))
    return x

results = {}
# 1 standard cmac
def cmac_std(key, msg):
    rk = expand(key)
    l = encrypt(rk, bytes(16))
    k1 = dbl(l); k2 = dbl(k1)
    n = max(1, (len(msg) + 15) // 16)
    x = bytes(16)
    for i in range(n - 1):
        x = encrypt(rk, bytes(a ^ b for a, b in zip(x, msg[i*16:(i+1)*16])))
    last = msg[(n-1)*16:]
    if len(last) == 16:
        blk = bytes(a ^ b for a, b in zip(last, k1))
    else:
        blk = bytearray(last) + bytearray([0] * (16 - len(last)))
        blk[-1] = 0x80
        blk = bytes(a ^ b for a, b in zip(bytes(blk), k2))
    return encrypt(rk, bytes(a ^ b for a, b in zip(x, blk)))

results["cmac_std"] = cmac_std(KEY, MSG)
# 2 cbc-mac zero pad
results["cbc_zero"] = cbc_mac(KEY, MSG)
# 3 cbc-mac pad 0x80
results["cbc_80"] = cbc_mac(KEY, MSG, b"\x80")
# 4 cmac but last partial block XOR K1 instead of K2
def cmac_k1(key, msg):
    rk = expand(key)
    l = encrypt(rk, bytes(16)); k1 = dbl(l)
    n = max(1, (len(msg) + 15) // 16)
    x = bytes(16)
    for i in range(n - 1):
        x = encrypt(rk, bytes(a ^ b for a, b in zip(x, msg[i*16:(i+1)*16])))
    last = msg[(n-1)*16:]
    if len(last) == 16:
        blk = bytes(a ^ b for a, b in zip(last, k1))
    else:
        blk = bytearray(last) + bytearray([0] * (16 - len(last)))
        blk[-1] = 0x80
        blk = bytes(a ^ b for a, b in zip(bytes(blk), k1))
    return encrypt(rk, bytes(a ^ b for a, b in zip(x, blk)))
results["cmac_k1last"] = cmac_k1(KEY, MSG)
# 5 cmac without subkey on partial (just pad 0x80)
def cmac_nosub(key, msg):
    rk = expand(key)
    n = max(1, (len(msg) + 15) // 16)
    x = bytes(16)
    for i in range(n - 1):
        x = encrypt(rk, bytes(a ^ b for a, b in zip(x, msg[i*16:(i+1)*16])))
    last = msg[(n-1)*16:]
    if len(last) == 16:
        blk = last
    else:
        blk = bytes(bytearray(last) + bytearray([0] * (16 - len(last)))[:-1] + bytearray([0x80]))
    return encrypt(rk, bytes(a ^ b for a, b in zip(x, blk)))
results["cmac_nosub"] = cmac_nosub(KEY, MSG)
# 6 raw ecb first block
results["ecb_b0"] = encrypt(rk, MSG[:16])
# 7 cmac with double-02 prefix
results["cmac_2x02"] = cmac_std(KEY, b"\x02" + MSG)
# 8 cmac of body only (40 bytes)
results["cmac_body"] = cmac_std(KEY, MSG[1:])
# 9 reversed key
results["cmac_revkey"] = cmac_std(KEY[::-1], MSG)
# 10 reversed message
results["cmac_revmsg"] = cmac_std(KEY, MSG[::-1])

for name, v in results.items():
    mark = "  <== MATCH" if v[:8] == TARGET else ""
    print(f"{name:14s} {v[:8].hex()} (full {v.hex()}){mark}")
print("kernel target  ", TARGET.hex())
