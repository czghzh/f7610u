#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""stock_58dump.py —— 抓原厂 0x17058000-0x170593ff 与 0x17059a00-0x17059fff。

原厂只有 telnet，没有 sshd，zxreg 传不上去，用 devmem2 扫。
只打印非零值，压缩传输量。写完 /tmp/s_a.txt /tmp/s_b.txt，再分批取回。
"""
import socket, time

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


b = drain(1.2)
if b"ogin" in b:
    s.sendall(b"root\r\n")
    time.sleep(0.6)
    b += drain(0.8)
if b"assword" in b:
    s.sendall(b"Zte521\r\n")
    time.sleep(1.0)
    b += drain(1.2)

SCAN = (
    "scan() { base=$1; n=$2; out=$3; i=0; while [ $i -lt $n ]; do "
    "a=$((base+i*4)); "
    "v=$(/bin/devmem2 $a 2>/dev/null | awk '/Value at/{print $NF}'); "
    "[ \"$v\" != \"0x00000000\" ] && echo \"$a $v\" >> $out; "
    "i=$((i+1)); done; }; "
    "rm -f /tmp/s_a.txt /tmp/s_b.txt; "
    "scan 0x17058000 5120 /tmp/s_a.txt; "
    "scan 0x17059a00 1024 /tmp/s_b.txt; "
    "echo SCANDONE; wc -l /tmp/s_a.txt /tmp/s_b.txt"
)

print("[1] scan (expect ~2min)")
s.sendall((SCAN + "\r\n").encode())
time.sleep(2.0)
drain(1.0)
t0 = time.time()
buf = b""
while time.time() - t0 < 400:
    d = drain(0.8)
    if d:
        buf += d
        if b"SCANDONE" in buf:
            break
print("scan %.1fs" % (time.time() - t0))
print(buf.decode("utf-8", "replace").replace("\r", "")[-300:])

out = b""
for tag, path in (("===A===", "/tmp/s_a.txt"), ("===B===", "/tmp/s_b.txt")):
    # 分批取，每次 120 行，避免 telnet 丢包
    nlines = 0
    while True:
        s.sendall(("sed -n '%d,%dp' %s; echo %s%d\r\n"
                   % (nlines + 1, nlines + 120, path, tag, nlines + 1)).encode())
        time.sleep(0.8)
        chunk = drain(1.0)
        marker = ("%s%d" % (tag, nlines + 1)).encode()
        if marker not in chunk:
            break
        body = chunk.split(marker, 1)[0]
        out += body
        if not body.strip():
            break
        nlines += 120
    print("fetched %s up to line %d" % (path, nlines))

s.close()
txt = out.decode("utf-8", "replace").replace("\r", "")
lines = []
for ln in txt.split("\n"):
    p = ln.strip().split()
    if len(p) == 2 and p[0].startswith("170") and p[1].startswith("0x"):
        lines.append("%s %s" % (p[0], p[1].lower()))
outfile = "/home/wen/WorkBuddy/zx/f7610u/work/stock_58k.txt"
open(outfile, "w").write("\n".join(lines) + "\n")
print("saved %d registers -> %s" % (len(lines), outfile))
