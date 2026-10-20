#!/usr/bin/env python3
"""Telnet into the stock firmware (192.168.1.1, root/Zte521) and issue a command.

Used to trigger a reboot from the stock system, because its serial console does
not echo - the only way back to the U-Boot prompt is a network-side reboot
followed by an autoboot-window interrupt.

  python3 telnet_reboot.py            # default: reboot
  python3 telnet_reboot.py "hostname" # arbitrary command
"""
import socket
import sys
import time
import select

HOST, PORT = "192.168.1.1", 23
TIMEOUT = 12


def _read(s, t=1.5):
    out = b""
    end = time.time() + t
    while time.time() < end:
        r, _, _ = select.select([s], [], [], 0.3)
        if not r:
            continue
        try:
            data = s.recv(65536)
        except OSError:
            break
        if not data:
            break
        out += data
        end = time.time() + t
    return out


def _wait_for(s, marker, t=TIMEOUT):
    buf = b""
    end = time.time() + t
    while time.time() < end:
        r, _, _ = select.select([s], [], [], 0.3)
        if not r:
            continue
        try:
            data = s.recv(65536)
        except OSError:
            break
        if not data:
            break
        buf += data
        if marker.lower() in buf.lower():
            return buf
    return buf


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "reboot"
    s = socket.create_connection((HOST, PORT), timeout=TIMEOUT)
    s.setblocking(False)

    sys.stdout.write(_wait_for(s, b"login").decode("utf-8", "replace"))
    s.sendall(b"root\r\n")
    time.sleep(0.6)

    sys.stdout.write(_wait_for(s, b"password").decode("utf-8", "replace"))
    s.sendall(b"Zte521\r\n")
    time.sleep(1.2)
    sys.stdout.write(_read(s, 1.5).decode("utf-8", "replace"))

    s.sendall(cmd.encode() + b"\r\n")
    time.sleep(0.5)
    sys.stdout.write(_read(s, 2.0).decode("utf-8", "replace"))
    s.close()
    print("\n[telnet_reboot] sent: %s" % cmd)


if __name__ == "__main__":
    main()
