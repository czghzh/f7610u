#!/usr/bin/env python3
"""Raw L4 probe v2 - all L4 checksums CORRECT.

Sends three frames that differ ONLY in the IP protocol field / L4 header,
so the device's behaviour isolates "protocol-based filtering" from
"checksum validation".
"""
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


s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW)
s.bind((IFACE, 0))


def send(ip_pkt):
    frame = DST_MAC + SRC_MAC + struct.pack("!H", 0x0800) + ip_pkt
    if len(frame) < 60:
        frame += b"\x00" * (60 - len(frame))
    s.send(frame)


# --- TCP SYN, protocol 6, checksum CORRECT -------------------------------
tcp = struct.pack("!HHIIBBHHH", 40011, 80, 0x11223344, 0, 5 << 4, 0x02, 65535, 0, 0)
tcp = tcp[:16] + struct.pack("!H", l4_csum(6, tcp)) + tcp[18:]
send(ip4(6, tcp, 0x2001))
time.sleep(0.3)

# --- UDP, protocol 17, checksum CORRECT ----------------------------------
udp_payload = b"abcd"
udp = struct.pack("!HHHH", 40012, 53, 8 + len(udp_payload), 0) + udp_payload
udp = udp[:6] + struct.pack("!H", l4_csum(17, udp)) + udp[8:]
send(ip4(17, udp, 0x2002))
time.sleep(0.3)

# --- ICMP echo, protocol 1, checksum CORRECT (control) -------------------
icmp = struct.pack("!BBHHH", 8, 0, 0, 0x3333, 1) + b"hello"
icmp = icmp[:2] + struct.pack("!H", csum(icmp)) + icmp[4:]
send(ip4(1, icmp, 0x2003))
time.sleep(0.3)

print("sent: tcp-syn(ck-ok) udp(ck-ok) icmp(ck-ok)")
