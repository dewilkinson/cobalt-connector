#include "app.hpp"

#include <bcrypt.h>
#include <shlobj.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <vector>

namespace cw {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && static_cast<unsigned char>(s[a]) <= ' ') ++a;
    while (b > a && static_cast<unsigned char>(s[b - 1]) <= ' ') --b;
    return s.substr(a, b - a);
}

bool readFile(const std::wstring& path, std::string& out, size_t maxBytes) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && static_cast<unsigned long long>(size.QuadPart) <= maxBytes;
    if (ok) {
        out.assign(static_cast<size_t>(size.QuadPart), '\0');
        DWORD got = 0;
        ok = size.QuadPart == 0 || (ReadFile(h, out.data(), static_cast<DWORD>(size.QuadPart), &got, nullptr) &&
                                    got == static_cast<DWORD>(size.QuadPart));
    }
    CloseHandle(h);
    return ok;
}

bool writeFileAtomic(const std::wstring& path, const std::string& data) {
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = data.empty() || (WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &put, nullptr) &&
                               put == static_cast<DWORD>(data.size()));
    CloseHandle(h);
    if (!ok) { DeleteFileW(tmp.c_str()); return false; }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

std::string tailFile(const std::wstring& path, size_t maxBytes, int maxLines) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size{};
    std::string out;
    if (GetFileSizeEx(h, &size)) {
        long long from = size.QuadPart > static_cast<long long>(maxBytes) ? size.QuadPart - static_cast<long long>(maxBytes) : 0;
        LARGE_INTEGER pos{};
        pos.QuadPart = from;
        SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
        out.assign(static_cast<size_t>(size.QuadPart - from), '\0');
        DWORD got = 0;
        if (!out.empty() && ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr)) out.resize(got);
        else out.clear();
        if (from > 0) {
            size_t nl = out.find('\n');
            out = nl == std::string::npos ? std::string() : out.substr(nl + 1);
        }
    }
    CloseHandle(h);
    int lines = 0;
    for (size_t i = out.size(); i-- > 0;) {
        if (out[i] == '\n' && i + 1 < out.size() && ++lines >= maxLines) { out = out.substr(i + 1); break; }
    }
    return out;
}

bool fileExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool dirExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

void ensureDir(const std::wstring& path) { SHCreateDirectoryExW(nullptr, path.c_str(), nullptr); }

bool deleteTree(const std::wstring& path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return true;
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY) || (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
        // a file, or a link to a folder: the link itself goes, never what it points at
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (attrs & FILE_ATTRIBUTE_DIRECTORY) return RemoveDirectoryW(path.c_str()) != 0;
        return DeleteFileW(path.c_str()) != 0;
    }
    bool ok = true;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((path + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            ok = deleteTree(path + L"\\" + name) && ok;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryW(path.c_str()) != 0 && ok;
}

double fileAgeSeconds(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return 1e12;
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER a{}, b{};
    a.LowPart = d.ftLastWriteTime.dwLowDateTime; a.HighPart = d.ftLastWriteTime.dwHighDateTime;
    b.LowPart = now.dwLowDateTime; b.HighPart = now.dwHighDateTime;
    return b.QuadPart > a.QuadPart ? static_cast<double>(b.QuadPart - a.QuadPart) / 1e7 : 0.0;
}

std::wstring exePath() {
    std::wstring p(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, p.data(), static_cast<DWORD>(p.size()));
        if (n < p.size()) { p.resize(n); return p; }
        p.resize(p.size() * 2);
    }
}

std::wstring exeDir() {
    std::wstring p = exePath();
    size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}

std::wstring localAppDataDir() {
    PWSTR local = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) dir = local;
    if (local) CoTaskMemFree(local);
    return dir;
}

static std::wstring homeOverride() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(L"COBALT_CONNECTOR_HOME", buf, static_cast<DWORD>(std::size(buf)));
    return n > 0 && n < std::size(buf) ? std::wstring(buf, n) : std::wstring();
}

bool homeOverridden() { return !homeOverride().empty(); }

