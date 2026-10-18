import http.server, json, sys, threading
# Fake LRCLIB. /get is always 404. /search is broken (closed connection, then HTTP 500) for the first N calls, then answers with one good entry.
mode, n_bad = sys.argv[1], int(sys.argv[2])
calls = {"search": 0}
ENTRY = {"id": 1, "trackName": "Song", "artistName": "Artist", "albumName": "Album", "duration": 200.0, "instrumental": False,
         "plainLyrics": "a\nb", "syncedLyrics": "[00:01.00] a\n[00:03.00] b"}
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        if self.path.startswith("/api/get"):
            self.send_response(404); self.end_headers(); self.wfile.write(b"{}"); return
        calls["search"] += 1
        if calls["search"] <= n_bad:
            if mode == "drop": self.connection.close(); return
            self.send_response(500); self.end_headers(); return
        body = json.dumps([ENTRY]).encode()
        self.send_response(200); self.send_header("content-type", "application/json"); self.send_header("content-length", str(len(body))); self.end_headers(); self.wfile.write(body)
srv = http.server.ThreadingHTTPServer(("127.0.0.1", int(sys.argv[3])), H)
srv.serve_forever()
