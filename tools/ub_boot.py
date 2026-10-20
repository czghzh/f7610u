#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ub_boot.py —— 单次打开串口抢 U-Boot 提示符，拿到后可继续发命令。

之前用 bash 'printf > /dev/ttyUSB0' 循环喂键，每次 printf 都是一次 open/close，
USB 串口会因此复位、日志全空。这里只 open 一次。

用法:
  ub_boot.py grab        # 重启原厂并抢提示符
  ub_boot.py keep        # 只喂键(设备已在重启中)
  ub_boot.py cmd "..."   # 已能进 U-Boot 时，抢到后依次执行命令(; 分隔)
"""
import os, sys, time, select, termios, socket, tty

DEV = "/dev/ttyUSB0"
LOG = "/tmp/ub_boot.bin"


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


def reboot_vendor():
    try:
        s = socket.create_connection(("192.168.1.1", 23), 5)
    except OSError as e:
        print("telnet connect failed:", e)
        return False
    time.sleep(0.5)
    s.settimeout(1.0)

    def rd():
        out = b""
        try:
            while True:
                d = s.recv(4096)
                if not d:
                    break
                out += d
        except Exception:
            pass
        return out

    b = rd()
    if b"ogin" in b:
        s.sendall(b"root\r\n")
        time.sleep(0.6)
        b += rd()
    if b"assword" in b:
        s.sendall(b"Zte521\r\n")
        time.sleep(0.8)
        rd()
    s.sendall(b"reboot -f\r\n")
    time.sleep(0.5)
    try:
        s.close()
    except Exception:
        pass
    return True


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "grab"
    fd = open_port()
    log = open(LOG, "wb")
    if mode == "grab":
        time.sleep(0.2)
        reboot_vendor()
    t0 = time.time()
    last_key = 0.0
    buf = b""
    got = False
    limit = 100.0 if mode != "keep" else 60.0
    prev = b""
    while time.time() - t0 < limit:
        r, _, _ = select.select([fd], [], [], 0.02)
        if r:
            try:
                d = os.read(fd, 4096)
            except OSError:
                d = b""
            if d:
                log.write(d)
                log.flush()
                buf += d
        # 持续喂回车（U-Boot 不打印 Hit any key，只能盲打）
        if time.time() - last_key > 0.03:
            try:
                os.write(fd, b"\r")
            except OSError:
                pass
            last_key = time.time()
        # 判断是否停在提示符：出现 "=> " 且之后 0.6s 无新数据
        if buf.endswith(b"=> ") or buf.endswith(b"=> \r") or buf.endswith(b"=>"):
            time.sleep(0.25)
            r, _, _ = select.select([fd], [], [], 0.3)
            if r:
                d = os.read(fd, 4096)
                log.write(d)
                buf += d
            if buf.rstrip().endswith(b"=>"):
                got = True
                break
        # 提示符可能被回车刷掉，检查尾部片段
        tail = buf[-400:]
        if b"=> " in tail and b"Starting kernel" not in buf[-200:]:
            # 再稳一下
            time.sleep(0.2)
            got = True
            break
    # 停止喂键
    time.sleep(0.2)
    log.close()
    os.close(fd)
    print("=== GOT=%s ===" % got)
    txt = buf.decode("utf-8", "replace").replace("\r", "")
    print(txt[-1200:])

    if got and mode == "cmd" and len(sys.argv) > 2:
        run_cmds(sys.argv[2])


def run_cmds(cmds):
    fd = open_port()
    for c in cmds.split(";"):
        c = c.strip()
        if not c:
            continue
        os.write(fd, (c + "\r").encode())
        end = time.time() + 1.5
        out = b""
        while time.time() < end:
            r, _, _ = select.select([fd], [], [], 0.05)
            if r:
                try:
                    out += os.read(fd, 4096)
                except (BlockingIOError, OSError):
                    pass
                end = time.time() + 0.6
        print(out.decode("utf-8", "replace").replace("\r", ""))
    os.close(fd)


main()
