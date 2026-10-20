#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""stock_boot_catch.py —— 重启原厂，100ms 粒度抢 telnet，立即高频轮询 0x17059400，
抓 O1→O2→O3→O4→O5 的激活过程（带主机侧时间戳）。
"""
import socket, time, sys, re

HOST, PORT = "192.168.1.1", 23
READY = []


def tcp_login(timeout=3.0):
    s = socket.socket()
    s.settimeout(timeout)
    s.connect((HOST, PORT))
    time.sleep(0.6)
    s.settimeout(0.5)
    try:
        s.recv(4096)
    except Exception:
        pass
    s.sendall(b"root\r\n")
    time.sleep(0.5)
    s.sendall(b"Zte521\r\n")
    time.sleep(0.8)
    try:
        s.recv(4096)
    except Exception:
        pass
    return s


# 1) 先登录现有会话发 reboot
s = tcp_login()
s.settimeout(1.0)
s.sendall(b"reboot -f\r\n")
time.sleep(0.3)
try:
    s.close()
except Exception:
    pass
print("[host] reboot sent, waiting for telnet to come back...")
T0 = time.time()

# 2) 抢 telnet
while time.time() - T0 < 180:
    try:
        s = tcp_login(timeout=1.0)
        break
    except Exception:
        time.sleep(0.1)
else:
    print("NO TELNET")
    sys.exit(1)
print("[host %.1fs] telnet back, polling state" % (time.time() - T0))

# 3) 高频轮询
cmd = ("i=0; while [ $i -lt 300 ]; do "
       "V=$(/bin/devmem2 0x17059400 2>/dev/null | grep -o '0x[0-9a-fA-F]*$'); "
       "echo \"S $V\"; i=$((i+1)); done; echo DONE")
s.sendall((cmd + "\r\n").encode())
s.settimeout(0.5)
buf = b""
end = time.time() + 40
hist = []
while time.time() < end:
    try:
        d = s.recv(8192)
    except Exception:
        d = b""
    if d:
        buf += d
        if b"DONE" in buf:
            break
    for m in re.finditer(rb"S (0x[0-9a-fA-F]+)", buf):
        pass
    # 记录到达时间
txt = buf.decode("utf-8", "replace")
vals = re.findall(r"S (0x[0-9a-fA-F]+)", txt)
seq = []
for v in vals:
    if not seq or seq[-1][0] != v:
        seq.append([v, 1])
    else:
        seq[-1][1] += 1
print("[host] state sequence (value x count):")
for v, c in seq:
    print("  %s  x%d" % (v, c))
try:
    s.close()
except Exception:
    pass
