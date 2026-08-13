// Сбор системной информации для вкладки «Инфо»: ПК, мониторы, местоположение.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "sysinfo.h"
#include "config.h"

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

// ------------------------------------------------------ JSON-утилиты ------
static void jsonEscape(const std::wstring& s, std::string& out) {
    for (wchar_t c : s) {
        switch (c) {
            case L'"':  out += "\\\""; break;
            case L'\\': out += "\\\\"; break;
            case L'\n': out += "\\n";  break;
            case L'\r': out += "\\r";  break;
            case L'\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof(b), "\\u%04x", c);
                    out += b;
                } else if (c <= 0x7F) {
                    out += (char)c;
                } else {
                    // UTF-8 (консоль/сервер работают в UTF-8)
                    wchar_t u = c;
                    char utf8[8];
                    int n = WideCharToMultiByte(CP_UTF8, 0, &u, 1, utf8, sizeof(utf8), nullptr, nullptr);
                    out.append(utf8, n > 0 ? n : 0);
                }
        }
    }
}

static void jsonKey(std::string& out, const char* key) {
    out += "\"";
    out += key;
    out += "\":";
}

static std::wstring regStr(HKEY root, const wchar_t* path, const wchar_t* name) {
    HKEY k = nullptr;
    wchar_t buf[512] = {};
    DWORD sz = sizeof(buf);
    DWORD type = 0;
    if (RegOpenKeyExW(root, path, 0, KEY_READ, &k) != ERROR_SUCCESS) return L"";
    LONG r = RegQueryValueExW(k, name, nullptr, &type, (LPBYTE)buf, &sz);
    RegCloseKey(k);
    if (r != ERROR_SUCCESS || type != REG_SZ) return L"";
    return buf;
}

// ------------------------------------------------------------ инфо о ПК --
static void collectPc(std::string& out) {
    // Имя компьютера
    wchar_t cn[256] = {};
    DWORD cnSz = 256;
    GetComputerNameW(cn, &cnSz);

    // Версия ОС: читаем из реестра (GetVersionEx зашимана на Win8.1+)
    std::wstring prod = regStr(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"ProductName");
    std::wstring disp = regStr(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"DisplayVersion");
    std::wstring build = regStr(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"CurrentBuildNumber");
    std::wstring os = prod;
    if (!disp.empty()) { os += L" " + disp; }
    if (!build.empty()) { os += L" (сборка " + build + L")"; }

    // Процессор
    std::wstring cpu = regStr(HKEY_LOCAL_MACHINE,
        L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString");
    while (!cpu.empty() && (cpu.back() == L' ' || cpu.back() == L'\t')) cpu.pop_back();

    // Разрядность ОС
    BOOL is64 = FALSE;
    const char* arch = "x86";
    IsWow64Process(GetCurrentProcess(), &is64);
    if (is64 || sizeof(void*) == 8) arch = "x64";

    // Оперативная память
    MEMORYSTATUSEX mem = {};
    mem.dwLength = sizeof(mem);
    GlobalMemoryStatusEx(&mem);
    double ramTotal = mem.ullTotalPhys / (1024.0 * 1024.0 * 1024.0);
    double ramFree  = mem.ullAvailPhys / (1024.0 * 1024.0 * 1024.0);

    // Аптайм
    unsigned long long uptime = GetTickCount64() / 1000ull;

    jsonKey(out, "pc");
    out += "{\"name\":\"";
    std::string nameUtf8;
    jsonEscape(cn, nameUtf8);
    out += nameUtf8;
    out += "\",\"os\":\"";
    std::string osUtf8;
    jsonEscape(os, osUtf8);
    out += osUtf8;
    out += "\",\"arch\":\"";
    out += arch;
    out += "\",\"cpu\":\"";
    std::string cpuUtf8;
    jsonEscape(cpu, cpuUtf8);
    out += cpuUtf8;
    char b[128];
    snprintf(b, sizeof(b), "\",\"ram_total_gb\":%.2f,\"ram_free_gb\":%.2f,"
             "\"uptime_sec\":%llu}", ramTotal, ramFree, uptime);
    out += b;
}

// ---------------------------------------------------------- мониторы ------
struct MonInfo {
    std::wstring name;
    int x = 0, y = 0, w = 0, h = 0;
    bool primary = false;
};

static BOOL CALLBACK monEnum(HMONITOR hMon, HDC, LPRECT, LPARAM lp) {
    auto* list = reinterpret_cast<std::vector<MonInfo>*>(lp);
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMon, &mi)) return TRUE;
    MonInfo m;
    m.name = mi.szDevice;
    m.x = mi.rcMonitor.left;
    m.y = mi.rcMonitor.top;
    m.w = mi.rcMonitor.right - mi.rcMonitor.left;
    m.h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    m.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    list->push_back(m);
    return TRUE;
}

static void collectMonitors(std::string& out) {
    std::vector<MonInfo> list;
    EnumDisplayMonitors(nullptr, nullptr, monEnum, reinterpret_cast<LPARAM>(&list));

    // Размер всего виртуального стола
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    jsonKey(out, "monitors");
    out += "[";
    for (size_t i = 0; i < list.size(); i++) {
        const MonInfo& m = list[i];
        if (i) out += ",";
        out += "{\"name\":\"";
        std::string n;
        jsonEscape(m.name, n);
        out += n;
        char b[96];
        snprintf(b, sizeof(b), "\",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"primary\":%s}",
                 m.x, m.y, m.w, m.h, m.primary ? "true" : "false");
        out += b;
    }
    out += "]";

    char b[64];
    snprintf(b, sizeof(b), ",\"desktop\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}", vx, vy, vw, vh);
    out += b;
}

// ------------------------------------------------------------ главная ----
std::string collectSystemInfo(const Config& cfg) {
    std::string out = "{\"type\":\"info\",";
    collectPc(out);
    out += ",";
    collectMonitors(out);

    out += ",\"location\":\"";
    std::wstring loc;
    int n = MultiByteToWideChar(CP_UTF8, 0, cfg.location.c_str(),
                                (int)cfg.location.size(), nullptr, 0);
    if (n > 0) {
        loc.resize(n);
        MultiByteToWideChar(CP_UTF8, 0, cfg.location.c_str(),
                            (int)cfg.location.size(), &loc[0], n);
    }
    std::string locJson;
    jsonEscape(loc, locJson);
    out += locJson;
    out += "\"}";
    return out;
}
