#!/usr/bin/env python3
# mkmtd2.py — 重建 mtd2 当前 128KB 块（校验 md5）并生成 BootImageNum=1 版本
import re, hashlib

WS = "/home/wen/WorkBuddy/zx/f7610u"
txt = open(f"{WS}/work/telnet_run_20261017-082339.txt").read()
# r6: 第一段 hexdump 0x0-0x200（mtd2）
start = txt.index("hexdump -C -n 512 /dev/mtd2")
seg = txt[start:start + 4000]

buf = bytearray(b"\xff" * 0x20000)
for m in re.finditer(r"^000000([0-9a-f]{2})  ((?:[0-9a-f]{2} ){1,8})(?: )?(?:[0-9a-f ]*?)  \|", seg, re.M):
    pass
# 更稳妥：逐行解析 "OFFS  b0 b1 ...  |ascii|"
for line in seg.splitlines():
    m = re.match(r"^([0-9a-f]{8})  (.+?)  \|", line)
    if not m:
        continue
    off = int(m.group(1), 16)
    hexes = re.findall(r"[0-9a-f]{2}", m.group(2))
    bs = bytes(int(h, 16) for h in hexes)
    # hexdump 行 16 字节（中间双空格）
    if len(bs) >= 16:
        buf[off:off+16] = bs[:16]
    else:
        buf[off:off+len(bs)] = bs

m_cur = hashlib.md5(bytes(buf)).hexdigest()
print("重建 mtd2 当前块 md5:", m_cur)
print("设备侧 mtd_debug md5:  a7b7dd4a8e3306694eae0fab5804f3e2")
print("一致!" if m_cur == "a7b7dd4a8e3306694eae0fab5804f3e2" else "*** 不一致，需排查 ***")
open(f"{WS}/httpdrop/mtd2_cur_128k.bin", "wb").write(bytes(buf))

# BootImageNum 值最后一位 '0'(0x30)->'1'(0x31)，位于偏移 0x16
assert buf[0x16] == 0x30, f"byte16={buf[0x16]:#x} 预期 0x30"
buf2 = bytearray(buf)
buf2[0x16] = 0x31
m_new = hashlib.md5(bytes(buf2)).hexdigest()
print("BootImageNum=1 版 md5:", m_new)
open(f"{WS}/httpdrop/mtd2_new_128k.bin", "wb").write(bytes(buf2))
print("前 32 字节（新）:", bytes(buf2[:32]))
# 供设备端校验用的 0x400 头两段 md5
print("新块头 0x400 md5:", hashlib.md5(bytes(buf2[:0x400])).hexdigest())
print("原块头 0x400 md5:", hashlib.md5(bytes(buf[:0x400])).hexdigest())
