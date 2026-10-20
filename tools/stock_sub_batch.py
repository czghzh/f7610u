#!/usr/bin/env python3
# 一次性下发：对每个候选寄存器，清 0 -> fire -> 读 59908 -> 恢复。
# 原始值取自 o5_regs.json（原厂 O5 快照）。
import json, socket, time, re

ADDRS = [
    0x17059878, 0x1705987c, 0x17059888, 0x17059890, 0x170598a4, 0x170598b8,
    0x170598cc, 0x1705998c, 0x17059994, 0x1705999c, 0x170599bc,
    0x170599d8, 0x170599dc, 0x170599e0, 0x170599e4, 0x170599e8, 0x170599ec,
    0x170599f0, 0x170599f4, 0x17059a08, 0x17059a0c, 0x17059a10,
    0x17059a58, 0x17059a5c, 0x17059a64, 0x17059a6c, 0x17059a7c,
]
D = "/bin/devmem2"
o5 = {int(k, 16): v for k, v in json.load(open("/home/wen/WorkBuddy/zx/f7610u/work/o5_regs.json")).items()}

lines = []
for i, a in enumerate(ADDRS):
    orig = o5.get(a, 0)
    lines.append("echo MK%d" % i)
    lines.append("%s 0x%08x w 0x0" % (D, a))
    lines.append("%s 0x17059908 w 0x1" % D)
    lines.append("sleep 0.3")
    lines.append("%s 0x17059908" % D)
    lines.append("%s 0x%08x w 0x%x" % (D, a, orig))
lines.append("echo ALLDONE")

s = socket.socket(); s.settimeout(15); s.connect(("192.168.1.1", 23)); time.sleep(0.7)
def drain(t=1.0):
    s.settimeout(t); out = b""
    try:
        while True:
            d = s.recv(65536)
            if not d: break
            out += d
    except Exception: pass
    return out
b = drain(1.2)
if b"ogin" in b: s.sendall(b"root\r\n"); time.sleep(0.7); b += drain(0.8)
if b"assword" in b: s.sendall(b"Zte521\r\n"); time.sleep(1.2); b += drain(1.5)

# 基线
s.sendall((D + " 0x17059908 w 0x1\nsleep 0.3\n" + D + " 0x17059908\necho BASE\n").encode())
time.sleep(2)
base = drain(2).decode("utf-8", "replace").replace("\r", "")
print("BASE:", [l for l in base.splitlines() if "0x17059908" in l or l.strip() == "BASE"][-3:])

payload = "\n".join(lines) + "\n"
s.sendall(payload.encode())
buf = b""
t0 = time.time()
while time.time() - t0 < 120:
    d = drain(2.0)
    if d: buf += d
    if b"ALLDONE" in buf: break
txt = buf.decode("utf-8", "replace").replace("\r", "")
s.sendall(b"exit\r\n")

# 解析
blocks = re.split(r"MK(\d+)\n", txt)
res = {}
for i in range(1, len(blocks) - 1, 2):
    idx = int(blocks[i]); body = blocks[i + 1]
    m = re.findall(r"0x17059908 \([^)]*\): (0x[0-9A-Fa-f]+)", body)
    if m:
        res[idx] = int(m[-1], 16)
print("\n逐个清 0 后 0x17059908 的读回值（0=能发, 1=卡住）:")
bad = []
for i, a in enumerate(ADDRS):
    v = res.get(i)
    tag = "" if v == 0 else ("  <== 卡住" if v is not None else "  (无数据)")
    if v not in (0, None):
        bad.append(a)
    print("  %08x  orig=%08x  59908=%s%s" % (a, o5.get(a, 0), ("0x%x" % v) if v is not None else "?", tag))
print("\n候选:", ["0x%08x" % x for x in bad] or "无")
