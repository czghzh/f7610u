#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""stock_fetch.py —— 分批把原厂 /tmp/o1_a.txt、/tmp/o1_b.txt 取回来拼成一个文件。

整文件 cat 会被 telnet 丢包（768 行约 19 KB），所以按 96 行一段取。
"""
import socket, sys, time

s = socket.socket()
s.settimeout(8)
s.connect(("192.168.1.1", 23))
time.sleep(0.6)


def drain(t=1.0):
    s.settimeout(t)
    out = b""
    try:
        while True:
            d = s.recv(16384)
            if not d:
                break
            out += d
    except Exception:
        pass
    return out


def cmd(c, pre=0.9, post=1.6):
    s.sendall((c + "\r\n").encode())
    time.sleep(pre)
    out = drain(0.3)
    out += drain(post)
    return out


b = drain(1.2)
if b"ogin" in b:
    s.sendall(b"root\r\n")
    time.sleep(0.6)
    b += drain(0.8)
if b"assword" in b:
    s.sendall(b"Zte521\r\n")
    time.sleep(1.0)
    b += drain(1.2)

rows = []
for fname, total in (("/tmp/o1_a.txt", 256), ("/tmp/o1_b.txt", 512)):
    step = 96
    for start in range(1, total + 1, step):
        end = min(start + step - 1, total)
        out = cmd("sed -n '%d,%dp' %s" % (start, end, fname), 0.9, 1.6)
        txt = out.decode("utf-8", "replace").replace("\r", "")
        for ln in txt.split("\n"):
            p = ln.strip().split()
            if len(p) == 2 and p[0].startswith("170") and p[1].lower().startswith("0x"):
                rows.append("%s %s" % (p[0], p[1].lower()))
        print("%s %d-%d -> total %d" % (fname, start, end, len(rows)))

s.close()
outfile = "/home/wen/WorkBuddy/zx/f7610u/work/stock_o1.txt"
open(outfile, "w").write("\n".join(rows) + "\n")
print("saved %d registers -> %s" % (len(rows), outfile))
