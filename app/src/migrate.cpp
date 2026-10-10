// Moving the settings of an older install. Before 1.2.0 the app was called Cobalt Worker and kept everything
// in %LOCALAPPDATA%\CobaltWorker; it now lives in %LOCALAPPDATA%\CobaltConnector.
//
// Nothing here ever loses a key: the old folder is deleted only once the new one holds a partner key that
// opens and every settings file the old folder had. Keys sealed with DPAPI stay valid after a move (they are sealed for the Windows user, not the path).
#include "app.hpp"

#include <vector>

namespace cw {

namespace {

// Lines for app.log, held until the move is done: writing the log first would make the new folder (logs// lives in it) and the old folder could then no longer simply be renamed.
std::vector<std::wstring> g_notes;

void note(const std::wstring& text) { g_notes.push_back(text); }

void flushNotes() {
    for (const auto& n : g_notes) logLine(n);
    g_notes.clear();
}

// The small files that carry the owner's settings and the machine's identity. The cached market data and
// the downloaded worker build are not copied: they download again.
const wchar_t* const kSettingsFiles[] = {L"partner.key", L"anthropic.key", L"contributor.json", L"limits.json"};
// Folders worth moving whole when they can be (a rename on the same disk is instant).
const wchar_t* const kBulkFolders[] = {L"cache", L"build"};

std::wstring legacyHome() {
    std::wstring local = localAppDataDir();
    return local.empty() ? std::wstring() : local + L"\\" + kLegacyHomeName;
}

std::wstring newHome() {
    std::wstring local = localAppDataDir();
    return local.empty() ? std::wstring() : local + L"\\" + kHomeName;
}

// Copies what the new folder lacks; returns how many files were copied.
int copyMissing(const std::wstring& from, const std::wstring& to) {
    ensureDir(to);
    int copied = 0;
    for (const wchar_t* name : kSettingsFiles) {
        std::wstring src = from + L"\\" + name, dst = to + L"\\" + name;
        if (fileExists(src) && !fileExists(dst) && CopyFileW(src.c_str(), dst.c_str(), TRUE)) ++copied;
    }
    return copied;
}

// A running Cobalt Worker (1.1.0 or older) still holds files in its folder: ask it to quit, as its own
// --quit does, and wait for it.
void quitLegacyApp() {
    HWND w = FindWindowW(kLegacyWindowClass, nullptr);
    if (!w) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid);
    PostMessageW(w, WM_APP_QUIT, 0, 0);
    if (p) {
        WaitForSingleObject(p, 10000);
        CloseHandle(p);
    }
    note(L"Asked the old Cobalt Worker app to quit.");
}

// The old app's "run at logon" entry starts an exe the installer has removed: replace it with ours.
void replaceLegacyRunValue() {
    static constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    bool had = RegQueryValueExW(key, kLegacyRunValue, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    if (had) RegDeleteValueW(key, kLegacyRunValue);
    RegCloseKey(key);
    if (had) {
        setRunAtLogon(true);
        note(L"Moved the start-at-sign-in setting from the old Cobalt Worker.");
    }
}

}  // namespace

void copyLegacySettings() {
    if (homeOverridden()) return;
    std::wstring from = legacyHome(), to = newHome();
    if (from.empty() || !dirExists(from)) return;
    int n = copyMissing(from, to);
    if (n > 0) logLine(L"Copied your settings from the old Cobalt Worker folder (" + std::to_wstring(n) + L" files).");
}

void migrateLegacyHome() {
    if (homeOverridden()) return;
    struct Flush { ~Flush() { flushNotes(); } } flushAtEnd;
    replaceLegacyRunValue();
    std::wstring from = legacyHome(), to = newHome();
    if (from.empty() || !dirExists(from)) return;
    quitLegacyApp();

    if (!dirExists(to)) {
        if (MoveFileExW(from.c_str(), to.c_str(), 0)) {
            note(L"Moved your settings from the old Cobalt Worker folder.");
            return;
        }
        note(L"The old Cobalt Worker folder could not be moved (error " + std::to_wstring(GetLastError()) +
                L"); copying your settings instead.");
    }
    int n = copyMissing(from, to);
    for (const wchar_t* name : kBulkFolders) {
        std::wstring src = from + L"\\" + name, dst = to + L"\\" + name;
        if (dirExists(src) && !dirExists(dst)) MoveFileExW(src.c_str(), dst.c_str(), 0);
    }
    if (n > 0) note(L"Copied your settings from the old Cobalt Worker folder (" + std::to_wstring(n) + L" files).");

    // Delete the old folder only when a partner key that opens is in the new one.
    std::optional<std::string> key = loadSecret(L"partner");
    if (!key) {
        note(L"The old Cobalt Worker folder is kept: no readable partner key is in the new folder yet.");
        return;
    }
    SecureZeroMemory(key->data(), key->size());
    // ... and only when every settings file the old folder holds is in the new one too: a copy that failed
    // (a file held open by a backup or antivirus tool, a full disk) is tried again next start, not lost
    for (const wchar_t* name : kSettingsFiles) {
        if (fileExists(from + L"\\" + name) && !fileExists(to + L"\\" + name)) {
            note(std::wstring(L"The old Cobalt Worker folder is kept: ") + name +
                 L" could not be copied from it yet; trying again next start.");
            return;
        }
    }
    if (deleteTree(from)) note(L"Removed the old Cobalt Worker folder.");
    else note(L"Part of the old Cobalt Worker folder could not be removed yet; trying again next start.");
}

}  // namespace cw
