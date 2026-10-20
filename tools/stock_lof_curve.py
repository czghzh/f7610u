#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""stock_lof_curve.py —— 原厂固件(telnet root/Zte521)采 0x17058418/1c 生长曲线。

目的：判定 0x17058418 / 0x1705841c 是不是 LOF（下行帧失步）计数。
方法：稳态采 20 s -> 清 0x17058400 bit0 造 LOS 3 s -> 恢复 -> 再采 40 s。
      若断链期间两数跳变、恢复后重新归零/继续涨，就是失步类计数；
      若全程线性匀速增长、与 LOS 无关，那是别的东西（定时器/统计）。

同时采 0x17058514（接收计数）和 0x17059400（ONU 状态）作参照。

用法：python3 stock_lof_curve.py [host]
"""
import socket
import sys
import re
import time

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.1"

# 设备侧一次跑完，减少 telnet 往返。
# 每行格式：S <序号> <418> <41c> <514> <59400>
SCRIPT = r"""
R=/bin/devmem2
rd() { $R $1 2>/dev/null | grep "Value at" | awk '{print $NF}'; }
V0=$(rd 0x17058400)
echo "BASE $V0"
echo "PHASE1"
i=0
while [ $i -lt 20 ]; do
  A=$(rd 0x17058418)
  B=$(rd 0x1705841c)
  C=$(rd 0x17058514)
  D=$(rd 0x17059400)
  echo "S1 $i $A $B $C $D"
  i=$((i+1)); sleep 1
done
echo "LOSSTART"
$R 0x17058400 w 0x63a
sleep 3
$R 0x17058400 w $V0
echo "LOSEND $V0"
i=0
while [ $i -lt 40 ]; do
  A=$(rd 0x17058418)
  B=$(rd 0x1705841c)
  C=$(rd 0x17058514)
  D=$(rd 0x17059400)
  echo "S2 $i $A $B $C $D"
  i=$((i+1)); sleep 1
done
echo DONE
"""


def drain(s, t=1.0):
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


s = socket.socket()
s.settimeout(10)
s.connect((HOST, 23))
time.sleep(0.8)

b = drain(s, 1.2)
if b"ogin" in b:
    s.sendall(b"root\r\n")
    time.sleep(0.6)
    b += drain(s, 0.8)
if b"assword" in b:
    s.sendall(b"Zte521\r\n")
    time.sleep(1.0)
    b += drain(s, 1.2)

s.sendall((SCRIPT + "\r\n").encode())

buf = b""
end = time.time() + 110
while time.time() < end:
    d = drain(s, 1.0)
    if d:
        buf += d
        if b"DONE" in buf:
            break
try:
    s.sendall(b"exit\r\n")
except Exception:
    pass

txt = buf.decode("utf-8", "replace").replace("\r", "")
open("/home/wen/WorkBuddy/zx/f7610u/work/stock_lof_curve.raw.txt", "w").write(txt)

base = re.search(r"BASE (0x[0-9a-fA-F]+)", txt)
print("0x17058400 原值 =", base.group(1) if base else "?")

rows = []
for ph, i, a, b_, c, d in re.findall(
        r"S([12]) (\d+) (0x[0-9a-fA-F]+) (0x[0-9a-fA-F]+) (0x[0-9a-fA-F]+) (0x[0-9a-fA-F]+)",
        txt):
    rows.append((int(ph), int(i), int(a, 16), int(b_, 16), int(c, 16), int(d, 16)))

print("采样点数 =", len(rows), " (raw -> stock_lof_curve.raw.txt)")
print("")
print("阶段  序号  0x58418    0x5841c    0x58514    0x59400")
prev = None
for ph, i, a, b_, c, d in rows:
    cur = (a, b_, c, d)
    # 只在变化时打印，避免 60 行刷屏
    if prev is None or cur != prev:
        print("S%d   %3d   %#010x %#010x %#010x %#010x" % (ph, i, a, b_, c, d))
        prev = cur

# Δ 统计
if rows:
    def delta(key):
        vals = [r[key] for r in rows]
        return vals[-1] - vals[0], len(vals)
    for name, key in (("0x58418", 2), ("0x5841c", 3), ("0x58514", 4)):
        dv, n = delta(key)
        print("\n%s: 首 %#x -> 末 %#x  Δ=%d  (%d 点)" %
              (name, rows[0][key], rows[-1][key], dv, n))
