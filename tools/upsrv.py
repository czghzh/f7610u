#!/usr/bin/env python3
"""Tiny upload receiver: POST /<name> saves body to ./up_<name>."""
import http.server
import os
import socketserver

OUT = os.path.dirname(os.path.abspath(__file__))


class H(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n)
        name = self.path.strip("/").replace("/", "_") or "noname"
        path = os.path.join(OUT, "up_" + name)
        with open(path, "wb") as f:
            f.write(body)
        self.send_response(200)
        self.end_headers()
        self.wfile.write(b"ok %d\n" % len(body))
        print("saved", path, len(body), flush=True)

    def log_message(self, *a):
        pass


class S(socketserver.ThreadingTCPServer):
    allow_reuse_address = True


if __name__ == "__main__":
    S(("192.168.1.100", 8001), H).serve_forever()
