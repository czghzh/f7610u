#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ub_cold_boot.py <itb> <grab_seconds> —— 冷启动专用：抢 U-Boot + 刷机，全程单次 open 串口。

为什么要一体：抢到提示符后如果另一个进程再 open 串口，USB-TTL 会复位，
U-Boot 收到垃圾字符，刚抢到的提示符就废了。

流程：持续喂回车抢 `=> ` → setenv bootargs → tftp → pon_reset 脉冲 → bootm
日志：work/ub_cold_boot.log
"""
import os, sys, time, select, termios, tty

DEV = "/dev/ttyUSB0"
ITB = sys.argv[1] if len(sys.argv) > 1 else "f7610u-v9.34-serdesreg.itb"
GRAB = float(sys.argv[2]) if len(sys.argv) > 2 else 300.0
LOG = "/home/wen/WorkBuddy/zx/f7610u/work/ub_cold_boot.log"

BOOTARGS = ("console=ttyAMA0,115200n8 earlycon=zteuart,mmio32,0x10d0d000 "
            "root=/dev/mtdblock7 ro rootwait")

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


def rd(t):
    global buf
    end = time.time() + t
    got = False
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.03)
        if r:
            try:
                d = os.read(fd, 8192)
            except (BlockingIOError, OSError):
                d = b""
            if d:
                logf.write(d)
                buf += d
                got = True
                end = max(end, time.time() + 0.15)
    return got


def send(s):
    os.write(fd, (s + "\r").encode())


def say(m):
    print(m, flush=True)


# ---------- 阶段 1：抢 U-Boot ----------
say("[1] grabbing U-Boot (max %.0fs) ..." % GRAB)
t0 = time.time()
last_key = 0.0
got = False

if os.environ.get("PON_REBOOT_FIRST") == "1":
    say("[1a] reboot from running Linux")
    for c in ("sync", "reboot -f"):
        os.write(fd, (c + "\r").encode())
        time.sleep(0.4)
    time.sleep(1.0)

while time.time() - t0 < GRAB:
    rd(0.02)
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

say("[1] got=%s elapsed=%.1fs" % (got, time.time() - t0))
if not got:
    logf.close()
    os.close(fd)
    sys.exit(2)

# 停喂键，清掉残留
time.sleep(0.3)
buf = b""
rd(0.4)
send("")
rd(0.8)
if b"=>" not in buf[-200:]:
    say("[!] prompt unstable: %r" % buf[-120:])
    # 再试一次
    buf = b""
    time.sleep(0.3)
    send("")
    rd(1.0)
    if b"=>" not in buf[-200:]:
        logf.close()
        os.close(fd)
        sys.exit(3)

# ---------- 阶段 2：bootargs ----------
say("[2] setenv bootargs")
buf = b""
send("setenv bootargs " + BOOTARGS)
rd(1.2)

# ---------- 阶段 3：tftp ----------
say("[3] tftp %s" % ITB)
buf = b""
send("tftp 0x88000000 " + ITB)
ok = False
t0 = time.time()
while time.time() - t0 < 150:
    rd(0.5)
    txt = buf.decode("utf-8", "replace")
    if "Bytes transferred" in txt:
        ok = True
        break
    if "alloc bp failed" in txt or "Retry count exceeded" in txt:
        break
say("[3] tftp ok=%s" % ok)
if not ok:
    logf.close()
    os.close(fd)
    sys.exit(4)

# ---------- 阶段 4：pon_reset 脉冲 ----------
say("[4] pon_reset pulse")
for c in ("mw 0x1a2c0004 0", "mw 0x17240004 0",
          "mw 0x1a2c0004 0xffffffff", "mw 0x17240004 0xffffffff"):
    buf = b""
    send(c)
    rd(0.3)
time.sleep(0.3)
for c in ("mw 0x1a000008 0xffffffff", "mw 0x1a00000c 0xffffffff"):
    buf = b""
    send(c)
    rd(0.3)

# ---------- 阶段 5：bootm ----------
say("[5] bootm")
buf = b""
send("bootm")
t0 = time.time()
hit = None
while time.time() - t0 < 60:
    rd(0.5)
    txt = buf.decode("utf-8", "replace")
    for p in ("Starting kernel", "Kernel panic", "Unsupported", "kernel start"):
        if p in txt:
            hit = p
            break
    if hit:
        break
say("[5] bootm -> %s" % hit)
rd(6.0)
logf.close()
os.close(fd)
say("DONE ok=%s" % bool(hit and hit != "Unsupported"))
