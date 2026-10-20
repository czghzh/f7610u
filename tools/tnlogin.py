import socket, sys, time

HOST = "192.168.1.1"
USER = sys.argv[1] if len(sys.argv) > 1 else "root"
PASS = sys.argv[2] if len(sys.argv) > 2 else ""
CMD = sys.argv[3] if len(sys.argv) > 3 else "id; uname -a"
WAIT = float(sys.argv[4]) if len(sys.argv) > 4 else 1.2


def drain(s, t=1.0):
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


s = socket.socket()
s.settimeout(6)
s.connect((HOST, 23))
time.sleep(0.5)
buf = drain(s, 0.8)

if b"ogin" in buf or b"username" in buf.lower():
    s.sendall((USER + "\r\n").encode())
    time.sleep(0.6)
    buf += drain(s, 0.8)

if b"assword" in buf:
    s.sendall((PASS + "\r\n").encode())
    time.sleep(0.9)
    buf += drain(s, 1.0)

if b"enied" in buf or b"ncorrect" in buf:
    sys.stdout.write("LOGIN-FAILED\n" + buf.decode("utf-8", "replace"))
    raise SystemExit(2)

s.sendall((CMD + "\r\n").encode())
time.sleep(WAIT)
buf += drain(s, 2.5)
try:
    s.sendall(b"exit\r\n")
except Exception:
    pass
sys.stdout.write(buf.decode("utf-8", "replace"))
