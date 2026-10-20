#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""probe_state.py —— 一次打开串口，敲回车判断当前停在 U-Boot 还是 shell。

只 open 一次（反复 open USB 串口会复位它，导致日志全空）。
用法: probe_state.py
退出码: 0=U-Boot 提示符, 1=Linux shell, 2=其它/无响应
"""
import os, sys, time, select, termios, tty

DEV = "/dev/ttyUSB0"


def open_port():
    fd = os.open(DEV, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    tty.setraw(fd)
    a = termios.tcgetattr(fd)
    a[2] |= termios.CLOCAL | termios.CREAD
    a[4] = a[5] = termios.B115200
    a[6][termios.VMIN] = 0
    a[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, a)
    return fd


fd = open_port()
buf = b""


def pump(t):
    global buf
    end = time.time() + t
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            try:
                d = os.read(fd, 4096)
            except OSError:
                d = b""
            if d:
                buf += d
                end = max(end, time.time() + 0.2)


# 先清掉缓冲区里的回声，再敲两次回车
pump(1.0)
buf = b""
os.write(fd, b"\r")
pump(2.0)
os.write(fd, b"\r")
pump(2.0)

txt = buf.decode("utf-8", "replace").replace("\r", "")
os.close(fd)

print("---- 串口最后 500 字节 ----")
print(txt[-500:])
print("--------------------------")

if "=>" in txt[-300:]:
    print("STATE: UBOOT")
    sys.exit(0)
if "#" in txt[-300:] or "$" in txt[-300:]:
    print("STATE: SHELL")
    sys.exit(1)
print("STATE: UNKNOWN")
sys.exit(2)
