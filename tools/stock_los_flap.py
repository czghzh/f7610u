#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""stock_los_flap.py —— 高频反复清 0x17058400 bit0 制造链路抖动，停手后抓激活迁移。"""
import socket, time, re

SCRIPT = (
    "i=0; while [ $i -lt 120 ]; do /bin/devmem2 0x17058400 w 0x63a >/dev/null 2>&1; "
    "i=$((i+1)); done; echo FLAPDONE; i=0; "
    "while [ $i -lt 120 ]; do "
    "A=$(/bin/devmem2 0x17059400 2>/dev/null | grep -o '0x[0-9a-fA-F]*$'); "
    "B=$(/bin/devmem2 0x170599bc 2>/dev/null | grep -o '0x[0-9a-fA-F]*$'); "
    "C=$(/bin/devmem2 0x170598f8 2>/dev/null | grep -o '0x[0-9a-fA-F]*$'); "
    "echo \"S $A $B $C\"; i=$((i+1)); done; echo POLLDONE"
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
    time.sleep(0.5)
    b += drain(0.6)
if b"assword" in b:
    s.sendall(b"Zte521\r\n")
    time.sleep(0.8)
    b += drain(1.0)

s.sendall((SCRIPT + "\r\n").encode())
buf = b""
end = time.time() + 45
while time.time() < end:
    d = drain(0.5)
    if d:
        buf += d
        if b"POLLDONE" in buf:
            break
try:
    s.sendall(b"exit\r\n")
except Exception:
    pass

txt = buf.decode("utf-8", "replace").replace("\r", "")
rows = re.findall(r"S (0x[0-9a-fA-F]+) (0x[0-9a-fA-F]+) (0x[0-9a-fA-F]+)", txt)
print("samples:", len(rows))
prev = None
for st, ctrl, mic in rows:
    if (st, ctrl) != prev:
        print("  state=%s sn_ctrl=%s mic=%s" % (st, ctrl, mic))
        prev = (st, ctrl)
