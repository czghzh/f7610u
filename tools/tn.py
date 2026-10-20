import socket, sys, time

cmd = sys.argv[1] if len(sys.argv) > 1 else "echo ALIVE"
s = socket.socket()
s.settimeout(6)
s.connect(("192.168.1.1", 23))
time.sleep(0.6)


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


banner = drain(1.0)
s.sendall((cmd + "\r\n").encode())
time.sleep(0.8)
mid = drain(0.8)
if b"assword" in mid or b"login" in mid:
    s.sendall(b"\r\n")          # empty root password
    time.sleep(1.0)
    mid += drain(1.0)
    s.sendall((cmd + "\r\n").encode())
    time.sleep(1.2)
data = mid + drain(2.0)
try:
    s.sendall(b"exit\r\n")
except Exception:
    pass
sys.stdout.write(data.decode("utf-8", "replace"))
