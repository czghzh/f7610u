#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""stock_o1dump.py —— 抓原厂「O1（注册前）」的寄存器快照。

背景（PON-PLAN §79）：原厂重注册时间线是 O1 →(10 s)→ O3 →(2 s)→ O4 → O5，
中间没有 O2。O1 有约 10 秒窗口，足够用 devmem2 扫 768 个寄存器。

扫描区间（区别于已存在的 O5 快照 `work/o5_regs.json`）：
  0x17059400 .. 0x170594ff   alloc / ONU 状态
  0x17059800 .. 0x170599ff   上行 / SN / burst profile

拿到后与 o5_regs.json 做 diff，差异就是原厂状态机在激活过程中配置的东西。
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

SCAN = (
    "scan() { base=$1; n=$2; out=$3; i=0; while [ $i -lt $n ]; do "
    "a=$((base+i*4)); printf \"%08x \" $a; "
    "/bin/devmem2 $a 2>/dev/null | awk '/Value at/{print $NF}'; "
    "i=$((i+1)); done > $out; }; "
    "scan 0x17059400 256 /tmp/o1_a.txt; "
    "scan 0x17059800 512 /tmp/o1_b.txt; "
    "echo SCANDONE; wc -l /tmp/o1_a.txt /tmp/o1_b.txt"
)

FLAP = (
    "n=0; while [ $n -lt 12 ]; do i=0; "
    "while [ $i -lt 100 ]; do "
    "/bin/devmem2 0x17058400 w 0x63a >/dev/null 2>&1; i=$((i+1)); done; "
    "n=$((n+1)); done; echo FLAPDONE"
)

print("[1] flap")
s.sendall((FLAP + "\r\n").encode())
t0 = time.time()
buf = b""
while time.time() - t0 < 40:
    d = drain(0.4)
    if d:
        buf += d
        if b"FLAPDONE" in buf:
            break
print("flap %.1fs" % (time.time() - t0))

print("[2] scan O1 (expect ~15s)")
s.sendall((SCAN + "\r\n").encode())
time.sleep(2.0)          # 吃掉命令回显，否则会立刻匹配到 SCANDONE
drain(1.0)
t0 = time.time()
buf = b""
while time.time() - t0 < 60:
    d = drain(0.6)
    if d:
        buf += d
        if b"SCANDONE" in buf:
            break
print("scan %.1fs" % (time.time() - t0))
print(buf.decode("utf-8", "replace").replace("\r", "")[-300:])

print("[3] fetch")
s.sendall(b"cat /tmp/o1_a.txt; echo ===B===; cat /tmp/o1_b.txt; echo ===END===\r\n")
time.sleep(2.0)
drain(1.0)
out = b""
end = time.time() + 40
while time.time() < end:
    d = drain(1.0)
    if d:
        out += d
        if b"===END===" in out:
            break
s.close()

txt = out.decode("utf-8", "replace").replace("\r", "")
# 只保留 "addr value" 行
lines = []
for ln in txt.split("\n"):
    p = ln.strip().split()
    if len(p) == 2 and p[0].startswith("170") and p[1].startswith("0x"):
        lines.append("%s %s" % (p[0], p[1].lower()))
outfile = "/home/wen/WorkBuddy/zx/f7610u/work/stock_o1.txt"
open(outfile, "w").write("\n".join(lines) + "\n")
print("saved %d registers -> %s" % (len(lines), outfile))