std::wstring homeDir() {
    static std::wstring cached;
    if (!cached.empty()) return cached;
    cached = homeOverride();
    if (cached.empty()) {
        std::wstring local = localAppDataDir();
        cached = local.empty() ? exeDir() + L"\\home" : local + L"\\" + kHomeName;
    }
    ensureDir(cached);
    return cached;
}

std::wstring homeFile(const wchar_t* name) { return homeDir() + L"\\" + name; }

std::wstring logsDir() {
    std::wstring d = homeDir() + L"\\logs";
    ensureDir(d);
    return d;
}

std::wstring pythonExe() { return exeDir() + L"\\python\\python.exe"; }
std::wstring bootstrapScript() { return exeDir() + L"\\bootstrap\\cobalt_bootstrap.py"; }

void logLine(const std::wstring& text) {
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    std::wstring path = logsDir() + L"\\app.log";
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d) && d.nFileSizeLow > 1000000) {
        std::wstring old = path + L".1";
        MoveFileExW(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t stamp[32];
    swprintf_s(stamp, L"%04u-%02u-%02u %02u:%02u:%02u ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::string line = narrow(std::wstring(stamp) + text) + "\r\n";
    DWORD put = 0;
    WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &put, nullptr);
    CloseHandle(h);
}

Json readJsonFile(const std::wstring& path) {
    std::string text;
    Json j;
    if (readFile(path, text, 4u << 20) && parseJson(text, j) && j.isObject()) return j;
    return Json::object();
}

bool writeJsonFile(const std::wstring& path, const Json& j) { return writeFileAtomic(path, j.dump(2)); }

// http://127.0.0.1:<port> or http://localhost:<port> and nothing after the port: this machine only.
static bool loopbackHttp(const std::string& u) {
    for (const char* prefix : {"http://127.0.0.1:", "http://localhost:"}) {
        std::string p = prefix;
        if (u.size() > p.size() && u.compare(0, p.size(), p) == 0) {
            std::string port = u.substr(p.size());
            if (port.size() <= 5 && port.find_first_not_of("0123456789") == std::string::npos) return true;
        }
    }
    return false;
}

std::string normalRelay(const std::string& url) {
    std::string r = trim(url);
    while (!r.empty() && r.back() == '/') r.pop_back();
    return r;
}

bool secureRelay(const std::string& url) {
    std::string r = normalRelay(url);
    if (r.size() > 8 && _strnicmp(r.c_str(), "https://", 8) == 0) return true;
    return loopbackHttp(r);
}

std::wstring insecureRelayMessage(const std::string& url) {
    return L"The server address " + widen(normalRelay(url)) +
           L" is not secure: Cobalt Connector only talks to a server over https.";
}

std::string relayUrl() {
    std::string r = normalRelay(readJsonFile(homeFile(L"contributor.json")).str("relay"));
    if (r.empty()) return std::string(kDefaultRelay);
    if (!secureRelay(r)) {
        // an edited contributor.json never downgrades the connection: the hosted backend is used instead
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            logLine(insecureRelayMessage(r) + L" The stored address is ignored; using " + widen(kDefaultRelay) + L".");
        return std::string(kDefaultRelay);
    }
    return r;
}

std::string machineId() {
    std::wstring path = homeFile(L"contributor.json");
    Json cfg = readJsonFile(path);
    std::string id = cfg.str("machine");
    if (!id.empty()) return id;       // an id made by an older version (the computer's name and 8 hex) is kept
    // "cc-" and 12 random hex digits: nothing of the computer's name is in it
    unsigned char rnd[6] = {};
    BCryptGenRandom(nullptr, rnd, sizeof rnd, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    char hex[16];
    std::snprintf(hex, sizeof hex, "%02x%02x%02x%02x%02x%02x", rnd[0], rnd[1], rnd[2], rnd[3], rnd[4], rnd[5]);
    id = std::string("cc-") + hex;
    cfg.set("machine", Json::string(id));
    writeJsonFile(path, cfg);
    return id;
}

bool limitsPaused() { return readJsonFile(homeFile(L"limits.json")).flag("paused"); }

bool setPaused(bool paused) {
    std::wstring path = homeFile(L"limits.json");
    Json lim = readJsonFile(path);
    lim.set("paused", Json::boolean(paused));
    return writeJsonFile(path, lim);
}

bool localModelChosen() { return readJsonFile(homeFile(L"limits.json")).flag("local_model"); }

bool setLocalModel(bool on) {
    std::wstring path = homeFile(L"limits.json");
    Json lim = readJsonFile(path);
    lim.set("local_model", Json::boolean(on));
    return writeJsonFile(path, lim);
}

std::string machineName() { return trim(readJsonFile(homeFile(L"limits.json")).str("name")); }

bool validMachineName(const std::string& name) {
    if (name.size() > 32) return false;
    for (unsigned char c : name) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' ||
                  c == '_' || c == '.';
        if (!ok) return false;
    }
    return true;
}

bool setMachineName(const std::string& name) {
    std::wstring path = homeFile(L"limits.json");
    Json lim = readJsonFile(path);
    lim.set("name", Json::string(trim(name)));
    return writeJsonFile(path, lim);
}

std::wstring computerName() {
    wchar_t host[MAX_COMPUTERNAME_LENGTH + 1] = L"";
    DWORD n = static_cast<DWORD>(std::size(host));
    if (!GetComputerNameW(host, &n)) return L"this computer";
    return host;
}

bool updatesAsk() { return readJsonFile(homeFile(L"limits.json")).str("updates") == "ask"; }

bool setUpdatesAsk(bool ask) {
    std::wstring path = homeFile(L"limits.json");
    Json lim = readJsonFile(path);
    lim.set("updates", Json::string(ask ? "ask" : "auto"));
    return writeJsonFile(path, lim);
}

int setLimitsFromFile(const std::wstring& file, std::wstring& message) {
    std::string text;
    if (!readFile(file, text, 1u << 16)) {
        message = L"The limits file could not be read.";
        return 1;
    }
    DeleteFileW(file.c_str());
    std::wstring path = homeFile(L"limits.json");
    Json lim = readJsonFile(path);
    size_t pos = 0;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF) pos = 3;   // a byte order mark
    int written = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = trim(text.substr(pos, end - pos));
        pos = end + 1;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        bool onlyIfMissing = !k.empty() && k[0] == '?';
        if (onlyIfMissing) k = trim(k.substr(1));
        if (k.empty() || (onlyIfMissing && lim.get(k))) continue;
        Json value;
        if (k == "cores" || k == "memory_gb") {
            char* stop = nullptr;
            double d = std::strtod(v.c_str(), &stop);
            if (v.empty() || *stop != '\0' || d < 1.0) continue;
            value = Json::number(d);
        } else if (k == "paused" || k == "local_model") {
            value = Json::boolean(v == "1" || v == "true" || v == "yes" || v == "on");
        } else if (k == "name") {
            if (!validMachineName(v)) continue;            // the computer's own name is shown instead
            value = Json::string(v);
        } else if (k == "updates") {
            value = Json::string(v == "ask" ? "ask" : "auto");
        } else if (k == "hours") {
            value = Json::string(v.empty() ? "any" : v);
        } else if (k == "primed") {
            value = Json::string(v);
        } else {
            continue;                                      // not a setting the app knows
        }
        lim.set(k, value);
        ++written;
    }
    if (!writeJsonFile(path, lim)) {
        message = L"The limits could not be saved.";
        return 1;
    }
    logLine(L"limits set by the installer (" + std::to_wstring(written) + L" settings)");
    message = L"The limits are saved.";
    return 0;
}

std::string anthropicTestUrl() {
    std::string u = trim(readJsonFile(homeFile(L"contributor.json")).str("anthropic_url"));
    while (!u.empty() && u.back() == '/') u.pop_back();
    return loopbackHttp(u) ? u : std::string();
}

int cpuCount() {
    DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return n ? static_cast<int>(n) : 1;
}

double totalMemoryGB() {
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof m;
    GlobalMemoryStatusEx(&m);
    return static_cast<double>(m.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0);
}

static constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

bool runAtLogon() {
    wchar_t buf[2048];
    DWORD size = sizeof buf;
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS;
}

bool setRunAtLogon(bool on) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    LSTATUS rc;
    if (on) {
        std::wstring cmd = L"\"" + exePath() + L"\" --background";
        rc = RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                            static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        rc = RegDeleteValueW(key, kRunValue);
        if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

}  // namespace cw
