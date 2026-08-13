# Деплой FoxMonitor на Railway

Vercel не подходит (serverless: нет долгоживущих WebSocket-процессов и диска).
Railway — правильный выбор: постоянный контейнер, WebSocket, volume для данных.

## Что уже сделано в репозитории

- `server/Dockerfile` — образ: python 3.12, uvicorn, healthcheck.
- `server/.dockerignore` — не тащим кэш/конфиги/данные в образ.
- `server/server.py` — читает `PORT`, `HOST`, `CLIENT_KEY`, `ADMIN_USER`, `ADMIN_PASS` из env.
- `server/server.py` — добавлен `GET /health` (healthcheck Railway).
- `railway.json` — builder `DOCKERFILE`, `rootDirectory: server` (контекст сборки),
  healthcheck + restart policy.
- `client/src/wsclient.*` — поддержка wss:// (TLS 1.2/1.3 через Schannel).
- `client/config.ini` — флаг `tls = 1` (wss, порт 443 автоматически).

## Шаг 1. Развернуть сервер на Railway

Вариант А — через GitHub (рекомендуется):

1. Залей репозиторий на GitHub.
2. Railway → New Project → Deploy from GitHub repo → выбрать репозиторий.
3. Railway сам увидит `railway.json` и соберёт Dockerfile.
4. Переменные окружения (Project → Variables):
   | Имя | Значение |
   |---|---|
   | `CLIENT_KEY` | тот же ключ, что в `client/config.ini` (например `secret123`) |
   | `ADMIN_USER` | `admin` |
   | `ADMIN_PASS` | твой пароль (НЕ admin123, если сервер в интернете!) |
5. Получишь публичный домен вида `foxmon-production-XXXX.up.railway.app` (HTTPS).

Вариант Б — через CLI:

```
railway login
railway init
railway up --detach
railway variables set CLIENT_KEY=... ADMIN_USER=admin ADMIN_PASS=...
railway domain
```

## Шаг 2. Persistent Volume (записи и скриншоты)

Данные пишутся в `server/data/`. На Railway ФС контейнера эфемерна —
нужен volume:

1. Railway → Project → Volumes → New Volume.
2. Mount path: `/app/data` (путь, куда примонтировать).
3. В деплой можно не привязывать — все деплойменты проекта делят volume
   (пока он один).

Без volume записи и скриншоты пропадут при передеплое.

## Шаг 3. Подключить Windows-клиент (wss)

В `client/config.ini`:

```ini
host = foxmon-production-XXXX.up.railway.app   ; твой домен Railway
port = 443         ; или удали строку port (wss сам подставит 443)
tls = 1            ; wss:// через Schannel

client_id = pc1
client_key = secret123   ; ТОТ ЖЕ, что CLIENT_KEY на Railway
```

Пересобери клиент (Visual Studio 2022, Release x64) и запусти.

Проверка в логе клиента:

```
[foxmon] pc1 -> foxmon-...up.railway.app:443 (wss)
[net] tls: handshake ok (hdr=... trl=... max=...)
[net] connected
```

## Шаг 4. Проверка

- `GET https://ДОМЕН/health` → `{"ok":true,"clients":0}` (пока клиент не подключён).
- Открой `https://ДОМЕН` в браузере, войди (admin / ADMIN_PASS).
- Клиент подключится по `wss://ДОМЕН/ws/client` и появится в карточках.

## Примечания

- **Smart App Control / Defender**: Windows 11 может блокировать запуск
  несобранного exe (политика CI). Решение: Settings → Privacy & security →
  Windows Security → App & browser control → Smart App Control → Off.
  Или добавь папку `client\run` в исключения Defender (Exclusions).
- WebSocket-соединения на Railway живут долго, ограничений нет — стрим
  экрана/камеры работает как на локалке.
- Медленный/тяжёлый трафик (10+ fps экран) на бесплатном тарифе может
  лимитироваться — при необходимости уменьши `screen_fps`.
- Пароли в `server/config.json` локально менять не надо: env на Railway
  имеет приоритет.
