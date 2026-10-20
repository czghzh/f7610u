#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ser_stock_boot_poll.py —— 重启原厂固件，从串口最早时机登录并轮询 PON 状态寄存器，
抓 O1→O2→O3→O4→O5 的激活过程。

用法: ser_stock_boot_poll.py [poll_seconds]
"""
import os, sys, time, select, termios, tty

DEV = "/dev/ttyUSB0"
POLL_SEC = int(sys.argv[1]) if len(sys.argv) > 1 else 100

fd = os.open(DEV, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
tty.setraw(fd)
a = termios.tcgetattr(fd)
a[2] |= termios.CLOCAL | termios.CREAD
a[4] = a[5] = termios.B115200
a[6][termios.VMIN] = 0
a[6][termios.VTIME] = 0
termios.tcsetattr(fd, termios.TCSANOW, a)

T0 = time.time()
log = []


def pump(t=0.2):
    out = b""
    end = time.time() + t
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.03)
        if r:
            try:
                d = os.read(fd, 4096)
            except OSError:
                d = b""
            if d:
                out += d
                end = max(end, time.time() + 0.08)
    return out


def send(s, wait=0.5):
    os.write(fd, s.encode() + b"\r")
    return pump(wait)


def ts():
    return "%6.1f" % (time.time() - T0)


def wait_for(pat, timeout):
    buf = b""
    end = time.time() + timeout
    while time.time() < end:
        d = pump(0.2)
        if d:
            buf += d
            log.append(d)
            if pat in buf:
                return True
    return False


# --- 阶段1: 登录并重启 ---
print("[%s] waiting for login prompt..." % ts())
if not wait_for(b"Login:", 20):
    # 也许已在 shell 里
    send("")
    pump(0.5)
send("root", 0.6)
send("Zte521", 0.8)
send("echo READY", 0.8)
send("reboot -f", 0.5)
print("[%s] reboot sent" % ts())

# --- 阶段2: 等新启动的 login 提示 ---
time.sleep(3)
print("[%s] waiting for boot login..." % ts())
if not wait_for(b"Login:", 120):
    print("[%s] NO LOGIN PROMPT" % ts())
    os.close(fd)
    sys.exit(1)
print("[%s] login prompt appeared" % ts())

# --- 阶段3: 登录并轮询 ---
send("root", 0.6)
send("Zte521", 0.8)
poll = ("i=0; while [ $i -lt %d ]; do "
        "V=$(/bin/devmem2 0x17059400 2>/dev/null | grep -o '0x[0-9a-fA-F]*$'); "
        "echo \"POLL $V\"; sleep 1; i=$((i+1)); done; echo POLLDONE" % POLL_SEC)
os.write(fd, poll.encode() + b"\r")

buf = b""
end = time.time() + POLL_SEC + 30
while time.time() < end:
    d = pump(0.3)
    if d:
        buf += d
        if b"POLLDONE" in buf:
            break
os.close(fd)

txt = buf.decode("utf-8", "replace").replace("\r", "")
lines = [l.strip() for l in txt.split("\n") if "POLL " in l or "POLLDONE" in l]
for l in lines:
    print(l)
