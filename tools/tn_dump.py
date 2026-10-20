import socket, sys, os, time

HOST="192.168.1.1"; USER="root"; PASS="Zte521"
OUT=sys.argv[1] if len(sys.argv)>1 else "/home/wen/WorkBuddy/zx/f7610u/work/stock_dump"
CMDS=[
 ("kallsyms","cat /proc/kallsyms"),
 ("iomem","cat /proc/iomem"),
 ("modules","cat /proc/modules"),
 ("dmesg","dmesg"),
 ("cmdline","cat /proc/cmdline"),
 ("ver","cat /proc/version; cat /proc/cpuinfo | sed -n 1,20p"),
]
os.makedirs(OUT, exist_ok=True)

def rx(s, idle=1.5, limit=90):
    s.settimeout(idle); out=b""; t0=time.time()
    while time.time()-t0 < limit:
        try:
            d=s.recv(65536)
            if not d: break
            out+=d; t0=time.time()
        except socket.timeout:
            break
    return out

s=socket.socket(); s.settimeout(8); s.connect((HOST,23)); time.sleep(0.6)
buf=rx(s,1.0)
if b"ogin" in buf: s.sendall((USER+"\r\n").encode()); time.sleep(0.8); buf+=rx(s,1.0)
if b"assword" in buf: s.sendall((PASS+"\r\n").encode()); time.sleep(1.2); buf+=rx(s,1.5)
if b"enied" in buf or b"ncorrect" in buf:
    print("LOGIN FAILED"); sys.exit(2)

for name,cmd in CMDS:
    s.sendall(("cat /dev/null; "+cmd+"; echo __CDONE_"+name+"__\r\n").encode())
    data=b""; t0=time.time()
    while time.time()-t0 < 180:
        chunk=rx(s, 2.0)
        data+=chunk
        if ("__CDONE_"+name+"__").encode() in data: break
    open(os.path.join(OUT,name+".txt"),"wb").write(data)
    print("%-10s %8d bytes" % (name, len(data)))
s.close()
