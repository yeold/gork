"""Checks for a gork built --with-openssl: TLS of its own, with no relay.

Usage:  python3 testing/tlstest.py ./gork
Needs the openssl command, to make a certificate for localhost.  Exits 0
only if every check passes.

A fake Messages API listens on TLS with that certificate.  gork reaches
it with no relay set, trusting the certificate through SSL_CERT_FILE, in
a reply taken whole and in a streamed one; and refuses it when it is not
trusted, and when the name does not match.
"""
import http.server, json, os, ssl, subprocess, sys, tempfile, threading

BINARY = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "./gork")
PORT = 8096
hosts = []


class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def do_POST(self):
        req = json.loads(self.rfile.read(int(self.headers["content-length"])))
        hosts.append(self.headers["host"])
        if req.get("stream"):
            ev = [{"type": "message_start", "message": {
                      "id": "m", "role": "assistant", "content": [],
                      "usage": {"input_tokens": 9, "output_tokens": 1}}},
                  {"type": "content_block_start", "index": 0,
                   "content_block": {"type": "text", "text": ""}}]
            ev += [{"type": "content_block_delta", "index": 0,
                    "delta": {"type": "text_delta", "text": t}}
                   for t in ("streamed ", "over ", "TLS")]
            ev += [{"type": "content_block_stop", "index": 0},
                   {"type": "message_delta", "delta": {"stop_reason": "end_turn"},
                    "usage": {"output_tokens": 3}},
                   {"type": "message_stop"}]
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            for e in ev:                    # an event per TLS record
                self.wfile.write(b"event: %s\ndata: %s\n\n"
                                 % (e["type"].encode(), json.dumps(e).encode()))
                self.wfile.flush()
            return
        body = json.dumps({"id": "m", "type": "message", "role": "assistant",
                           "stop_reason": "end_turn",
                           "content": [{"type": "text", "text": "hello over TLS"}],
                           "usage": {"input_tokens": 9, "output_tokens": 3}}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


def run(tmp, config, **env_extra):
    env = dict(os.environ, HOME=tmp, ANTHROPIC_API_KEY="sk-test")
    for k in ("SSL_CERT_FILE", "SSL_CERT_DIR", "AGENT_HOST"):
        env.pop(k, None)
    env.update(env_extra)
    conf = os.path.join(tmp, "gork.conf")
    with open(conf, "w") as f:
        f.write(config + "retries = 0\ntimeout = 10\n")
    os.chmod(conf, 0o600)
    env["GORK_CONFIG"] = conf
    return subprocess.run([BINARY, "hi"], cwd=tmp, capture_output=True,
                          text=True, timeout=30, env=env)


def main():
    with tempfile.TemporaryDirectory() as tmp:
        cert, key = os.path.join(tmp, "cert.pem"), os.path.join(tmp, "key.pem")
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048",
                        "-nodes", "-keyout", key, "-out", cert, "-days", "2",
                        "-subj", "/CN=localhost",
                        "-addext", "subjectAltName=DNS:localhost"],
                       check=True, capture_output=True)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(cert, key)
        srv = http.server.ThreadingHTTPServer(("127.0.0.1", PORT), H)
        srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
        srv.daemon_threads = True
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        direct = "host = localhost:%d\n" % PORT
        try:
            p = run(tmp, direct, SSL_CERT_FILE=cert)
            assert p.returncode == 0 and p.stdout == "hello over TLS\n", \
                (p.stdout, p.stderr)
            assert hosts[-1] == "localhost:%d" % PORT, hosts
            print("ok: no relay: TLS straight to host, its certificate checked")

            p = run(tmp, direct + "stream = yes\n", SSL_CERT_FILE=cert)
            assert p.returncode == 0 and "streamed over TLS" in p.stdout, \
                (p.stdout, p.stderr)
            print("ok: a streamed reply over TLS")

            p = run(tmp, direct, SSL_CERT_FILE=os.path.join(tmp, "none"),
                    SSL_CERT_DIR=os.path.join(tmp, "none"))
            assert p.returncode != 0 and "certificate" in p.stderr, p.stderr
            n = len(hosts)
            p = run(tmp, "relay = tls:127.0.0.1:%d\nhost = localhost\n" % PORT,
                    SSL_CERT_FILE=cert)
            assert p.returncode != 0 and "mismatch" in p.stderr, p.stderr
            assert len(hosts) == n, "a request went to a server not trusted"
            print("ok: an untrusted certificate, and a name that does not "
                  "match, refused")
        finally:
            srv.shutdown()
    print("all TLS checks passed")


if __name__ == "__main__":
    main()
