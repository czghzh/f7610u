#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""telnet 原厂(root/Zte521)，依次执行 argv 里的命令并打印。"""
import socket, sys, time

HOST = "192.168.1.1"


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
    cmds = sys.argv[1:]
    s = socket.create_connection((HOST, 23), 6)
    time.sleep(0.6)
    b = drain(s, 1.0)
    if b"ogin" in b or b"ser" in b:
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
    for c in cmds:
        s.sendall(c.encode() + b"\r\n")
        time.sleep(0.7)
        out = drain(s, 1.0)
        print("### " + c)
        print(out.decode("utf-8", "replace").replace("\r", ""))
    try:
        s.sendall(b"exit\r\n")
    except Exception:
        pass


main()
