#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""在原厂固件上，用我们驱动的「O1 装帧 -> 切 O3」方法手工发一帧 SN，
看 OLT 是否回 03/05（表现为 0x17059400 变成 0x….0005）。"""
import socket, time, sys

HOST = "192.168.1.1"
FRAME = [0x03FF0100, 0x534B5957, 0xA8D40BA7, 0x0000ADE5,
         0, 0, 0, 0, 0, 0, 0x66E17D0E, 0x35D7C55F]


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


def send(s, cmd, wait=0.8):
    s.sendall(cmd.encode() + b"\r\n")
    time.sleep(wait)
    return drain(s, max(0.5, wait)).decode("utf-8", "replace").replace("\r", "")


def val(txt):
    i = txt.rfind(":")
    return txt[i + 1:].strip() if i > 0 else "?"


def main():
    s = socket.create_connection((HOST, 23), 6)
    time.sleep(0.6)
    b = drain(s, 1.0)
    if b"ogin" in b:
        s.sendall(b"root\r\n"); time.sleep(0.8); b += drain(s, 1.0)
    if b"assword" in b:
        s.sendall(b"Zte521\r\n"); time.sleep(1.2); b += drain(s, 1.2)
    s.sendall(b"\r\n"); time.sleep(0.4); drain(s, 0.6)

    print("state:", val(send(s, "devmem2 0x17059400")))
    print("5947c:", val(send(s, "devmem2 0x1705947c")))
    print("599bc:", val(send(s, "devmem2 0x170599bc")))

    # 1) O1 + 清 pending
    send(s, "devmem2 0x17059400 w 0x03ff0001")
    send(s, "devmem2 0x17059908 w 0x00000000")
    # 2) 灌 12 字
    for i, w in enumerate(FRAME):
        a = 0x17059B08 + i * 4
        send(s, "devmem2 0x%08x w 0x%08x" % (a, w), 0.5)
    # 3) 切 O3（原厂驱动此刻是 O1，不会被它干扰）
    send(s, "devmem2 0x17059400 w 0x03ff0003")
    # 4) 也点一下 SN 引擎触发位
    send(s, "devmem2 0x170599bc w 0x00000009", 0.5)

    for k in range(12):
        time.sleep(2)
        st = val(send(s, "devmem2 0x17059400", 0.6))
        ctl = val(send(s, "devmem2 0x170599bc", 0.4))
        print("t=%2ds state=%s snctl=%s" % (2 * (k + 1), st, ctl))
        if st.upper().endswith("00005"):
            print("*** OLT 回了！state 已到 O5 ***")
            break
    s.close()


main()
