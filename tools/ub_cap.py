#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ub_cap.py "<命令>" <秒数> <输出文件>
在 U-Boot 提示符下发一条命令，然后持续抓串口。全程只 open 一次。"""
import os, sys, time, select, termios, tty

DEV = "/dev/ttyUSB0"
cmd = sys.argv[1] if len(sys.argv) > 1 else "boot"
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 90.0
out = sys.argv[3] if len(sys.argv) > 3 else "/tmp/ub_cap.bin"

fd = os.open(DEV, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
tty.setraw(fd)
a = termios.tcgetattr(fd)
a[2] |= termios.CLOCAL | termios.CREAD
a[4] = a[5] = termios.B115200
a[6][termios.VMIN] = 0
a[6][termios.VTIME] = 0
termios.tcsetattr(fd, termios.TCSANOW, a)

# 先把缓冲里的东西读掉
end = time.time() + 1.0
while time.time() < end:
    r, _, _ = select.select([fd], [], [], 0.05)
    if r:
        try:
            os.read(fd, 4096)
        except OSError:
            pass

os.write(fd, (cmd + "\r").encode())
log = open(out, "wb")
t0 = time.time()
nbytes = 0
while time.time() - t0 < secs:
    r, _, _ = select.select([fd], [], [], 0.2)
    if r:
        try:
            d = os.read(fd, 8192)
        except OSError:
            d = b""
        if d:
            log.write(d)
            log.flush()
            nbytes += len(d)
log.close()
os.close(fd)
print("captured %d bytes -> %s" % (nbytes, out))
