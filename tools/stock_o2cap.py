#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""stock_o2cap.py —— 在原厂固件上抓「O2 注册瞬间」的寄存器。

做法：
  1. 后台高频采样 4 个寄存器（state / snctl / burst / alloc0），~30 Hz
  2. 前台高频清 `0x17058400` bit0 制造 LOS，把原厂打回 O1，恢复后它会重走 O2/O3
  3. 采样跑满后，只打印「状态或 snctl 变化」的那些行

用法：python3 stock_o2cap.py [采样轮数]
"""
import socket, sys, time

HOST, PORT = "192.168.1.1", 23
ROUNDS = int(sys.argv[1]) if len(sys.argv) > 1 else 4000

s = socket.socket()
s.settimeout(8)
s.connect((HOST, PORT))
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


b = drain(1.2)
if b"ogin" in b:
    s.sendall(b"root\r\n")
    time.sleep(0.6)
    b += drain(0.8)
if b"assword" in b:
    s.sendall(b"Zte521\r\n")
    time.sleep(1.0)
    b += drain(1.2)

SAMPLE = (
    "( i=0; while [ $i -lt %d ]; do "
    "t=$(cut -d' ' -f1 /proc/uptime); "
    "s=$(/bin/devmem2 0x17059400 2>/dev/null | awk '/Value at/{print $NF}'); "
    "c=$(/bin/devmem2 0x170599bc 2>/dev/null | awk '/Value at/{print $NF}'); "
    "p=$(/bin/devmem2 0x17059910 2>/dev/null | awk '/Value at/{print $NF}'); "
    "a=$(/bin/devmem2 0x1705947c 2>/dev/null | awk '/Value at/{print $NF}'); "
    "echo \"$t $s $c $p $a\"; "
    "i=$((i+1)); done ) > /tmp/o2cap.log 2>&1 & echo CAPSTARTED"
) % ROUNDS

FLAP = (
    "n=0; while [ $n -lt 12 ]; do i=0; "
    "while [ $i -lt 100 ]; do "
    "/bin/devmem2 0x17058400 w 0x63a >/dev/null 2>&1; i=$((i+1)); done; "
    "n=$((n+1)); done; echo FLAPDONE"
)

print("[1] start sampler")
s.sendall((SAMPLE + "\r\n").encode())
time.sleep(2.0)
print(drain(2.0).decode("utf-8", "replace").replace("\r", "")[-200:])

print("[2] flap LOS")
s.sendall((FLAP + "\r\n").encode())
t0 = time.time()
buf = b""
while time.time() - t0 < 40:
    d = drain(0.5)
    if d:
        buf += d
        if b"FLAPDONE" in buf:
            break
print("flap done in %.1fs" % (time.time() - t0))

wait = int(ROUNDS * 0.032) + 10
print("[3] collect (wait %ds)" % wait)
time.sleep(wait)

print("[4] dump changes")
cmd = (
    "wc -l /tmp/o2cap.log; "
    "awk 'NR==1{print;ps=$2;pc=$3;next}{if($2!=ps||$3!=pc){print;ps=$2;pc=$3}}' "
    "/tmp/o2cap.log | sed -n '1,120p'; echo ===TAIL===; tail -3 /tmp/o2cap.log"
)
s.sendall((cmd + "\r\n").encode())
time.sleep(1.0)
out = b""
end = time.time() + 25
while time.time() < end:
    d = drain(0.8)
    if d:
        out += d
        end = time.time() + 3
print(out.decode("utf-8", "replace").replace("\r", ""))
s.close()
