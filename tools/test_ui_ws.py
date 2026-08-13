#!/usr/bin/env python3
"""Тест WS-канала веб-интерфейса: auth -> subscribe -> приём кадров."""
import base64
import json
import os
import socket
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
from simulate_client import SimWs  # переиспользуем минимальный WS-клиент


def main():
    import urllib.request

    req = urllib.request.Request(
        "http://127.0.0.1:8080/api/login",
        data=json.dumps({"user": "admin", "pass": "admin123"}).encode(),
        headers={"Content-Type": "application/json"},
    )
    token = json.loads(urllib.request.urlopen(req).read())["token"]

    ws = SimWs("127.0.0.1", 8080, "/ws/ui")
    ws.send_text(json.dumps({"type": "auth", "token": token}))
    ws.send_text(json.dumps({"type": "subscribe", "cid": "pc1", "stream": "screen"}))

    got_frame = False
    deadline = time.time() + 8
    while time.time() < deadline:
        ws.sock.settimeout(1.0)
        try:
            fr = ws.recv_frame()
        except socket.timeout:
            continue
        if fr is None:
            break
        op, payload = fr
        if op == 0x9:
            ws.send_frame(0xA, payload)
            continue
        if op != 0x1:
            continue
        evt = json.loads(payload.decode())
        if evt.get("type") == "clients":
            print("[ui-ws] clients:", [c["id"] for c in evt["clients"]])
        elif evt.get("type") == "frame":
            data = base64.b64decode(evt["data"])
            print(f"[ui-ws] frame: cid={evt['cid']} stream={evt['stream']} "
                  f"bytes={len(data)} -> {evt['cid']}_{evt['stream']}.jpg")
            with open("ui_frame_test.jpg", "wb") as f:
                f.write(data)
            got_frame = True

    print("[ui-ws]", "OK: кадры получены" if got_frame else "FAIL: кадров нет")
    ws.sock.close()
    return 0 if got_frame else 1


if __name__ == "__main__":
    sys.exit(main())
