#!/usr/bin/env python3
# telnet 进原厂固件执行命令并回收输出
import socket, time

CMDS = [
    "reboot -f",
]

s = socket.create_connection(("192.168.1.1", 23), 8)
s.settimeout(1.0)
buf = b""

def rd(t=1.0):
    global buf
    end = time.time() + t
    while time.time() < end:
        try:
            d = s.recv(8192)
            if not d:
                break
            buf += d
            end = time.time() + 0.3
        except Exception:
            pass
    return buf

rd(1.5)
if b"ogin" in buf:
    s.sendall(b"root\r\n"); time.sleep(0.5); rd(1.0)
if b"assword" in buf:
    s.sendall(b"Zte521\r\n"); time.sleep(0.8); rd(1.5)
buf = b""
for c in CMDS:
    s.sendall(c.encode() + b"\r\n")
    time.sleep(0.4)
    out = rd(12.0)
    txt = out.decode("utf-8", "replace").replace("\r", "")
    print("$ %s\n%s" % (c, txt.strip()[-2000:]))
    buf = b""
s.close()
