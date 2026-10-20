import json
import urllib.request
import http.cookiejar

BASE = "http://192.168.1.1"
cj = http.cookiejar.CookieJar()
op = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(cj))

op.open(urllib.request.Request(
    BASE + "/cgi-bin/luci/",
    data=b"luci_username=root&luci_password="))

sid = None
for c in cj:
    if c.name.startswith("sysauth"):
        sid = c.value
print("sid:", sid)

for field in ("serdes_reset", "serdes_pll", "serdes_cdr", "work_mode", "rx_signal"):
    body = json.dumps({
        "jsonrpc": "2.0", "id": 1, "method": "call",
        "params": [sid, "file", "exec",
                   {"command": "/usr/libexec/pon-board-identity",
                    "params": ["read", "pon", field]}],
    }).encode()
    req = urllib.request.Request(BASE + "/cgi-bin/luci/admin/ubus", data=body,
                                 headers={"Content-Type": "application/json"})
    r = json.load(urllib.request.urlopen(req, timeout=20))
    res = r.get("result", [])
    out = res[1].get("stdout", "").strip() if len(res) > 1 and isinstance(res[1], dict) else r
    print("%-13s -> %s" % (field, out))
