#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ser_read.py <seconds> —— 单次打开串口被动读日志（不喂键，不干扰设备）。"""
import os, sys, time, select, termios, tty

DEV = "/dev/ttyUSB0"
SEC = float(sys.argv[1]) if len(sys.argv) > 1 else 5.0

fd = os.open(DEV, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
tty.setraw(fd)
a = termios.tcgetattr(fd)
a[2] |= termios.CLOCAL | termios.CREAD
a[4] = a[5] = termios.B115200
a[6][termios.VMIN] = 0
a[6][termios.VTIME] = 0
termios.tcsetattr(fd, termios.TCSANOW, a)

buf = b""
end = time.time() + SEC
while time.time() < end:
    r, _, _ = select.select([fd], [], [], 0.1)
    if r:
        try:
            d = os.read(fd, 4096)
        except OSError:
            d = b""
        if d:
            buf += d
            end = time.time() + 0.5
os.close(fd)
sys.stdout.write(buf.decode("utf-8", "replace").replace("\r", ""))
