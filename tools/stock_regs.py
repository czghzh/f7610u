#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""stock_regs.py [addr...] —— 原厂固件(telnet root/Zte521)一次登录内批量读 PON 寄存器。

默认地址集覆盖：激活态/SN/使能/MIC/alloc/profile/上行参数。
用法: stock_regs.py            # 默认集
      stock_regs.py 0x17059400 0x170599bc ...
"""
import socket, sys, time, re

DEFAULT = [
    "0x17059400", "0x170599bc", "0x170598d8", "0x170598dc",
    "0x170598f8", "0x170598fc", "0x1705947c", "0x17059480",
    "0x17059800", "0x17059910", "0x17059844", "0x1705984c",
    "0x17059850", "0x17059880", "0x17059894", "0x170598a8",
    "0x170598bc", "0x17059988", "0x17059998", "0x170599a4",
    "0x17058400", "0x1705842c", "0x17058430", "0x17058514",
]

addrs = sys.argv[1:] or DEFAULT
loop = "; ".join(
    'printf "%s=" %s; /bin/devmem2 %s 2>/dev/null | grep "Value at" | awk "{print \\$NF}"'
    % (a, a, a)
    for a in addrs
)

s = socket.socket()
s.settimeout(8)
s.connect(("192.168.1.1", 23))
time.sleep(0.7)


def drain(t=1.0):
    s.settimeout(t)
    out = b""
    try:
        while True:
            d = s.recv(4096)
            if not d:
                break
            out += d
    except Exception:
        pass
    return out


b = drain(1.2)
if b"ogin" in b:
    s.sendall(b"root\r\n")
    time.sleep(0.6)
    b += drain(0.8)
if b"assword" in b:
    s.sendall(b"Zte521\r\n")
    time.sleep(1.0)
    b += drain(1.2)

s.sendall((loop + "\r\n").encode())
time.sleep(0.4)
b = drain(3.0)
try:
    s.sendall(b"exit\r\n")
except Exception:
    pass

txt = b.decode("utf-8", "replace").replace("\r", "")
pairs = re.findall(r"(0x[0-9a-fA-F]{8})=0x([0-9a-fA-F]+)", txt)
seen = {}
for a, v in pairs:
    seen[a.lower()] = v
for a in addrs:
    print("%s = %s" % (a, ("0x" + seen[a.lower()]) if a.lower() in seen else "?"))
