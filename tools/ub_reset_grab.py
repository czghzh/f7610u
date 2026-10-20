#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ub_reset_grab.py —— U-Boot 里发 reset，然后持续喂回车抢回提示符（单次 open 串口）。

用途：U-Boot 的 tftp 反复失败后会把包缓冲池耗尽（`alloc bp failed`），
只有 reset 能清掉。reset 后 U-Boot 只有约 3 秒窗口，必须从 reset 那一刻就喂键。
"""
import os, sys, time, select, termios, tty

DEV = "/dev/ttyUSB0"
LIMIT = float(sys.argv[1]) if len(sys.argv) > 1 else 60.0

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
        r, _, _ = select.select([fd], [], [], 0.02)
        if r:
            try:
                d = os.read(fd, 4096)
            except (BlockingIOError, OSError):
                d = b""
            if d:
                buf += d
    return buf


read_for(0.3)
os.write(fd, b"reset\r")
print("[reset sent]")

buf = b""
last_key = 0.0
t0 = time.time()
got = False
while time.time() - t0 < LIMIT:
    r, _, _ = select.select([fd], [], [], 0.02)
    if r:
        try:
            d = os.read(fd, 4096)
        except (BlockingIOError, OSError):
            d = b""
        if d:
            buf += d
    if time.time() - last_key > 0.03:
        try:
            os.write(fd, b"\r")
        except OSError:
            pass
        last_key = time.time()
    tail = buf[-300:]
    if b"=> " in tail and b"Starting kernel" not in buf[-200:]:
        time.sleep(0.3)
        got = True
        break

os.close(fd)
print("=== GOT=%s ===" % got)
print(buf.decode("utf-8", "replace").replace("\r", "")[-800:])
