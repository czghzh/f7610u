#!/usr/bin/env python3
"""
uboot_guard.py v2 — 串口守护：开机自动抢 U-Boot 并**直接注入完整引导序列**
================================================================================
v1 的致命缺陷：注入 `run bootcmd`，但 autoboot 每次把 env bootcmd 重写为原厂模板
（setenv bootargs...; bootm 0x88000240），导致 Wrong Image Format。
v2 修正：不依赖 bootcmd，逐条注入完整序列（每条都 <255 字符）：

    setenv bootargs console=ttyAMA0,115200n8 earlycon=zteuart,mmio32,0x10d0d000 root=/dev/mtdblock7 ro rootwait
    nand read 0x88000000 0x3680000 0x1A80000
    mw 0x1a2c0004 0; mw 0x17240004 0
    mw 0x1a2c0004 0xffffffff; mw 0x17240004 0xffffffff
    mw 0x1a000008 0xffffffff; mw 0x1a00000c 0xffffffff
    bootm 0x88000000

用法：
    python3 uboot_guard.py -d /dev/ttyUSB0 -l <log>
前提：kernel2 里是**纯 FIT**（work 里 v8.8c-rw），mtd2 任意值均可（本序列不读 mtd2）。
判据：设备起我们的固件 → ssh 22 开放 / 串口出现 v8.8 ready。
"""
import argparse
import os
import select
import sys
import termios
import time
import tty

HIT = b"Hit any key to stop autoboot"
PROMPT = b"=>"

# (命令, 发后等待秒数)
SEQ = [
    (b"setenv bootargs console=ttyAMA0,115200n8 earlycon=zteuart,mmio32,0x10d0d000 root=/dev/mtdblock7 ro rootwait", 0.6),
    (b"nand read 0x88000000 0x3680000 0x1A80000", 7.0),
    (b"mw 0x1a2c0004 0; mw 0x17240004 0", 0.4),
    (b"mw 0x1a2c0004 0xffffffff; mw 0x17240004 0xffffffff", 0.4),
    (b"mw 0x1a000008 0xffffffff; mw 0x1a00000c 0xffffffff", 0.4),
    (b"bootm 0x88000000", 3.0),
]


def open_serial(dev, baud=115200):
    fd = os.open(dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    tty.setraw(fd)
    attrs = termios.tcgetattr(fd)
    attrs[2] &= ~(termios.PARENB | termios.CSTOPB | termios.CSIZE
                  | termios.CRTSCTS | termios.IXON | termios.IXOFF)
    attrs[2] |= termios.CS8 | termios.CREAD | termios.CLOCAL
    speed = getattr(termios, "B%d" % baud)
    attrs[4] = speed
    attrs[5] = speed
    attrs[0] = 0
    attrs[1] = 0
    attrs[3] = 0
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-d", "--dev", default="/dev/ttyUSB0")
    ap.add_argument("-l", "--log", default="/home/wen/WorkBuddy/zx/f7610u/work/uboot_guard.log")
    ap.add_argument("-b", "--baud", type=int, default=115200)
    args = ap.parse_args()

    fd = open_serial(args.dev, args.baud)
    log = open(args.log, "ab", buffering=0)

    def note(msg):
        line = ("[%s] %s\n" % (time.strftime("%H:%M:%S"), msg)).encode()
        log.write(line)
        sys.stdout.write(line.decode())
        sys.stdout.flush()

    note("guard v2 started on %s @%d" % (args.dev, args.baud))
    buf = b""
    state = "idle"       # idle / picked / running / done
    step = 0
    state_t = 0.0
    cycle = 0

    while True:
        r, _, _ = select.select([fd], [], [], 0.2)
        now = time.time()
        if r:
            try:
                data = os.read(fd, 4096)
            except OSError:
                data = b""
            if data:
                buf += data
                if len(buf) > 8192:
                    buf = buf[-8192:]
                log.write(data)

        if state == "idle":
            if HIT in buf:
                cycle += 1
                note("cycle %d: autoboot window -> CR x3" % cycle)
                for _ in range(3):
                    os.write(fd, b"\r")
                    time.sleep(0.15)
                state = "picked"
                state_t = now
                buf = b""
        elif state == "picked":
            if PROMPT in buf[-256:]:
                note("cycle %d: prompt found -> injecting sequence" % cycle)
                state = "running"
                step = 0
                state_t = now
                buf = b""
            elif now - state_t > 8:
                note("cycle %d: no prompt, retry CR" % cycle)
                os.write(fd, b"\r")
                state_t = now
        elif state == "running":
            cmd, wait = SEQ[step]
            note("  [%d/%d] %s" % (step + 1, len(SEQ), cmd.decode()))
            os.write(fd, cmd + b"\r")
            time.sleep(wait)
            step += 1
            if step >= len(SEQ):
                note("cycle %d: sequence done" % cycle)
                state = "done"
                state_t = time.time()
        else:  # done
            if now - state_t > 30:
                note("cycle %d: returning to idle" % cycle)
                state = "idle"
                buf = b""


if __name__ == "__main__":
    main()
