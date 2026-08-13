#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Симулятор C++ клиента (чистый stdlib, без зависимостей).
Повторяет протокол FoxMonitor: hello -> бинарные кадры экрана/камеры,
отвечает на команду screenshot кадром kind=3.

Использование:
    python simulate_client.py --id pc1 --key secret123 --host 127.0.0.1 --port 8080
"""
import argparse
import base64
import json
import os
import random
import socket
import struct
import sys
import time

TINY_JPEG = base64.b64decode(
    "/9j/4AAQSkZJRgABAQEAYABgAAD/2wBDAAgGBgcGBQgHBwcJCQgKDBQNDAsLDBkSEw8UHRofHh0aHBwgJC4nICIsIxwcKDcpLDAxNDQ0Hyc5PTgyPC4zNDL/wAALCAABAAEBAREA/8QAFAABAAAAAAAAAAAAAAAAAAAACf/EABQQAQAAAAAAAAAAAAAAAAAAAAD/2gAIAQEAAD8AVN//2Q=="
)


class SimWs:
    def __init__(self, host, port, path):
        self.sock = socket.create_connection((host, port), timeout=10)
        key = base64.b64encode(os.urandom(16)).decode()
        req = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {host}:{port}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n"
        )
        self.sock.sendall(req.encode())
        resp = b""
        while b"\r\n\r\n" not in resp:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError("handshake failed")
            resp += chunk
        if b" 101 " not in resp:
            raise RuntimeError("handshake rejected: " + resp.decode(errors="replace")[:200])

    def _recvn(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                return None
            buf += chunk
        return buf

    def send_frame(self, opcode, payload):
        mask = os.urandom(4)
        hdr = bytes([0x80 | opcode])
        n = len(payload)
        if n <= 125:
            hdr += bytes([0x80 | n])
        elif n <= 0xFFFF:
            hdr += bytes([0x80 | 126]) + struct.pack(">H", n)
        else:
            hdr += bytes([0x80 | 127]) + struct.pack(">Q", n)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self.sock.sendall(hdr + mask + masked)

    def send_text(self, s):
        self.send_frame(0x1, s.encode())

    def send_binary_kind(self, kind, data):
        pkt = bytes([kind]) + struct.pack(">I", len(data)) + data
        self.send_frame(0x2, pkt)

    def recv_frame(self):
        hdr = self._recvn(2)
        if hdr is None:
            return None
        opcode = hdr[0] & 0x0F
        masked = bool(hdr[1] & 0x80)
        ln = hdr[1] & 0x7F
        if ln == 126:
            ext = self._recvn(2)
            if ext is None:
                return None
            ln = struct.unpack(">H", ext)[0]
        elif ln == 127:
            ext = self._recvn(8)
            if ext is None:
                return None
            ln = struct.unpack(">Q", ext)[0]
        mask = None
        if masked:
            mask = self._recvn(4)
            if mask is None:
                return None
        payload = self._recvn(ln)
        if payload is None:
            return None
        if mask:
            payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        return opcode, payload


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--id", default="pc1")
    ap.add_argument("--key", default="secret123")
    ap.add_argument("--no-camera", action="store_true")
    args = ap.parse_args()

    path = f"/ws/client?name={args.id}&key={args.key}"
    ws = SimWs(args.host, args.port, path)
    ws.send_text(json.dumps({"type": "hello", "id": args.id, "key": args.key}))
    print("[sim] connected, hello sent")

    run = {"screen": True, "camera": not args.no_camera}
    ws.send_text(json.dumps({
        "type": "status",
        "streams": {"screen": run["screen"], "camera": run["camera"]}}))
    print("[sim] status sent")

    last_screen = time.time()
    last_camera = time.time()

    while True:
        ws.sock.settimeout(0.2)
        try:
            fr = ws.recv_frame()
        except socket.timeout:
            fr = None
        if fr is not None:
            opcode, payload = fr
            if opcode == 0x9:  # ping
                ws.send_frame(0xA, payload)
            elif opcode == 0x1:  # text command
                try:
                    evt = json.loads(payload.decode())
                except Exception:
                    continue
                print("[sim] cmd:", evt)
                if evt.get("command") == "screenshot":
                    stream = evt.get("stream", "screen")
                    ws.send_binary_kind(3, bytes([1 if stream == "screen" else 2]) + TINY_JPEG)
                    print("[sim] screenshot sent:", stream)
                elif evt.get("command") == "stop":
                    run[evt.get("stream", "screen")] = False
                    ws.send_text(json.dumps({
                        "type": "status",
                        "streams": {"screen": run["screen"], "camera": run["camera"]}}))
                elif evt.get("command") == "start":
                    run[evt.get("stream", "screen")] = True
                    ws.send_text(json.dumps({
                        "type": "status",
                        "streams": {"screen": run["screen"], "camera": run["camera"]}}))
            elif opcode == 0x8:
                print("[sim] server closed connection")
                break

        now = time.time()
        if run["screen"] and now - last_screen >= 1.0:
            ws.send_binary_kind(1, TINY_JPEG)
            last_screen = now
        if run["camera"] and now - last_camera >= 2.0:
            ws.send_binary_kind(2, TINY_JPEG)
            last_camera = now


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
