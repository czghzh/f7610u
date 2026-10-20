#!/usr/bin/env python3
"""Toggle the serial modem control lines, in case DTR/RTS is wired to RESET."""
import fcntl
import struct
import sys
import time

DEV = "/dev/ttyUSB0"
TIOCM_DTR = 0x002
TIOCM_RTS = 0x004
TIOCMBIS = 0x5416
TIOCMBIC = 0x5417


def setlines(fd, bits):
    fcntl.ioctl(fd, TIOCMBIS, struct.pack("i", bits))


def clrlines(fd, bits):
    fcntl.ioctl(fd, TIOCMBIC, struct.pack("i", bits))


fd = open(DEV, "r+b", buffering=0)
print("toggling DTR/RTS sequence")
for i in range(3):
    clrlines(fd, TIOCM_DTR | TIOCM_RTS)
    time.sleep(0.4)
    setlines(fd, TIOCM_DTR | TIOCM_RTS)
    time.sleep(0.6)
    print("  cycle", i + 1)
clrlines(fd, TIOCM_DTR)
setlines(fd, TIOCM_DTR)
print("done")
