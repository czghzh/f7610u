#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""stock_sn_pulse.py —— 高频盯 `0x170599bc`（SN 触发寄存器），看原厂发 SN 时它到底怎么变。

上一轮 32 ms 采样只看到 state O1 -> O3、snctl 恒 0x1，bit3 从未置位。
两种可能：(a) bit3 是极短脉冲被采样跳过；(b) 原厂根本不用 bit3。
本脚本把采样压到单寄存器（~8 ms/点），并在抖动后进入 O3 的窗口内连盯 40 秒。
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
            d = s.recv(8192)
            if not d:
                break
            out += d
    except Exception:
        pass
    return out


def send(cmd, wait=1.5):
    s.sendall((cmd + "\r\n").encode())
    time.sleep(wait)
    return drain(1.5)


b = drain(1.2)
if b"ogin" in b:
    s.sendall(b"root\r\n")
    time.sleep(0.6)
    b += drain(0.8)
if b"assword" in b:
    s.sendall(b"Zte521\r\n")
    time.sleep(1.0)
    b += drain(1.2)

# 单寄存器高速采样：同时记录 state 和 snctl（两次 devmem2，约 16 ms 一点）
SAMPLE = (
    "( i=0; while [ $i -lt 2499 ]; do "
    "s=$(/bin/devmem2 0x17059400 2>/dev/null | awk '/Value at/{print $NF}'); "
    "c=$(/bin/devmem2 0x170599bc 2>/dev/null | awk '/Value at/{print $NF}'); "
    "echo \"$s $c\"; "
    "i=$((i+1)); done ) > /tmp/snp.log 2>&1 & echo CAPSTARTED"
)

FLAP = (
    "n=0; while [ $n -lt 12 ]; do i=0; "
    "while [ $i -lt 100 ]; do "
    "/bin/devmem2 0x17058400 w 0x63a >/dev/null 2>&1; i=$((i+1)); done; "
    "n=$((n+1)); done; echo FLAPDONE"
)

print("[1] start pulse sampler")
print(send(SAMPLE, 2.0).decode("utf-8", "replace")[-120:])

print("[2] flap LOS")
s.sendall((FLAP + "\r\n").encode())
t0 = time.time()
buf = b""
while time.time() - t0 < 45:
    d = drain(0.5)
    if d:
        buf += d
        if b"FLAPDONE" in buf:
            break
print("flap %.1fs" % (time.time() - t0))

print("[3] wait for sampler (45s)")
time.sleep(45)

print("[4] analyse")
cmd = (
    "wc -l /tmp/snp.log; echo '--- distinct state/ctl ---'; "
    "sort -u /tmp/snp.log | sed -n '1,40p'; "
    "echo '--- any ctl != 0x1 ---'; "
    "awk '$2!=\"0x1\"{print; n++} END{print \"count=\" n+0}' /tmp/snp.log | sed -n '1,40p'; "
    "echo ===TAIL===; tail -2 /tmp/snp.log"
)
print(send(cmd, 2.0).decode("utf-8", "replace"))
s.close()
