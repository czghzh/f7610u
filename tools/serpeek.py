import serial, sys, time
try:
    s = serial.Serial('/dev/ttyUSB0', 115200, timeout=1)
except Exception as e:
    print("OPEN_FAIL:", e); sys.exit(1)
s.reset_input_buffer()
s.write(b'\r')
buf = b''
t0 = time.time()
while time.time() - t0 < 6:
    d = s.read(4096)
    if d:
        buf += d
    else:
        s.write(b'\r')
s.close()
txt = buf.decode('utf-8', 'replace')
print(txt[-1500:] if txt else "(no output)")
