#!/usr/bin/env python3
"""Read a list of registers from the stock firmware over telnet.

Stock has no ssh; busybox devmem2 is the only reader.  We emit one
"addr=value" line per register and parse it back on the host.
"""
import re
import socket
import sys
import time

HOST = "192.168.1.1"


def connect():
    s = socket.socket()
    s.settimeout(6)
    s.connect((HOST, 23))
    time.sleep(0.8)

    def rd(t=1.0):
        s.settimeout(t)
        out = b""
        try:
            while True:
                d = s.recv(16384)
                if not d:
                    break
                out += d
        except Exception:
            pass
        return out

    b = rd(1.0)
    if b"ogin" in b:
        s.sendall(b"root\r\n")
        time.sleep(0.6)
        b += rd(0.8)
    if b"assword" in b:
        s.sendall(b"Zte521\r\n")
        time.sleep(1.0)
        b += rd(1.2)
    return s, rd


def read_regs(addrs, timeout=30):
    s, rd = connect()
    expr = " ".join("%08x" % a for a in addrs)
    cmd = ('for a in %s; do echo -n "$a="; '
           '/bin/devmem2 0x$a | tail -1 | cut -d: -f2; done; echo RDONE\r\n' % expr)
    s.sendall(cmd.encode())
    buf = b""
    end = time.time() + timeout
    while time.time() < end:
        d = rd(1.0)
        buf += d
        if b"RDONE" in buf:
            break
    s.sendall(b"exit\r\n")
    s.close()
    txt = buf.decode("utf8", "replace").replace("\r", "")
    out = []
    for line in txt.split("\n"):
        m = re.match(r"^([0-9a-f]{8})=\s*(0x[0-9a-fA-F]+|\S*)", line.strip())
        if m:
            try:
                out.append((int(m.group(1), 16), int(m.group(2), 16)))
            except ValueError:
                out.append((int(m.group(1), 16), None))
    return out


if __name__ == "__main__":
    addrs = []
    for a in sys.argv[1:]:
        addrs.extend(range(int(a, 16), int(a, 16) + 0x40, 4))
    # de-dup, keep order
    seen = set()
    addrs = [a for a in addrs if not (a in seen or seen.add(a))]
    for a, v in read_regs(addrs):
        print("%08x %s" % (a, "0x%08x" % v if v is not None else "ERR"))
