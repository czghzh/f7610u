#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ser_log.py <outfile> —— 单次打开串口，持续把原始输出追加到文件。"""
import os, sys, time, select, termios, tty

DEV = "/dev/ttyUSB0"
out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/ser.log"

fd = os.open(DEV, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
tty.setraw(fd)
a = termios.tcgetattr(fd)
a[2] |= termios.CLOCAL | termios.CREAD
a[4] = a[5] = termios.B115200
a[6][termios.VMIN] = 0
a[6][termios.VTIME] = 0
termios.tcsetattr(fd, termios.TCSANOW, a)

f = open(out, "ab", buffering=0)
try:
    while True:
        r, _, _ = select.select([fd], [], [], 0.5)
        if r:
            try:
                d = os.read(fd, 4096)
            except OSError:
                d = b""
            if d:
                f.write(d)
finally:
    f.close()
    os.close(fd)
