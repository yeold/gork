"""A small MCP server for looptest.py, on stdio or, with --http PORT, over
streamable HTTP.

Deliberately awkward: prints a non-JSON line, sends a notification and a ping
request before answering, and splits tools/list across two pages.  Over HTTP
it hands out a session id and wants it back, answers with server-sent events
when there is more than the answer to send, and logs what it was sent.
"""
import json, os, sys

out = []        # what to send: stdout, or the HTTP reply being made

def send(obj):
    out.append(obj)

def note(text):
    """For the test that watches a call being cancelled."""
    if os.environ.get("FAKEMCP_LOG"):
        with open(os.environ["FAKEMCP_LOG"], "a") as f:
            f.write(text + "\n")

slow = None
TOOLS = [
    {"name": "echo", "description": "Echo text back.",
     "inputSchema": {"type": "object", "properties": {"text": {"type": "string"}},
                     "required": ["text"]}},
    {"name": "fail", "description": "Always fails.", "inputSchema": {"type": "object"}},
]

def handle(msg):
    """Act on one message; what to send back goes to `out`."""
    global slow
    method, mid = msg.get("method"), msg.get("id")
    if mid is None:                                 # a notification
        if method == "notifications/cancelled":
            note("cancelled %s" % (msg["params"]["requestId"] == slow))
        return
    if method is None:
        note("answer %s" % mid)                     # our ping's answer
        return
    if method == "initialize":
        send({"jsonrpc": "2.0", "method": "notifications/message",
              "params": {"level": "info", "data": "hello"}})
        send({"jsonrpc": "2.0", "id": "p1", "method": "ping"})
        send({"jsonrpc": "2.0", "id": mid, "result": {
            "protocolVersion": msg["params"]["protocolVersion"],
            "capabilities": {"tools": {}},
            "serverInfo": {"name": "fakemcp", "version": "1"}}})
    elif method == "tools/list":
        page = 1 if msg.get("params", {}).get("cursor") == "p2" else 0
        result = {"tools": [TOOLS[page]]}
        if page == 0:
            result["nextCursor"] = "p2"
        send({"jsonrpc": "2.0", "id": mid, "result": result})
    elif method == "tools/call":
        p = msg["params"]
        if p["name"] == "echo" and p["arguments"]["text"] == "slow":
            slow = mid                              # never answered
            note("started")
        elif p["name"] == "echo":
            send({"jsonrpc": "2.0", "id": mid, "result": {"content": [
                {"type": "text", "text": "echo: " + p["arguments"]["text"]},
                {"type": "image", "data": "", "mimeType": "image/png"}]}})
        else:
            send({"jsonrpc": "2.0", "id": mid, "result": {
                "content": [{"type": "text", "text": "it broke"}], "isError": True}})
    else:
        send({"jsonrpc": "2.0", "id": mid,
              "error": {"code": -32601, "message": "no such method"}})


def serve_http(port):
    import http.server, threading, time
    lock = threading.Lock()

    class H(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def do_POST(self):
            msg = json.loads(self.rfile.read(int(self.headers["content-length"])))
            sid = self.headers.get("Mcp-Session-Id")
            if msg.get("method") != "initialize":
                if sid != "s-1":
                    note("no session")
                    self.send_response(400)
                    self.end_headers()
                    return
                note("proto %s token %s" % (self.headers.get("MCP-Protocol-Version"),
                                            self.headers.get("X-Token")))
            with lock:
                del out[:]
                handle(msg)
                reply = list(out)
            if msg.get("id") is None or msg.get("method") is None:
                self.send_response(202)             # notification or answer
                self.end_headers()
                return
            if not reply:                           # the slow one: hold on
                time.sleep(30)
                return
            self.send_response(200)
            if msg.get("method") == "initialize":
                self.send_header("Mcp-Session-Id", "s-1")
            if len(reply) > 1:
                self.send_header("Content-Type", "text/event-stream")
                self.end_headers()
                for m in reply:
                    self.wfile.write(b"event: message\r\ndata: %s\r\n\r\n"
                                     % json.dumps(m).encode())
            else:
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(json.dumps(reply[0]).encode())

        def do_DELETE(self):
            note("deleted %s" % self.headers.get("Mcp-Session-Id"))
            self.send_response(200)
            self.end_headers()

        def log_message(self, *a):
            pass

    http.server.ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()


if len(sys.argv) > 2 and sys.argv[1] == "--http":
    serve_http(int(sys.argv[2]))
    sys.exit(0)

print("fakemcp starting up (not JSON)", flush=True)
for line in sys.stdin:
    del out[:]
    handle(json.loads(line))
    for m in out:
        sys.stdout.write(json.dumps(m) + "\n")
    sys.stdout.flush()
