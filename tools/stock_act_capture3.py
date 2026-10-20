#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""stock_act_capture3.py —— 抖动链路(带校验) + 全寄存器集采样，抓 O1→O5 全过程。"""
import socket, time

ADDRS = [
    ("state", "0x17059400"),
    ("snctl", "0x170599bc"),
    ("snlo", "0x170598d8"),
    ("snhi", "0x170598dc"),
    ("miclo", "0x170598f8"),
    ("alloc1", "0x1705947c"),
    ("pw54", "0x17059854"),
    ("prof80", "0x17059880"),
]

inner = " ".join(
    'printf "%%s=" %s; /bin/devmem2 %s 2>/dev/null | grep -o "0x[0-9a-fA-F]*$"; printf " ";'
    % (name, a)
    for name, a in ADDRS
)

SCRIPT = (
    "n=0; while [ $n -lt 6 ]; do "
    "i=0; while [ $i -lt 60 ]; do /bin/devmem2 0x17058400 w 0x63a >/dev/null 2>&1; i=$((i+1)); done; "
    "A=$(/bin/devmem2 0x17059400 2>/dev/null | grep -o '0x[0-9a-fA-F]*$'); echo \"CHK $A\"; "
    "case \"$A\" in 0x3ff0001|0x3ff0002|0x3ff0003) break;; esac; "
    "n=$((n+1)); done; echo FLAPDONE; i=0; "
    "while [ $i -lt 60 ]; do " + inner + " echo; i=$((i+1)); done; echo POLLDONE"
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
end = time.time() + 100
while time.time() < end:
    d = drain(0.6)
    if d:
        buf += d
        if b"POLLDONE" in buf:
            break
try:
    s.sendall(b"exit\r\n")
except Exception:
    pass

print(buf.decode("utf-8", "replace").replace("\r", "")[-14000:])
