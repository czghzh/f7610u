#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""uboot_load.py <itb> —— 已在 U-Boot 提示符时：setenv → tftp → pon_reset → bootm。
全程只打开串口一次（避免 USB 串口反复复位导致丢日志）。"""
import os, sys, time, select, termios, tty

DEV = "/dev/ttyUSB0"
ITB = sys.argv[1] if len(sys.argv) > 1 else "f7610u-v9.20-ponreg.itb"
BOOTARGS = ("console=ttyAMA0,115200n8 earlycon=zteuart,mmio32,0x10d0d000 "
            "root=/dev/mtdblock7 ro rootwait")


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


class Ser:
    def __init__(self):
        self.fd = open_port()
        self.buf = b""

    def pump(self, t):
        end = time.time() + t
        while time.time() < end:
            r, _, _ = select.select([self.fd], [], [], 0.03)
            if r:
                try:
                    d = os.read(self.fd, 4096)
                except (BlockingIOError, OSError):
                    d = b""
                if d:
                    self.buf += d
                    end = max(end, time.time() + 0.15)

    def send(self, s):
        os.write(self.fd, s.encode() + b"\r")

    def cmd(self, s, wait=1.2, quiet=False):
        self.buf = b""
        self.send(s)
        self.pump(wait)
        out = self.buf.decode("utf-8", "replace").replace("\r", "")
        if not quiet:
            print("$ %s\n%s" % (s, out.strip()[-600:]))
        return out

    def wait_for(self, pats, timeout):
        end = time.time() + timeout
        self.buf = b""
        while time.time() < end:
            r, _, _ = select.select([self.fd], [], [], 0.05)
            if r:
                try:
                    d = os.read(self.fd, 4096)
                except (BlockingIOError, OSError):
                    d = b""
                if d:
                    self.buf += d
            txt = self.buf.decode("utf-8", "replace")
            for p in pats:
                if p in txt:
                    return p
        return None


s = Ser()
print("=== 1) 确认提示符 ===")
ok = False
for _ in range(5):
    s.buf = b""
    s.send("")
    s.pump(0.8)
    if b"=>" in s.buf[-400:]:
        ok = True
        break
if not ok:
    print("!! 不在 U-Boot 提示符:", s.buf[-200:])
    sys.exit(1)

print("=== 2) setenv bootargs ===")
s.cmd("setenv bootargs %s" % BOOTARGS, 1.0, quiet=True)
s.cmd("printenv bootargs", 0.8)

print("=== 3) tftp %s ===" % ITB)
ok = False
for attempt in range(1, 5):
    s.buf = b""
    s.send("tftp 0x88000000 %s" % ITB)
    hit = s.wait_for(["Bytes transferred", "Retry count exceeded", "ERROR", "***"], 90)
    print("tftp#%d -> %s" % (attempt, hit))
    if hit == "Bytes transferred":
        ok = True
        break
    time.sleep(2)
txt = s.buf.decode("utf-8", "replace").replace("\r", "")
print("\n".join(l for l in txt.split("\n") if "Bytes transferred" in l or "ERROR" in l)[-300:])
if not ok:
    print("!! tftp 连续失败，停止（不 bootm）")
    os.close(s.fd)
    sys.exit(4)

print("=== 4) pon_reset 脉冲 ===")
s.cmd("mw 0x1a2c0004 0", 0.3, quiet=True)
s.cmd("mw 0x17240004 0", 0.3, quiet=True)
s.cmd("mw 0x1a2c0004 0xffffffff", 0.3, quiet=True)
s.cmd("mw 0x17240004 0xffffffff", 0.3, quiet=True)
time.sleep(0.3)
s.cmd("mw 0x1a000008 0xffffffff", 0.3, quiet=True)
s.cmd("mw 0x1a00000c 0xffffffff", 0.3, quiet=True)

print("=== 5) bootm ===")
s.send("bootm")
hit = s.wait_for(["ImmortalWrt", "Kernel panic", "Unsupported", "Starting kernel"], 60)
print("bootm ->", hit)
s.pump(6.0)
txt = s.buf.decode("utf-8", "replace").replace("\r", "")
print("\n".join(l for l in txt.split("\n") if l.strip())[-1500:])
os.close(s.fd)
