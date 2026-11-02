import http.server, json, os, sys, threading
# Fake LRCLIB. Mode "drop" / "err": /get is always 404 and /search is broken (closed connection, or HTTP 500) for the first N calls, then
# answers with one good entry. Mode "get503": /get answers 503 for the first N calls, then the good entry; /search always breaks (500).
# Every request path is appended to the file in $FAKE_LOG, so a test can see which requests were made.
mode, n_bad = sys.argv[1], int(sys.argv[2])
calls = {"search": 0, "get": 0}
LOG = os.environ.get("FAKE_LOG")
ENTRY = {"id": 1, "trackName": "Song", "artistName": "Artist", "albumName": "Album", "duration": 200.0, "instrumental": False,
         "plainLyrics": "a\nb", "syncedLyrics": "[00:01.00] a\n[00:03.00] b"}
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        if LOG:
            with open(LOG, "a") as f: f.write(self.path.split("?")[0] + "\n")
        if self.path.startswith("/api/get"):
            calls["get"] += 1
            if mode == "get503":
                if calls["get"] <= n_bad:
                    self.send_response(503); self.end_headers(); return
                body = json.dumps(ENTRY).encode()
                self.send_response(200); self.send_header("content-type", "application/json"); self.send_header("content-length", str(len(body))); self.end_headers(); self.wfile.write(body); return
            self.send_response(404); self.end_headers(); self.wfile.write(b"{}"); return
        if mode == "get503":
            self.send_response(500); self.end_headers(); return
        calls["search"] += 1
        if calls["search"] <= n_bad:
            if mode == "drop": self.connection.close(); return
            self.send_response(500); self.end_headers(); return
        body = json.dumps([ENTRY]).encode()
        self.send_response(200); self.send_header("content-type", "application/json"); self.send_header("content-length", str(len(body))); self.end_headers(); self.wfile.write(body)
srv = http.server.ThreadingHTTPServer(("127.0.0.1", int(sys.argv[3])), H)
srv.serve_forever()
