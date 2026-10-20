#!/usr/bin/env python3
"""Send hand-built L4 frames (TCP / UDP / ICMP) to the device, bypassing the
host TCP stack and all offload.  Count how many the device actually receives."""
import socket
import struct
import time

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


def ip4(proto, payload, ident):
    total = 20 + len(payload)
    hdr = struct.pack("!BBHHHBBH4s4s", 0x45, 0, total, ident, 0, 64, proto, 0,
                      socket.inet_aton(SRC_IP), socket.inet_aton(DST_IP))
    hdr = hdr[:10] + struct.pack("!H", csum(hdr)) + hdr[12:]
    return hdr + payload


s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW)
s.bind((IFACE, 0))


def send(ip_pkt):
    frame = DST_MAC + SRC_MAC + struct.pack("!H", 0x0800) + ip_pkt
    if len(frame) < 60:
        frame += b"\x00" * (60 - len(frame))
    s.send(frame)


# TCP SYN (protocol 6), checksum left 0 - device will drop at L4 but RX ring may still take it
tcp = struct.pack("!HHIIBBHHH", 40001, 80, 0x11223344, 0, 5 << 4, 0x02, 65535, 0, 0)
send(ip4(6, tcp, 0x1001))
time.sleep(0.3)

# UDP (protocol 17)
udp = struct.pack("!HHHH", 40002, 53, 8 + 4, 0) + b"abcd"
send(ip4(17, udp, 0x1002))
time.sleep(0.3)

# ICMP echo (protocol 1) - control: should be received
icmp = struct.pack("!BBHHH", 8, 0, 0, 0x2222, 1) + b"hello"
icmp = icmp[:2] + struct.pack("!H", csum(icmp)) + icmp[4:]
send(ip4(1, icmp, 0x1003))
time.sleep(0.3)

print("sent: tcp-syn udp icmp-echo")
