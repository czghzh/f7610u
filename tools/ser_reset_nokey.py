#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ser_reset_nokey.py —— 在 U-Boot 里发 reset，然后静默等待，不喂任何键。

用途：中断过 autoboot 的 U-Boot 不会把 flash 内核加载到 0x88000240，
手动 bootm 会失败。只有让 U-Boot 自己走完 autoboot，才会进原厂固件。
"""
import os, sys, time, select, termios, tty

DEV = "/dev/ttyUSB0"
WAIT = float(sys.argv[1]) if len(sys.argv) > 1 else 90.0

fd = os.open(DEV, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
tty.setraw(fd)
a = termios.tcgetattr(fd)
a[2] |= termios.CLOCAL | termios.CREAD
a[4] = a[5] = termios.B115200
a[6][termios.VMIN] = 0
a[6][termios.VTIME] = 0
termios.tcsetattr(fd, termios.TCSANOW, a)

time.sleep(0.2)
os.write(fd, b"reset\r")
print("[reset sent, staying silent]")

buf = b""
t0 = time.time()
while time.time() - t0 < WAIT:
    r, _, _ = select.select([fd], [], [], 0.2)
    if r:
        try:
            d = os.read(fd, 8192)
        except (BlockingIOError, OSError):
            d = b""
        if d:
            buf += d
os.close(fd)
print("captured %d bytes" % len(buf))
tail = buf.decode("utf-8", "replace").replace("\r", "")
print(tail[-1200:])
