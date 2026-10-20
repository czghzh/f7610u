#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""stock_los_exp.py —— 在原厂固件上制造 LOS 再恢复，分段抓注册全过程寄存器。

流程：基线 → 清 0x17058400 bit0（断 RX 制造 LOS）→ 等 2s → 恢复 → 连续 10 次扫关键寄存器。
"""
import socket, time

ADDRS = [
    "0x17059400",  # ONU 激活态 + alloc/onu_id
    "0x170599bc",  # SN 发送控制
    "0x170598d8",  # SN lo
    "0x170598dc",  # SN hi
    "0x170598f8",  # SN MIC lo
    "0x170598fc",  # SN MIC hi
    "0x1705947c",  # SN alloc-ID
    "0x17059480",  # SN alloc-ID
    "0x17059910",  # burst profile
]

dump = "".join(
    'printf "%s="; /bin/devmem2 %s 2>/dev/null | grep "Value at" | awk "{print \\$NF}"; '
    % (a, a) for a in ADDRS
)

script = (
    'echo ===BASELINE===; ' + dump +
    'echo ===CLEAR_RX===; /bin/devmem2 0x17058400 w 0x63a; sleep 2; '
    'echo ===AFTER_LOS===; /bin/devmem2 0x17058400 2>/dev/null | grep "Value at"; ' + dump +
    'echo ===REENABLE===; /bin/devmem2 0x17058400 w 0x63b; '
    'i=1; while [ $i -le 10 ]; do echo "===SWEEP$i==="; ' + dump +
    'sleep 1; i=$((i+1)); done; echo ===END==='
)

s = socket.socket()
s.settimeout(10)
s.connect(("192.168.1.1", 23))
time.sleep(0.7)


def drain(t=1.0):
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


b = drain(1.2)
if b"ogin" in b:
    s.sendall(b"root\r\n")
    time.sleep(0.6)
    b += drain(0.8)
if b"assword" in b:
    s.sendall(b"Zte521\r\n")
    time.sleep(1.0)
    b += drain(1.2)

s.sendall((script + "\r\n").encode())
time.sleep(0.5)
out = b""
end = time.time() + 30
while time.time() < end:
    d = drain(1.0)
    if d:
        out += d
        end = max(end, time.time() + 1.5)
    if b"===END===" in out:
        break
try:
    s.sendall(b"exit\r\n")
except Exception:
    pass

print(out.decode("utf-8", "replace").replace("\r", ""))
