// CobaltConnector - the Cobalt Connector app.
//
// The app does not speak the fleet protocol itself: it SUPERVISES the embedded private Python installed
// beside it, running bootstrap\cobalt_bootstrap.py. The bootstrap downloads the worker code the fleet
// runs (by its hash, and only once the partner key is accepted) and runs that code's pull loop; the app
// shows what the loop reports in status.json. The network calls the app makes itself, over WinHTTP, are
// the sign-in (token.cpp: enrol once with the partner key, then a one-hour pass renewed with the machine's own
// device key, devkey.cpp), --check-key (the installer's key check, which is the enrol), and the update check and
// download (update.cpp).
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
std::string relayUrl();            // contributor.json "relay" when secureRelay(), else the hosted backend
// A server address the partner key may be sent to: https://, or plain http only to this machine
// (http://127.0.0.1:<port> or http://localhost:<port>, for tests). Anything else would send the key in clear text.
bool secureRelay(const std::string& url);
std::string normalRelay(const std::string& url);              // trimmed, without trailing slashes
std::wstring insecureRelayMessage(const std::string& url);    // the plain sentence that refuses one
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

// ---- one request to the Cobalt server, never following a redirect (net.cpp)
struct ServerReply {
    DWORD status = 0;
    std::string body;
    Json json;                 // the body, when it is a JSON object
    std::string reason;        // X-Cobalt-Reason (or the body's "reason"): a code, never a secret
    int retryAfter = 0;        // seconds, from Retry-After
    std::wstring error;        // why it could not be sent or answered (then status is 0)
};
// `headers` (CRLF-terminated lines; it may hold a key or a pass) is wiped once sent. False when the server could not
// be reached; otherwise `out` holds whatever status came back, a 3xx included.
bool serverRequest(const std::string& url, const wchar_t* method, std::wstring& headers, const std::string& body,
                   ServerReply& out, DWORD timeoutMs = 20000);
std::wstring winHttpError(DWORD e);
// The server's host as the app addresses it, as signed requests name it: lowercase, ":port" only when it is not the
// scheme's default ("api.bluesec.ai", "127.0.0.1:8000").
std::string relayAudience(const std::string& relay);

// ---- the machine's own device key: ECDSA P-256 in the TPM, else Windows' software key store (devkey.cpp)
struct DeviceKey {
    std::string machine;
    std::string provider;      // "tpm" or "software"
    std::string publicKey;     // base64url of 0x04 || X || Y
};
std::string base64url(const std::string& bytes);                    // without padding
std::string randomNonce();                                          // 16 random bytes, base64url
// What opening the device key found. Missing: the key store itself answered that no such key exists (NTE_BAD_KEYSET:
// a cleared TPM, a deleted key) - only then may a new key be made. Unavailable: anything else (the TPM not ready at
// logon or after resume, a dictionary-attack lockout, its service not running): the key, the device record and the
// pass are all kept, and the app tries again later.
enum class KeyState { Ok, Missing, Unavailable };
KeyState openDeviceKey(const std::string& machine, const std::string& provider, DeviceKey& out, std::wstring& err);
bool createDeviceKey(const std::string& machine, DeviceKey& out, std::wstring& err);   // replaces one of that name
bool signWithDeviceKey(const DeviceKey& k, const std::string& text, std::string& sigB64u, std::wstring& err);
bool deleteDeviceKey(const std::string& machine);
int selfTestDeviceKey(std::wstring& message);                       // --self-test-devkey (build.ps1 runs it)
int signTestVectors(const std::wstring& outFile, std::wstring& message);   // --sign-test-vector <file>

// ---- signing in to the Cobalt server (token.cpp)
enum class SignIn {
    Waiting,       // not decided yet (the first enrol is in flight): the work process is not started
    Device,        // the work process gets COBALT_TOKEN_FILE and never the partner key
    Legacy,        // an older server with no device keys, before this machine ever enrolled: the partner key, as 1.2.1
    Blocked,       // removed by Cobalt's owner, a revoked or refused key: the work process is stopped; `problem` says why
};
struct SignInState {
    SignIn mode = SignIn::Waiting;
    std::wstring problem;
    unsigned generation = 0;   // changes whenever `mode` does: the supervisor restarts the work process then
};
void startSignIn();            // the app's start: the sign-in thread
void stopSignIn();
void wakeSignIn();             // resume from sleep: look again now
void signInTick();             // every second: token.stale (the work process's pass was refused) renews at once
SignInState signInState();
std::wstring tokenFilePath();  // %LOCALAPPDATA%\CobaltConnector\token.json
// The header line for an app request to the fleet (the update check): the pass, or the partner key only on an older
// server. False when there is neither yet.
bool fleetCredential(std::wstring& headerLine);
// The installer's key check as the enrol itself: 0 accepted (the device is made and holds its first pass), 1 refused,
// 2 could not check, 3 the server has no device keys yet (the caller checks the key the old way).
int enrolForKeyCheck(const std::string& key, const std::string& relay, std::wstring& message);
// --retire (the uninstaller): the device retires itself on the server (best effort, 5 s), its key is deleted from
// Windows' key store and token.json removed.
int retireDevice(std::wstring& message);

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
    unsigned signInGeneration_ = 0;    // the sign-in the running process was started with
    bool waitingForSignIn_ = false;
    ULONGLONG startedAt_ = 0;
    ULONGLONG nextStart_ = 0;
    int failures_ = 0;
    std::wstring problem_;
};

// ---- the tray icon and the window (ui.cpp)
int runApp(HINSTANCE inst, bool showWindow);

}  // namespace cw
