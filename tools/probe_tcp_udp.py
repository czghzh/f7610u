#!/usr/bin/env python3
"""Probe device L4 reachability: TCP 80/22 and UDP 53."""
import socket

HOST = "192.168.1.1"

for port in (80, 22):
    s = socket.socket()
    s.settimeout(1.5)
    try:
        s.connect((HOST, port))
        print("tcp%-3d ok" % port)
    except Exception as e:
        print("tcp%-3d err %s" % (port, type(e).__name__))
    s.close()

u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
u.settimeout(2.5)
q = (b"\x12\x34\x01\x00\x00\x01\x00\x00\x00\x00\x00\x00"
     b"\x07example\x03com\x00\x00\x01\x00\x01")
u.sendto(q, (HOST, 53))
try:
    d, a = u.recvfrom(512)
    print("udp53 reply %d bytes from %s" % (len(d), a[0]))
except Exception as e:
    print("udp53 err %s" % type(e).__name__)
u.close()
