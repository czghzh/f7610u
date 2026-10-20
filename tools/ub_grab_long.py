#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ub_grab_long.py <seconds> —— 长时间持续喂回车抢 U-Boot 提示符（单次 open 串口）。

用于冷启动配合：设备断电再上电时，从最低层就开始喂键，
确保 3 秒的 bootdelay 窗口不会漏掉。抢到后打印并退出。
日志写到 /home/wen/WorkBuddy/zx/f7610u/work/ub_grab_long.log
"""
import os, sys, time, select, termios, tty

DEV = "/dev/ttyUSB0"
LIMIT = float(sys.argv[1]) if len(sys.argv) > 1 else 240.0
LOG = "/home/wen/WorkBuddy/zx/f7610u/work/ub_grab_long.log"

fd = os.open(DEV, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
tty.setraw(fd)
a = termios.tcgetattr(fd)
a[2] |= termios.CLOCAL | termios.CREAD
a[4] = a[5] = termios.B115200
a[6][termios.VMIN] = 0
a[6][termios.VTIME] = 0
termios.tcsetattr(fd, termios.TCSANOW, a)

logf = open(LOG, "wb", buffering=0)
buf = b""
last_key = 0.0
t0 = time.time()
got = False

while time.time() - t0 < LIMIT:
    r, _, _ = select.select([fd], [], [], 0.02)
    if r:
        try:
            d = os.read(fd, 8192)
        except (BlockingIOError, OSError):
            d = b""
        if d:
            logf.write(d)
            buf += d
    # 持续喂回车
    if time.time() - last_key > 0.03:
        try:
            os.write(fd, b"\r")
        except OSError:
            pass
        last_key = time.time()
    tail = buf[-400:]
    if b"=> " in tail and b"Starting kernel" not in buf[-300:]:
        time.sleep(0.3)
        got = True
        break

logf.close()
os.close(fd)
print("GOT=%s elapsed=%.1fs bytes=%d" % (got, time.time() - t0, len(buf)))
if got:
    print("PROMPT_OK")
