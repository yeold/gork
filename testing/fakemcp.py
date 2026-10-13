"""A small stdio MCP server for looptest.py.

Deliberately awkward: prints a non-JSON line, sends a notification and a ping
request before answering, and splits tools/list across two pages.
"""
import json, os, sys

def send(obj):
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()

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

print("fakemcp starting up (not JSON)", flush=True)
for line in sys.stdin:
    msg = json.loads(line)
    method, mid = msg.get("method"), msg.get("id")
    if mid is None:                                 # a notification
        if method == "notifications/cancelled":
            note("cancelled %s" % (msg["params"]["requestId"] == slow))
        continue
    if method is None:
        continue                                    # our ping's answer
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
