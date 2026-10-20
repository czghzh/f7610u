#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ub_tftp.py <itb> —— U-Boot 里发 tftp 并等 60 秒，打印全部串口输出（诊断用）。"""
import os, sys, time, select, termios, tty

DEV = "/dev/ttyUSB0"
ITB = sys.argv[1] if len(sys.argv) > 1 else "f7610u-v9.32-gponsync.itb"

fd = os.open(DEV, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
tty.setraw(fd)
a = termios.tcgetattr(fd)
a[2] |= termios.CLOCAL | termios.CREAD
a[4] = a[5] = termios.B115200
a[6][termios.VMIN] = 0
a[6][termios.VTIME] = 0
termios.tcsetattr(fd, termios.TCSANOW, a)


def read_for(t):
    buf = b""
    end = time.time() + t
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            try:
                d = os.read(fd, 4096)
            except (BlockingIOError, OSError):
                d = b""
            if d:
                buf += d
    return buf


def cmd(s, wait=1.5):
    os.write(fd, s.encode() + b"\r")
    return read_for(wait)


read_for(1.0)
print("--- ping check ---")
print(cmd("printenv ipaddr; printenv serverip", 2.0).decode("utf-8", "replace").replace("\r", ""))
print("--- tftp ---")
os.write(fd, ("tftp 0x88000000 %s" % ITB).encode() + b"\r")
out = read_for(75)
print(out.decode("utf-8", "replace").replace("\r", ""))
os.close(fd)
