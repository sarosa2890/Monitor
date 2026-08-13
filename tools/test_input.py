#!/usr/bin/env python3
"""Тест транзита удалённого управления: UI WS -> сервер -> клиент (input события)."""
import json
import os
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
from simulate_client import SimWs


def main(target, events):
    import urllib.request

    req = urllib.request.Request(
        "http://127.0.0.1:8080/api/login",
        data=json.dumps({"user": "admin", "pass": "admin123"}).encode(),
        headers={"Content-Type": "application/json"},
    )
    token = json.loads(urllib.request.urlopen(req).read())["token"]

    ws = SimWs("127.0.0.1", 8080, "/ws/ui")
    ws.send_text(json.dumps({"type": "auth", "token": token}))
    time.sleep(0.5)

    for ev in events:
        ev = {"type": "input", "cid": target, **ev}
        ws.send_text(json.dumps(ev))
        print("[input-test] sent:", ev)
        time.sleep(0.2)

    ws.sock.close()
    return 0


if __name__ == "__main__":
    target = sys.argv[1] if len(sys.argv) > 1 else "pc1"
    events = [
        {"kind": "mousemove", "x": 800, "y": 450},
        {"kind": "mousedown", "x": 800, "y": 450, "button": "left"},
        {"kind": "mouseup", "button": "left"},
        {"kind": "mousemove", "x": 400, "y": 300},
        {"kind": "wheel", "delta": -120},
        {"kind": "key", "key": "a", "down": True},
        {"kind": "key", "key": "a", "down": False},
        {"kind": "key", "key": "Enter", "down": True},
        {"kind": "key", "key": "Enter", "down": False},
        {"kind": "key", "key": "й", "down": True},
        {"kind": "key", "key": "й", "down": False},
    ]
    sys.exit(main(target, events))