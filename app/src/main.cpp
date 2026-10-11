// What leaves your machine (full list, from the code: README.md, section "What leaves your machine"):
// - To the Cobalt server (always HTTPS; a plain-http address is refused): your partner key in X-Cobalt-Contributor on every request
//   (how the machine signs in; the server keeps only its SHA-256), the machine id ("cc-" + 12 random hex), the
//   computer's name, the name you chose, cores, memory, your limits, versions, job results or error messages, and
//   model answers with token counts. Other partners see the machine only by its label (the name you chose, or
//   "<owner>'s machine N"), never the computer's name or the machine id. No request with the key follows a redirect.
// - To Anthropic, only with model work on: your Anthropic key and your own question's text. Never to Cobalt. With
//   model work off the work process is never given the key.
// - To GitHub: a plain download request for an app update (update.cpp). No key, no id.
// - Never: your files, your environment, your Windows user name, your logs. Market data only comes down.
//   (A failed job's error text goes back, and a file path in it can include your user folder's name.)
//
// CobaltConnector.exe entry point.
//
//   CobaltConnector.exe                         the tray app, with its window open
//   CobaltConnector.exe --background            the tray app only (what "Run at logon" starts)
//   CobaltConnector.exe --check-key <key> [--relay URL] [--out FILE]
//   CobaltConnector.exe --check-key-file <file> [--relay URL] [--out FILE]   (the installer: no key on a command line)
//        exit 0: accepted, 1: refused or not a key, 2: could not check (offline, or the server has no fleet route)
//   CobaltConnector.exe --set-keys <file> [--out FILE]     store partner=/anthropic=/relay= lines with DPAPI
//   CobaltConnector.exe --set-limits <file> [--out FILE]   merge key=value lines into limits.json (?key= only if unset)
//   CobaltConnector.exe --run-at-logon on|off
//   CobaltConnector.exe --quit                   stop a running instance (and the loop it supervises)
//   CobaltConnector.exe --version
#include "app.hpp"

#include <shellapi.h>

#include <vector>

namespace {

// Writes to the console that started us, when there is one (a GUI-subsystem exe has none of its own).
void say(const std::wstring& text) {
    static bool attached = AttachConsole(ATTACH_PARENT_PROCESS) != FALSE;
    (void)attached;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!out || out == INVALID_HANDLE_VALUE) return;
    std::wstring line = text + L"\r\n";
    DWORD n = 0;
    if (GetFileType(out) == FILE_TYPE_CHAR) {
        WriteConsoleW(out, line.c_str(), static_cast<DWORD>(line.size()), &n, nullptr);
    } else {
        std::string u = cw::narrow(line);
        WriteFile(out, u.data(), static_cast<DWORD>(u.size()), &n, nullptr);
    }
}

void report(const std::wstring& message, const std::wstring& outFile) {
    say(message);
    if (!outFile.empty()) cw::writeFileAtomic(outFile, cw::narrow(message));
}

int quitRunning() {
    HWND w = FindWindowW(cw::kWindowClass, nullptr);
    if (!w) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid);
    PostMessageW(w, cw::WM_APP_QUIT, 0, 0);
    if (p) {
        DWORD r = WaitForSingleObject(p, 15000);
        CloseHandle(p);
        return r == WAIT_OBJECT_0 ? 0 : 1;
    }
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args(argv + 1, argv + argc);
    LocalFree(argv);

    auto option = [&](const wchar_t* name) -> std::wstring {
        for (size_t i = 0; i + 1 < args.size(); ++i)
            if (args[i] == name) return args[i + 1];
        return {};
    };
    auto has = [&](const wchar_t* name) {
        for (const auto& a : args) if (a == name) return true;
        return false;
    };
    std::wstring outFile = option(L"--out");

    if (has(L"--version")) { say(L"Cobalt Connector " COBALT_CONNECTOR_VERSION_W); return 0; }
    if (has(L"--help") || has(L"/?")) {
        say(L"Cobalt Connector " COBALT_CONNECTOR_VERSION_W L"\r\n"
            L"  (no options)            the tray app, window open\r\n"
            L"  --background            the tray app only\r\n"
            L"  --check-key <key>       check a partner key against the Cobalt server (exit 0 ok, 1 refused, 2 offline)\r\n"
            L"  --check-key-file <f>    the same, the key read from a file (deleted after)\r\n"
            L"  --set-keys <file>       store partner=/anthropic=/relay= lines, encrypted for this Windows user\r\n"
            L"  --set-limits <file>     save key=value lines into your limits (?key= only when not set yet)\r\n"
            L"  --run-at-logon on|off   start at sign-in, or not\r\n"
            L"  --quit                  stop the running app and its work");
        return 0;
    }
    if (has(L"--check-key") || has(L"--check-key-file")) {
        cw::copyLegacySettings();          // an upgrade keeps the machine's id
        std::string key;
        if (has(L"--check-key-file")) {
            std::wstring f = option(L"--check-key-file");
            cw::readFile(f, key, 4096);
            if (!f.empty()) DeleteFileW(f.c_str());
        } else {
            key = cw::narrow(option(L"--check-key"));
        }
        std::wstring relay = option(L"--relay");
        std::wstring message;
        int rc = cw::checkKey(key, relay.empty() ? cw::relayUrl() : cw::narrow(relay), message);
        SecureZeroMemory(key.data(), key.size());
        cw::logLine(L"key check: exit " + std::to_wstring(rc));
        report(message, outFile);
        return rc;
    }
    if (has(L"--set-keys")) {
        cw::copyLegacySettings();
        std::wstring message;
        int rc = cw::setKeysFromFile(option(L"--set-keys"), message);
        report(message, outFile);
        return rc;
    }
    if (has(L"--set-limits")) {
        cw::copyLegacySettings();
        std::wstring message;
        int rc = cw::setLimitsFromFile(option(L"--set-limits"), message);
        report(message, outFile);
        return rc;
    }
    if (has(L"--run-at-logon")) {
        std::wstring v = option(L"--run-at-logon");
        bool on = v == L"on" || v == L"1" || v == L"true";
        bool ok = cw::setRunAtLogon(on);
        report(ok ? (on ? L"Cobalt Connector starts at sign-in." : L"Cobalt Connector no longer starts at sign-in.")
                  : L"The sign-in setting could not be changed.", outFile);
        return ok ? 0 : 1;
    }
    if (has(L"--quit")) return quitRunning();

    HANDLE mutex = CreateMutexW(nullptr, TRUE, cw::kMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (!has(L"--background"))
            if (HWND w = FindWindowW(cw::kWindowClass, nullptr)) PostMessageW(w, cw::WM_APP_SHOW, 0, 0);
        if (mutex) CloseHandle(mutex);
        return 0;
    }
    cw::migrateLegacyHome();               // the first start after an upgrade from Cobalt Worker
    int rc = cw::runApp(inst, !has(L"--background"));
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return rc;
}
