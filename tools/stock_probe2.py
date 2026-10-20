#!/usr/bin/env python3
# 在原厂固件上做三件事：
#  1) 0x17059a58 是否自由计数（3 次 @1s）
#  2) 强制拉回 O1，读 SYSCTRL 0x10e00040..0x7c，判断那些 0x22/0x20 是静态配置还是 O5 状态
#  3) 看 /kmodule 里有哪些模块、有没有 tftp/nc 可以外传
import socket, time, re, sys

HOST = "192.168.1.1"

def run(lines, wait=2.0, read=3.0):
    s = socket.socket(); s.settimeout(10); s.connect((HOST, 23)); time.sleep(0.7)
    def drain(t=1.0):
        s.settimeout(t); out = b""
        try:
            while True:
                d = s.recv(65536)
                if not d:
                    break
                out += d
        except Exception:
            pass
        return out
    b = drain(1.2)
    if b"ogin" in b:
        s.sendall(b"root\r\n"); time.sleep(0.7); b += drain(0.8)
    if b"assword" in b:
        s.sendall(b"Zte521\r\n"); time.sleep(1.2); b += drain(1.5)
    s.sendall(("\r\n".join(lines) + "\r\n").encode())
    time.sleep(wait)
    txt = drain(read).decode("utf-8", "replace").replace("\r", "")
    s.sendall(b"exit\r\n")
    s.close()
    return txt

D = "/bin/devmem2"

def vals(txt):
    out = {}
    for a, v in re.findall(r"Value at address 0x([0-9A-Fa-f]+) \([^)]*\): (0x[0-9A-Fa-f]+)", txt):
        out.setdefault(int(a, 16), []).append(int(v, 16))
    return out

print("=== 1) 0x59a58 是否计数 ===")
L = [D + " 0x17059a58", "sleep 1", D + " 0x17059a58", "sleep 1", D + " 0x17059a58",
     D + " 0x17059400"]
v = vals(run(L, wait=4, read=4))
print(" 59a58:", [hex(x) for x in v.get(0x17059a58, [])])
print(" 59400:", [hex(x) for x in v.get(0x17059400, [])])

print("\n=== 2) 拉回 O1 后读 SYSCTRL ===")
L = [D + " 0x17059400 w 0x3ff0001", "sleep 1"]
for a in range(0x10e00028, 0x10e00080, 4):
    L.append(D + " 0x%08x" % a)
v = vals(run(L, wait=4, read=5))
for a in range(0x10e00028, 0x10e00080, 4):
    print("  %08x = %s" % (a, hex(v[a][0]) if a in v else "?"))

print("\n=== 3) /kmodule 与传输工具 ===")
print(run(["ls -l /kmodule", "which tftp nc ftpput tftpput", "uname -r"], wait=3, read=3)[-1500:])
