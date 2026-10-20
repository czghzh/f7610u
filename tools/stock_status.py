#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""stock_status.py —— telnet 进原厂固件(root/Zte521)跑只读状态命令。"""
import socket, sys, time

CMDS = sys.argv[1:] or ["cat /proc/uptime", "ifconfig -a", "cat /proc/net/dev"]
s = socket.socket()
s.settimeout(6)
s.connect(("192.168.1.1", 23))
time.sleep(0.6)


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
for c in CMDS:
    s.sendall((c + "\r\n").encode())
    time.sleep(1.2)
    b += drain(2.0)
try:
    s.sendall(b"exit\r\n")
except Exception:
    pass
txt = b.decode("utf-8", "replace").replace("\r", "")
# 只打印命令输出段（跳过登录横幅）
lines = txt.split("\n")
print(txt[-4000:])
