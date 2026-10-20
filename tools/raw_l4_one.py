#!/usr/bin/env python3
"""Send ONE hand-built frame of a chosen protocol, with a CORRECT L4 checksum.
Usage: raw_l4_one.py tcp|udp|icmp
"""
import socket
import struct
import sys

IFACE = "enx00e04c680a76"
SRC_MAC = bytes.fromhex("00e04c680a76")
DST_MAC = bytes.fromhex("00d0d0818283")
SRC_IP = "192.168.1.100"
DST_IP = "192.168.1.1"


def csum(data):
    if len(data) % 2:
        data += b"\x00"
    s = 0
    for i in range(0, len(data), 2):
        s += (data[i] << 8) + data[i + 1]
    while s >> 16:
        s = (s & 0xffff) + (s >> 16)
    return (~s) & 0xffff


def l4_csum(proto, payload):
    pseudo = (socket.inet_aton(SRC_IP) + socket.inet_aton(DST_IP) +
              struct.pack("!BBH", 0, proto, len(payload)))
    return csum(pseudo + payload)


def ip4(proto, payload, ident):
    total = 20 + len(payload)
    hdr = struct.pack("!BBHHHBBH4s4s", 0x45, 0, total, ident, 0, 64, proto, 0,
                      socket.inet_aton(SRC_IP), socket.inet_aton(DST_IP))
    hdr = hdr[:10] + struct.pack("!H", csum(hdr)) + hdr[12:]
    return hdr + payload


kind = sys.argv[1]

if kind == "tcp":
    l4 = struct.pack("!HHIIBBHHH", 40021, 80, 0x11223344, 0, 5 << 4, 0x02, 65535, 0, 0)
    l4 = l4[:16] + struct.pack("!H", l4_csum(6, l4)) + l4[18:]
    pkt = ip4(6, l4, 0x3001)
elif kind == "udp":
    pl = b"abcd"
    l4 = struct.pack("!HHHH", 40022, 53, 8 + len(pl), 0) + pl
    l4 = l4[:6] + struct.pack("!H", l4_csum(17, l4)) + l4[8:]
    pkt = ip4(17, l4, 0x3002)
elif kind == "icmp":
    l4 = struct.pack("!BBHHH", 8, 0, 0, 0x4444, 1) + b"hello"
    l4 = l4[:2] + struct.pack("!H", csum(l4)) + l4[4:]
    pkt = ip4(1, l4, 0x3003)
else:
    sys.exit("bad proto")

s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW)
s.bind((IFACE, 0))
frame = DST_MAC + SRC_MAC + struct.pack("!H", 0x0800) + pkt
if len(frame) < 60:
    frame += b"\x00" * (60 - len(frame))
s.send(frame)
print("sent %s" % kind)
