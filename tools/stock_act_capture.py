#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""stock_act_capture.py —— 抖动链路后，按阶段完整抓原厂激活过程的寄存器。"""
import socket, time, re

ADDRS = [
    ("state", "0x17059400"),
    ("snctl", "0x170599bc"),
    ("snlo", "0x170598d8"),
    ("snhi", "0x170598dc"),
    ("miclo", "0x170598f8"),
    ("michi", "0x170598fc"),
    ("alloc1", "0x1705947c"),
    ("alloc2", "0x17059480"),
    ("spd", "0x17059408"),
    ("pw4c", "0x1705984c"),
    ("pw50", "0x17059850"),
    ("pw54", "0x17059854"),
    ("prof80", "0x17059880"),
    ("prof94", "0x17059894"),
    ("profa8", "0x170598a8"),
    ("profbc", "0x170598bc"),
    ("up", "0x17059800"),
    ("burst", "0x17059910"),
    ("xmac", "0x17058400"),
    ("txstat", "0x17059908"),
]

inner = " ".join(
    'printf "%s=" %s; /bin/devmem2 %s 2>/dev/null | grep -o "0x[0-9a-fA-F]*$"; printf " ";'
    % (name, a, a)
    for name, a in ADDRS
)

SCRIPT = (
    "i=0; while [ $i -lt 120 ]; do /bin/devmem2 0x17058400 w 0x63a >/dev/null 2>&1; "
    "i=$((i+1)); done; echo FLAPDONE; i=0; "
    "while [ $i -lt 40 ]; do echo \"S$i\"; " + inner + " echo; i=$((i+1)); done; echo POLLDONE"
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
end = time.time() + 70
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

txt = buf.decode("utf-8", "replace").replace("\r", "")
print(txt[-6000:])
