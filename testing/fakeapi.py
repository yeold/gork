import http.server, json
class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"
    def do_POST(self):
        n = int(self.headers.get('content-length', 0))
        req = self.rfile.read(n)
        print("PATH:", self.path)
        print("HOST HDR:", self.headers.get('host'))
        print("KEY HDR:", self.headers.get('x-api-key'))
        print("VER HDR:", self.headers.get('anthropic-version'))
        print("BODY:", req.decode())
        out = json.dumps({"id":"msg_01","content":[{"type":"text","text":"pong"}],"stop_reason":"end_turn"}).encode()
        self.send_response(200); self.send_header("content-type","application/json")
        self.send_header("content-length", str(len(out))); self.end_headers()
        self.wfile.write(out)
    def log_message(self, *a): pass
http.server.HTTPServer(("127.0.0.1", 8099), H).serve_forever()