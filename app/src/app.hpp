// CobaltWorker - the Cobalt Worker app.
//
// The app does not speak the fleet protocol itself: it SUPERVISES the embedded private Python installed
// beside it, running bootstrap\cobalt_bootstrap.py. The bootstrap downloads the worker code the fleet
// runs (by its hash, and only once the partner key is accepted) and runs that code's pull loop; the app
// shows what the loop reports in status.json. The one network call the app makes itself is --check-key
// (the installer's key check), over WinHTTP.
#pragma once
#include <windows.h>

#include <optional>
#include <string>

#include "json.hpp"

#ifndef COBALT_WORKER_VERSION
#define COBALT_WORKER_VERSION "0.0.0"
#endif
#define CW_WIDEN2(x) L##x
#define CW_WIDEN(x) CW_WIDEN2(x)
#define COBALT_WORKER_VERSION_W CW_WIDEN(COBALT_WORKER_VERSION)

namespace cw {

inline constexpr wchar_t kAppName[] = L"Cobalt Worker";
inline constexpr wchar_t kWindowClass[] = L"CobaltWorkerMain";
inline constexpr wchar_t kMutexName[] = L"Local\\CobaltWorker.SingleInstance";
inline constexpr wchar_t kRunValue[] = L"CobaltWorker";
inline constexpr char kDefaultRelay[] = "https://api.bluesec.ai";

inline constexpr UINT WM_APP_TRAY = WM_APP + 1;     // the tray icon's callback
inline constexpr UINT WM_APP_SHOW = WM_APP + 2;     // a second launch asks the first to open its window
inline constexpr UINT WM_APP_QUIT = WM_APP + 3;     // --quit (the uninstaller) asks it to stop

// ---- strings and files (util.cpp)
std::wstring widen(const std::string& s);
std::string narrow(const std::wstring& s);
std::string trim(const std::string& s);
bool readFile(const std::wstring& path, std::string& out, size_t maxBytes = 16u << 20);
bool writeFileAtomic(const std::wstring& path, const std::string& data);
std::string tailFile(const std::wstring& path, size_t maxBytes, int maxLines);
bool fileExists(const std::wstring& path);
void ensureDir(const std::wstring& path);
double fileAgeSeconds(const std::wstring& path);   // a large number when absent

// ---- where things live (util.cpp)
std::wstring exeDir();             // the install folder
std::wstring exePath();
std::wstring homeDir();            // %LOCALAPPDATA%\CobaltWorker (COBALT_WORKER_HOME overrides, for tests)
std::wstring homeFile(const wchar_t* name);
std::wstring logsDir();
std::wstring pythonExe();          // <install>\python\python.exe
std::wstring bootstrapScript();    // <install>\bootstrap\cobalt_bootstrap.py

// ---- the app's own log: supervisor events, never a key (util.cpp)
void logLine(const std::wstring& text);

// ---- settings shared with the Python loop (util.cpp)
Json readJsonFile(const std::wstring& path);
bool writeJsonFile(const std::wstring& path, const Json& j);
std::string relayUrl();            // contributor.json "relay", else the hosted backend
std::string machineId();           // contributor.json "machine", made once (the loop's own format)
bool limitsPaused();
bool setPaused(bool paused);
int cpuCount();
double totalMemoryGB();

// ---- run at logon (util.cpp)
bool runAtLogon();
bool setRunAtLogon(bool on);

// ---- keys, encrypted with DPAPI for the current Windows user (keys.cpp)
bool saveSecret(const wchar_t* name, const std::string& value);   // name: L"partner" or L"anthropic"
std::optional<std::string> loadSecret(const wchar_t* name);
bool deleteSecret(const wchar_t* name);
int setKeysFromFile(const std::wstring& file, std::wstring& message);

// ---- the installer's key check (check_key.cpp): 0 accepted, 1 refused, 2 could not check
int checkKey(const std::string& key, const std::string& relay, std::wstring& message);

// ---- the supervised Python loop (supervisor.cpp)
class Supervisor {
public:
    Supervisor();
    ~Supervisor();
    void tick();                       // every second, from the UI timer: start, reap, restart with backoff
    void stop();                       // the app is quitting: stop the loop and every process it started
    bool running() const { return process_ != nullptr; }
    double runningSeconds() const;
    std::wstring problem() const { return problem_; }   // why it cannot run, or "" when it can
    int failures() const { return failures_; }
    ULONGLONG nextStartTick() const { return nextStart_; }

private:
    bool start();
    HANDLE job_ = nullptr;
    HANDLE process_ = nullptr;
    ULONGLONG startedAt_ = 0;
    ULONGLONG nextStart_ = 0;
    int failures_ = 0;
    std::wstring problem_;
};

// ---- the tray icon and the window (ui.cpp)
int runApp(HINSTANCE inst, bool showWindow);

}  // namespace cw
