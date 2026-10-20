#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""在原厂 O5 上强制退回 O1，然后密集采样状态寄存器，抓注册全过程。"""
import socket, time, sys

HOST = "192.168.1.1"

CMDS = [
    "devmem2 0x17059400",
    "devmem2 0x170599bc",
    "devmem2 0x1705947c",
    "devmem2 0x17059800",
    "devmem2 0x17059908",
    # 强制 O1（Alloc-ID 0x3ff + state 1）
    "devmem2 0x17059400 w 0x03ff0001",
    "echo START",
    "i=0; while [ $i -lt 90 ]; do "
    "a=$(devmem2 0x17059400 | tail -1 | sed 's/.*: //'); "
    "b=$(devmem2 0x170599bc | tail -1 | sed 's/.*: //'); "
    "c=$(devmem2 0x1705947c | tail -1 | sed 's/.*: //'); "
    "d=$(devmem2 0x17059800 | tail -1 | sed 's/.*: //'); "
    "e=$(devmem2 0x17059908 | tail -1 | sed 's/.*: //'); "
    "echo \"$i $a $b $c $d $e\"; i=$((i+1)); sleep 0.5; done > /tmp/ponlog 2>&1; echo END",
    "cat /tmp/ponlog",
]


def drain(s, t=1.0):
    s.settimeout(t)
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


def main():
    s = socket.create_connection((HOST, 23), 6)
    time.sleep(0.6)
    b = drain(s, 1.0)
    if b"ogin" in b:
        s.sendall(b"root\r\n")
        time.sleep(0.8)
        b += drain(s, 1.0)
    if b"assword" in b:
        s.sendall(b"Zte521\r\n")
        time.sleep(1.2)
        b += drain(s, 1.2)
    s.sendall(b"\r\n")
    time.sleep(0.4)
    drain(s, 0.6)
    for c in CMDS:
        s.sendall(c.encode() + b"\r\n")
        if c.startswith("i=0;"):
            # 长循环，等 55 秒
            time.sleep(55)
            out = drain(s, 3.0)
        elif c == "cat /tmp/ponlog":
            time.sleep(3.0)
            out = drain(s, 3.0)
        else:
            time.sleep(0.8)
            out = drain(s, 1.0)
        txt = out.decode("utf-8", "replace").replace("\r", "")
        print("### " + c[:60])
        print(txt)
        sys.stdout.flush()
    try:
        s.sendall(b"exit\r\n")
    except Exception:
        pass


main()
