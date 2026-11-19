#!/usr/bin/env python3
"""Drive spklib-host's line protocol: send requests, print replies.

Usage: python -I c1-probe.py <spklib-host.exe> <method> <params-json> [more method/json pairs...]
Reads nothing else; one process per run (host.shutdown at the end).
"""
import json, subprocess, sys, os

host = sys.argv[1]
proc = subprocess.Popen([host], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                        stderr=subprocess.DEVNULL, text=True, encoding="utf-8")
ready = proc.stdout.readline()
print("READY:", ready.strip())

pairs = list(zip(sys.argv[2::2], sys.argv[3::2]))
next_id = 1
for method, params in pairs:
    req = {"id": next_id, "method": method, "params": json.loads(params)}
    next_id += 1
    proc.stdin.write(json.dumps(req, ensure_ascii=False) + "\n")
    proc.stdin.flush()
    reply = proc.stdout.readline()
    print(f"--- {method}")
    print(reply.strip())

proc.stdin.write(json.dumps({"id": next_id, "method": "host.shutdown", "params": {}}) + "\n")
proc.stdin.flush()
try:
    proc.wait(timeout=5)
except subprocess.TimeoutExpired:
    proc.kill()
