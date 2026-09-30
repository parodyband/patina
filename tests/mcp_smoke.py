"""MCP smoke test: handshake, tools/list, and concurrent tools/call over stdio."""
import json, subprocess, sys, time, os

BIN = sys.argv[1] if len(sys.argv) > 1 else "build/patina"
p = subprocess.Popen([BIN, "mcp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)

def send(msg):
    p.stdin.write(json.dumps(msg) + "\n"); p.stdin.flush()

def recv():
    return json.loads(p.stdout.readline())

send({"jsonrpc": "2.0", "id": 0, "method": "initialize", "params": {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "smoke", "version": "1"}}})
init = recv()
assert init["result"]["serverInfo"]["name"] == "patina", init
send({"jsonrpc": "2.0", "method": "notifications/initialized"})
send({"jsonrpc": "2.0", "id": 1, "method": "tools/list"})
tools = [t["name"] for t in recv()["result"]["tools"]]
print("tools:", tools)

# fire several calls at once; responses may come back in any order
t0 = time.time()
calls = {
    10: ("render", {"project": "work/crate.patina.json", "views": "iso", "size": 256}),
    11: ("render", {"project": "work/barrel.patina.json", "views": "iso", "size": 256}),
    12: ("render", {"project": "work/suzanne.patina.json", "views": "front", "size": 256}),
    13: ("library", {"topic": "smart_materials"}),
    14: ("validate", {"project": "work/barrel.patina.json"}),
    15: ("edit", {"project": "work/nope.patina.json", "ops": []}),  # expected error
}
for cid, (name, args) in calls.items():
    send({"jsonrpc": "2.0", "id": cid, "method": "tools/call", "params": {"name": name, "arguments": args}})
results = {}
while len(results) < len(calls):
    r = recv()
    results[r["id"]] = r
    kinds = [c["type"] for c in r["result"]["content"]]
    print(f"id {r['id']} ({calls[r['id']][0]}): isError={r['result']['isError']} content={kinds} at {time.time()-t0:.2f}s")
assert all(not results[i]["result"]["isError"] for i in (10, 11, 12, 13, 14))
assert results[15]["result"]["isError"]
assert results[10]["result"]["content"][1]["type"] == "image"
p.stdin.close(); p.wait(timeout=10)
print("MCP smoke test passed")
