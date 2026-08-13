// Файловые операции для файлового менеджера (вкладка «Файлы»).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "filemgr.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

// -------------------------------------------------- UTF-8 <-> wide --------
static std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out;
    if (n <= 0) return out;
    out.resize(n);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n);
    return out;
}

static std::string wideToUtf8(const std::wstring& s) {
    if (s.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out;
    if (n <= 0) return out;
    out.resize(n);
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n, nullptr, nullptr);
    return out;
}

// Экранирование строки для JSON (заодно конвертит wide -> UTF-8).
static void jsonEscapeWide(const std::wstring& s, std::string& out) {
    std::string u8 = wideToUtf8(s);
    for (unsigned char c : u8) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof(b), "\\u%04x", c);
                    out += b;
                } else {
                    out += (char)c;
                }
        }
    }
}

static long long filetimeToUnix(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (long long)((u.QuadPart - 116444736000000000ULL) / 10000000ULL);
}

// ------------------------------------------------------------ листинг ----
bool fmListPath(const std::string& pathUtf8, std::string& jsonEntries, std::string& err) {
    std::wstring path = utf8ToWide(pathUtf8);
    std::string out = "[";

    // Пусто -> список логических дисков
    if (path.empty()) {
        DWORD drv = GetLogicalDrives();
        bool first = true;
        for (int i = 0; i < 26; i++) {
            if (drv & (1u << i)) {
                wchar_t name[8];
                swprintf(name, 8, L"%c:\\", L'A' + i);
                if (!first) out += ",";
                first = false;
                out += "{\"name\":\"";
                jsonEscapeWide(name, out);
                out += "\",\"size\":0,\"mtime\":0,\"dir\":true}";
            }
        }
        out += "]";
        jsonEntries = out;
        return true;
    }

    // Нормализация: убираем хвостовой слеш (кроме корня диска), добавляем "\\*"
    std::wstring base = path;
    while (base.size() > 3 && (base.back() == L'\\' || base.back() == L'/')) base.pop_back();
    std::wstring pattern = base + L"\\*";

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD attr = GetFileAttributesW(base.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            err = "not a directory";
            return false;
        }
        err = "no such directory";
        return false;
    }

    bool first = true;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        unsigned long long size =
            ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        long long mtime = filetimeToUnix(fd.ftLastWriteTime);

        if (!first) out += ",";
        first = false;
        out += "{\"name\":\"";
        jsonEscapeWide(name, out);
        char b[64];
        snprintf(b, sizeof(b), "\",\"size\":%llu,\"mtime\":%lld,\"dir\":%s}",
                 size, mtime, isDir ? "true" : "false");
        out += b;
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    out += "]";
    jsonEntries = out;
    return true;
}

// ------------------------------------------------------------- чтение ----
bool fmReadFile(const std::string& pathUtf8, size_t maxBytes,
                std::vector<uint8_t>& out, std::string& err) {
    std::wstring w = utf8ToWide(pathUtf8);
    HANDLE h = CreateFileW(w.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { err = "cannot open"; return false; }

    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0) {
        err = "cannot stat";
        CloseHandle(h);
        return false;
    }
    if ((unsigned long long)sz.QuadPart > maxBytes) {
        err = "file too large";
        CloseHandle(h);
        return false;
    }

    out.resize((size_t)sz.QuadPart);
    DWORD rd = 0;
    BOOL ok = ReadFile(h, out.data(), (DWORD)out.size(), &rd, nullptr);
    CloseHandle(h);
    if (!ok) { err = "read failed"; return false; }
    out.resize(rd);
    return true;
}

// ------------------------------------------------------------- запись ----
bool fmWriteFile(const std::string& pathUtf8, const uint8_t* data, size_t len, std::string& err) {
    printf("[fm] fmWriteFile begin: len=%zu\n", len);
    fflush(stdout);
    std::wstring w = utf8ToWide(pathUtf8);
    HANDLE h = CreateFileW(w.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        char b[256];
        snprintf(b, sizeof(b), "cannot create (%s, winerr %lu)", pathUtf8.c_str(), (unsigned long)e);
        err = b;
        return false;
    }

    size_t off = 0;
    while (off < len) {
        DWORD chunk = (DWORD)std::min<size_t>(len - off, (size_t)(1 << 20));
        DWORD wr = 0;
        if (!WriteFile(h, data + off, chunk, &wr, nullptr)) {
            err = "write failed";
            CloseHandle(h);
            return false;
        }
        off += wr;
        if (wr == 0) { err = "write stalled"; break; }
    }
    CloseHandle(h);
    if (off != len) return false;
    return true;
}

// ------------------------------------------------------------- запуск ----
bool fmRunFile(const std::string& pathUtf8, std::string& err) {
    std::wstring w = utf8ToWide(pathUtf8);
    HINSTANCE r = ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if ((intptr_t)r <= 32) { err = "cannot launch"; return false; }
    return true;
}

// --------------------------------------------- установка в автозагрузку ----
// Копирует файл в общую папку автозагрузки (для всех пользователей),
// чтобы приложение стартовало вместе с системой.
bool fmInstallAutostart(const std::string& pathUtf8, std::string& err) {
    std::wstring w = utf8ToWide(pathUtf8);
    DWORD attr = GetFileAttributesW(w.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) { err = "not found"; return false; }
    if (attr & FILE_ATTRIBUTE_DIRECTORY) { err = "is a directory"; return false; }

    PWSTR raw = nullptr;
    HRESULT hr = SHGetKnownFolderPath(FOLDERID_CommonStartup, 0, nullptr, &raw);
    if (FAILED(hr) || !raw) { err = "no startup folder"; return false; }
    std::wstring dir(raw);
    CoTaskMemFree(raw);

    size_t pos = w.find_last_of(L"\\/");
    std::wstring name = (pos == std::wstring::npos) ? w : w.substr(pos + 1);
    if (name.empty()) { err = "bad filename"; return false; }

    std::wstring dst = dir + L"\\" + name;
    if (!CopyFileW(w.c_str(), dst.c_str(), FALSE)) {
        DWORD e = GetLastError();
        char b[256];
        snprintf(b, sizeof(b), "cannot copy to startup (winerr %lu)", (unsigned long)e);
        err = b;
        return false;
    }
    return true;
}

// ----------------------------------------------------------- удаление ----
bool fmDeletePath(const std::string& pathUtf8, std::string& err) {
    std::wstring w = utf8ToWide(pathUtf8);
    DWORD attr = GetFileAttributesW(w.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) { err = "not found"; return false; }

    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        // рекурсивное удаление дерева через SHFileOperation
        std::vector<wchar_t> from(w.begin(), w.end());
        from.push_back(L'\0');
        from.push_back(L'\0');
        SHFILEOPSTRUCTW so = {};
        so.hwnd = nullptr;
        so.wFunc = FO_DELETE;
        so.pFrom = from.data();
        so.fFlags = FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOERRORUI;
        if (SHFileOperationW(&so) != 0) { err = "delete failed"; return false; }
        return true;
    }

    if (!DeleteFileW(w.c_str())) { err = "delete failed"; return false; }
    return true;
}
