#!/usr/bin/env python3
# 在原厂上做逐项减法：把「原厂非0 / 我方为0」的那些寄存器逐个清 0，
# 每次清完都测 0x17059908（写1后能否立刻回0），最后恢复。
import socket, time, re

ADDRS = [
    0x17059878, 0x1705987c, 0x17059888, 0x17059890, 0x170598a4, 0x170598b8,
    0x170598cc, 0x1705998c, 0x17059994, 0x1705999c, 0x170599bc,
    0x170599d8, 0x170599dc, 0x170599e0, 0x170599e4, 0x170599e8, 0x170599ec,
    0x170599f0, 0x170599f4, 0x17059a08, 0x17059a0c, 0x17059a10,
    0x17059a58, 0x17059a5c, 0x17059a64, 0x17059a6c, 0x17059a7c,
]

D = "/bin/devmem2"


def connect():
    s = socket.socket(); s.settimeout(12); s.connect(("192.168.1.1", 23)); time.sleep(0.7)
    b = b""
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
    return s, drain


def cmd(s, drain, lines, wait=0.8, rd=1.2):
    s.sendall(("\r\n".join(lines) + "\r\n").encode())
    time.sleep(wait)
    return drain(rd).decode("utf-8", "replace").replace("\r", "")


def read_reg(s, drain, a):
    t = cmd(s, drain, [D + " 0x%08x" % a])
    m = re.search(r"\): (0x[0-9A-Fa-f]+)", t)
    return int(m.group(1), 16) if m else None


def fire_ok(s, drain):
    """写 1 到 0x17059908，300ms 后读回；0 = 能发"""
    cmd(s, drain, [D + " 0x17059908 w 0x1"], wait=0.3, rd=0.4)
    v = read_reg(s, drain, 0x17059908)
    return v == 0, v


s, drain = connect()
ok, v = fire_ok(s, drain)
print("基线 fire: ok=%s (59908=%s)" % (ok, hex(v) if v is not None else "?"))
if not ok:
    print("基线就不通，先排查"); s.sendall(b"exit\r\n"); raise SystemExit

bad = []
for a in ADDRS:
    orig = read_reg(s, drain, a)
    if orig is None:
        print("%08x 读失败，跳过" % a); continue
    if orig == 0:
        print("%08x 本来就是 0，跳过" % a); continue
    cmd(s, drain, [D + " 0x%08x w 0x0" % a], wait=0.25, rd=0.35)
    ok2, v2 = fire_ok(s, drain)
    print("%08x 原=%08x -> 清0 后 fire %s (59908=%s)" %
          (a, orig, "OK" if ok2 else "**卡住**", hex(v2) if v2 is not None else "?"))
    if not ok2:
        bad.append(a)
    # 恢复
    cmd(s, drain, [D + " 0x%08x w 0x%x" % (a, orig)], wait=0.25, rd=0.35)

print("\n导致发送卡住的寄存器:", ["0x%08x" % x for x in bad] or "无")
s.sendall(b"exit\r\n")
