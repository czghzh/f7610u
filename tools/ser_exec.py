#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ser_exec.py "cmd1;cmd2" —— 串口登录(getty, root 空密码)后执行命令。单次 open。"""
import os, sys, time, select, termios, tty

DEV = "/dev/ttyUSB0"
CMDS = sys.argv[1] if len(sys.argv) > 1 else "id"

fd = os.open(DEV, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
tty.setraw(fd)
a = termios.tcgetattr(fd)
a[2] |= termios.CLOCAL | termios.CREAD
a[4] = a[5] = termios.B115200
a[6][termios.VMIN] = 0
a[6][termios.VTIME] = 0
termios.tcsetattr(fd, termios.TCSANOW, a)


def pump(t=1.0):
    out = b""
    end = time.time() + t
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            d = os.read(fd, 4096)
            if d:
                out += d
                end = max(end, time.time() + 0.15)
    return out


def send(s, wait=0.8):
    os.write(fd, s.encode() + b"\r")
    return pump(wait)


buf = pump(0.5)
if b"login" in buf.lower() or b"Password" in buf:
    buf += send("root", 1.0)
    if b"Password" in buf:
        buf += send("", 1.0)
else:
    buf += send("", 0.8)
    if b"login" in buf.lower():
        buf += send("root", 1.0)
        buf += send("", 1.0)

for c in CMDS.split(";"):
    c = c.strip()
    if c:
        buf += send(c, 1.2)
os.close(fd)
sys.stdout.write(buf.decode("utf-8", "replace").replace("\r", ""))
