import socket, sys, time

# 用法: tn_batch.py <cmdfile> <outfile> [idle]
HOST = "192.168.1.1"
USER = "root"
PASS = "Zte521"
cmdfile = sys.argv[1]
outfile = sys.argv[2]
IDLE = float(sys.argv[3]) if len(sys.argv) > 3 else 1.2

cmds = [l.rstrip("\n") for l in open(cmdfile) if l.strip() and not l.startswith("#")]


def rx(s, idle=1.0, limit=30):
    s.settimeout(idle)
    out = b""
    t0 = time.time()
    while time.time() - t0 < limit:
        try:
            d = s.recv(65536)
            if not d:
                break
            out += d
            t0 = time.time()
        except socket.timeout:
            break
    return out


s = socket.socket()
s.settimeout(8)
s.connect((HOST, 23))
time.sleep(0.5)
buf = rx(s, 1.0)
if b"ogin" in buf:
    s.sendall((USER + "\r\n").encode())
    time.sleep(0.7)
    buf += rx(s, 1.0)
if b"assword" in buf:
    s.sendall((PASS + "\r\n").encode())
    time.sleep(1.0)
    buf += rx(s, 1.2)
if b"enied" in buf or b"ncorrect" in buf:
    print("LOGIN FAILED")
    sys.exit(2)

f = open(outfile, "wb")
for c in cmds:
    s.sendall((c + "\r\n").encode())
    f.write(b"\n@@CMD " + c.encode() + b"\n")
    f.write(rx(s, IDLE, 20))
    f.flush()
s.close()
f.close()
print("done %d cmds -> %s" % (len(cmds), outfile))
