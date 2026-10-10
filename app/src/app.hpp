// CobaltConnector - the Cobalt Connector app.
//
// The app does not speak the fleet protocol itself: it SUPERVISES the embedded private Python installed
// beside it, running bootstrap\cobalt_bootstrap.py. The bootstrap downloads the worker code the fleet
// runs (by its hash, and only once the partner key is accepted) and runs that code's pull loop; the app
// shows what the loop reports in status.json. The network calls the app makes itself, over WinHTTP, are
// --check-key (the installer's key check) and the update check and download (update.cpp).
#pragma once
#include <windows.h>

#include <optional>
#include <string>

#include "json.hpp"

#if __has_include("version.h")
#include "version.h"            // made by CMake from version.h.in
#endif
#ifndef COBALT_CONNECTOR_VERSION
#define COBALT_CONNECTOR_VERSION "0.0.0"
#endif
#define CW_WIDEN2(x) L##x
#define CW_WIDEN(x) CW_WIDEN2(x)
#define COBALT_CONNECTOR_VERSION_W CW_WIDEN(COBALT_CONNECTOR_VERSION)

namespace cw {

inline constexpr wchar_t kAppName[] = L"Cobalt Connector";
inline constexpr wchar_t kWindowClass[] = L"CobaltConnectorMain";
inline constexpr wchar_t kMutexName[] = L"Local\\CobaltConnector.SingleInstance";
inline constexpr wchar_t kRunValue[] = L"CobaltConnector";
inline constexpr wchar_t kHomeName[] = L"CobaltConnector";            // the folder under %LOCALAPPDATA%
inline constexpr char kDefaultRelay[] = "https://api.bluesec.ai";
inline constexpr wchar_t kDownloadPage[] = L"https://github.com/dewilkinson/cobalt-connector/releases/latest";

// The app's names before 1.2.0, when it was called Cobalt Worker: used only to find, quit and move an
// older install (migrate.cpp, and the installer).
inline constexpr wchar_t kLegacyWindowClass[] = L"CobaltWorkerMain";
inline constexpr wchar_t kLegacyRunValue[] = L"CobaltWorker";
inline constexpr wchar_t kLegacyHomeName[] = L"CobaltWorker";

inline constexpr UINT WM_APP_TRAY = WM_APP + 1;     // the tray icon's callback
inline constexpr UINT WM_APP_SHOW = WM_APP + 2;     // a second launch asks the first to open its window
inline constexpr UINT WM_APP_QUIT = WM_APP + 3;     // --quit (the uninstaller) asks it to stop
inline constexpr UINT WM_APP_UPDATE = WM_APP + 4;   // the update thread is done: lParam is an UpdateResult*

// ---- strings and files (util.cpp)
std::wstring widen(const std::string& s);
std::string narrow(const std::wstring& s);
std::string trim(const std::string& s);
bool readFile(const std::wstring& path, std::string& out, size_t maxBytes = 16u << 20);
bool writeFileAtomic(const std::wstring& path, const std::string& data);
std::string tailFile(const std::wstring& path, size_t maxBytes, int maxLines);
bool fileExists(const std::wstring& path);
bool dirExists(const std::wstring& path);
void ensureDir(const std::wstring& path);
bool deleteTree(const std::wstring& path);          // a folder and everything in it; false if anything stayed
double fileAgeSeconds(const std::wstring& path);   // a large number when absent

// ---- where things live (util.cpp)
std::wstring exeDir();             // the install folder
std::wstring exePath();
std::wstring localAppDataDir();    // %LOCALAPPDATA%, or "" when Windows does not say
bool homeOverridden();             // COBALT_CONNECTOR_HOME is set (tests): no migration then
std::wstring homeDir();            // %LOCALAPPDATA%\CobaltConnector (COBALT_CONNECTOR_HOME overrides, for tests)
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
std::string machineId();           // contributor.json "machine", made once: "cc-" and 12 random hex digits
bool limitsPaused();
bool setPaused(bool paused);
// "Run my model work on this machine": limits.json "local_model". The loop is told through its environment
// (COBALT_LOCAL_MODEL), so the app restarts the loop when it changes.
bool localModelChosen();
bool setLocalModel(bool on);
// The name this machine is shown under in Cobalt: limits.json "name" ("" = other partners see "<owner>'s machine N";
// the owner and the fleet's admin see the computer's name).
// At most 32 characters: letters, digits, spaces, '-', '_' and '.'.
std::string machineName();
bool validMachineName(const std::string& name);
bool setMachineName(const std::string& name);
std::wstring computerName();
// How updates are installed: limits.json "updates", "auto" (the default) or "ask".
bool updatesAsk();
bool setUpdatesAsk(bool ask);
// --set-limits <file>: key=value lines merged into limits.json (the installer). A key written as ?key=
// is set only when limits.json does not have it yet, so a reinstall never overwrites the owner's settings.
int setLimitsFromFile(const std::wstring& file, std::wstring& message);
// A stand-in for Anthropic's API on this machine, for a smoke test only: contributor.json "anthropic_url",
// accepted only when it is http://127.0.0.1:<port> or http://localhost:<port>; "" otherwise.
std::string anthropicTestUrl();
int cpuCount();
double totalMemoryGB();

// ---- run at logon (util.cpp)
bool runAtLogon();
bool setRunAtLogon(bool on);

// ---- moving the settings of an older Cobalt Worker install (migrate.cpp)
// copyLegacySettings: copy the keys, the machine id and the limits from %LOCALAPPDATA%\CobaltWorker where
// the new folder lacks them; nothing is moved, quit or deleted (the installer's command-line calls).
// migrateLegacyHome: the app's first start - quit a running Cobalt Worker, move its folder, and delete the
// old one only once a readable partner key is in the new one.
void copyLegacySettings();
void migrateLegacyHome();

// ---- keys, encrypted with DPAPI for the current Windows user (keys.cpp)
bool saveSecret(const wchar_t* name, const std::string& value);   // name: L"partner" or L"anthropic"
std::optional<std::string> loadSecret(const wchar_t* name);
bool deleteSecret(const wchar_t* name);
int setKeysFromFile(const std::wstring& file, std::wstring& message);

// ---- the installer's key check (check_key.cpp): 0 accepted, 1 refused, 2 could not check
int checkKey(const std::string& key, const std::string& relay, std::wstring& message);

// ---- self-update (update.cpp)
struct UpdateOffer {
    std::string latest, minimum, url, sha256, notes;
    bool newer = false;        // latest is above this app's version
    bool required = false;     // this app's version is below the minimum the server accepts
};
enum class UpdateOutcome {
    NoInfo,        // the server offers nothing (no release named, or an older server)
    Failed,        // the server or the download could not be reached: try again after a back-off
    UpToDate,
    Available,     // newer, not downloaded (the owner chose to be asked)
    Ready,         // newer, downloaded and its SHA-256 checked: file names the installer
    Rejected,      // the download was not what the server said (its hash, or over 300 MB): never tried again
    Skipped,       // newer, but this release failed before and is not tried again
};
struct UpdateResult {
    UpdateOutcome outcome = UpdateOutcome::NoInfo;
    UpdateOffer offer;
    std::wstring file;
    std::wstring message;      // for the log; plain words, never a key
    bool forced = false;       // the owner pressed Update now
};
// <0, 0 or >0 like strcmp, for "N.N.N" versions; nullopt when either is not one.
std::optional<int> compareVersions(const std::string& a, const std::string& b);
// Starts the check (and, when allowed or required, the download) on a thread of its own. When done it
// posts WM_APP_UPDATE to `notify` with a new UpdateResult (the receiver deletes it). False when one is running.
bool startUpdateThread(HWND notify, bool allowDownload, bool forced);
bool updateThreadBusy();
void cancelUpdates();
// Runs the verified installer silently in update mode and records what it is installing in
// update_state.json. The caller then stops the loop and quits; the installer starts the new version.
bool launchInstaller(const UpdateResult& r, std::wstring& message);
// update_state.json: what was being installed, the back-off, and releases that failed. Only the window's
// thread writes it; the update thread only reads it.
Json readUpdateState();
void writeUpdateState(const Json& st);
bool isBadRelease(const Json& st, const std::string& version, const std::string& sha256);
void addBadRelease(Json& st, const std::string& version, const std::string& sha256);
// A release that did not install (the installer would not start, or the app came back on its old version):
// counted, and marked bad only on the kMaxReleaseStrikes-th time, so a reboot or a busy file mid-install is
// tried again after the usual wait. True when this strike made it bad.
inline constexpr int kMaxReleaseStrikes = 3;
bool strikeRelease(Json& st, const std::string& version, const std::string& sha256);

// ---- the supervised Python loop (supervisor.cpp)
class Supervisor {
public:
    Supervisor();
    ~Supervisor();
    void tick();                       // every second, from the UI timer: start, reap, restart with backoff
    void stop();                       // the app is quitting: stop the loop and every process it started
    void restart();                    // a key, the name or the model-work option changed: stop, start again at once
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
