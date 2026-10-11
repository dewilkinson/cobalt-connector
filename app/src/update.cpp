// Keeping the app up to date with the Cobalt server.
//
// Every few hours the window asks the server which version of Cobalt Connector it wants:
//     GET <relay>/api/fleet/connector?machine=<id>&version=<this version>
// with the machine's pass in X-Cobalt-Device (token.cpp; the partner key only on an older server without device
// keys), never following a redirect (net.cpp).
// The answer names the newest release, the oldest one the server still sends work to, the installer's
// address on GitHub and its SHA-256. A newer installer is downloaded only from the Cobalt Connector releases
// on GitHub (every redirect is checked by hand and must stay on GitHub's own download hosts), at most 300 MB,
// and run only when its SHA-256 matches what the server said. It then runs silently in update mode: it keeps
// the keys and the owner's settings and starts the new version. Nothing here runs on a version that is not
// newer than this one, so an update can never go backwards.
#include "app.hpp"

#include <bcrypt.h>
#include <shellapi.h>
#include <winhttp.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <thread>
#include <vector>

namespace cw {

namespace {

constexpr char kReleasePrefix[] = "https://github.com/dewilkinson/cobalt-connector/releases/download/";
const wchar_t* const kDownloadHosts[] = {L"github.com", L"objects.githubusercontent.com",
                                         L"release-assets.githubusercontent.com"};
constexpr unsigned long long kMaxDownload = 300ull << 20;
constexpr int kMaxRedirects = 6;
constexpr wchar_t kUserAgent[] = L"CobaltConnector/" COBALT_CONNECTOR_VERSION_W;

std::atomic<bool> g_busy{false};
std::atomic<bool> g_cancel{false};

struct Handle {
    HINTERNET h = nullptr;
    explicit Handle(HINTERNET v) : h(v) {}
    ~Handle() { if (h) WinHttpCloseHandle(h); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    explicit operator bool() const { return h != nullptr; }
};

struct Url {
    std::wstring host, path;      // path includes the query
    INTERNET_PORT port = 0;
    bool https = false;
    bool userInfo = false;
};

bool crack(const std::wstring& url, Url& out) {
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof uc;
    uc.dwSchemeLength = static_cast<DWORD>(-1);
    uc.dwHostNameLength = static_cast<DWORD>(-1);
    uc.dwUrlPathLength = static_cast<DWORD>(-1);
    uc.dwExtraInfoLength = static_cast<DWORD>(-1);
    uc.dwUserNameLength = static_cast<DWORD>(-1);
    uc.dwPasswordLength = static_cast<DWORD>(-1);
    if (url.empty() || !WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &uc)) return false;
    if (!uc.lpszHostName || uc.dwHostNameLength == 0) return false;
    out.host.assign(uc.lpszHostName, uc.dwHostNameLength);
    out.path.assign(uc.lpszUrlPath ? uc.lpszUrlPath : L"", uc.lpszUrlPath ? uc.dwUrlPathLength : 0);
    if (uc.lpszExtraInfo && uc.dwExtraInfoLength) out.path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (out.path.empty()) out.path = L"/";
    out.port = uc.nPort;
    out.https = uc.nScheme == INTERNET_SCHEME_HTTPS;
    out.userInfo = uc.dwUserNameLength > 0 || uc.dwPasswordLength > 0;
    return true;
}

bool allowedDownload(const Url& u) {
    if (!u.https || u.userInfo || u.port != INTERNET_DEFAULT_HTTPS_PORT) return false;
    for (const wchar_t* h : kDownloadHosts)
        if (CompareStringOrdinal(u.host.c_str(), -1, h, -1, TRUE) == CSTR_EQUAL) return true;
    return false;
}

DWORD statusOf(HINTERNET req) {
    DWORD status = 0, size = sizeof status;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    return status;
}

std::wstring headerOf(HINTERNET req, DWORD query) {
    DWORD size = 0;
    WinHttpQueryHeaders(req, query, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &size, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0 || size > 16384) return {};
    std::wstring v(size / sizeof(wchar_t) + 1, L'\0');
    if (!WinHttpQueryHeaders(req, query, WINHTTP_HEADER_NAME_BY_INDEX, v.data(), &size, WINHTTP_NO_HEADER_INDEX))
        return {};
    v.resize(size / sizeof(wchar_t));
    return v;
}

std::string percentEncode(const std::string& s) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                     c == '_' || c == '.' || c == '~';
        if (plain) {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

bool isSha256Hex(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    return true;
}

class Sha256 {
public:
    Sha256() {
        ok_ = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg_, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
              BCRYPT_SUCCESS(BCryptCreateHash(alg_, &hash_, nullptr, 0, nullptr, 0, 0));
    }
    ~Sha256() {
        if (hash_) BCryptDestroyHash(hash_);
        if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
    }
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    void update(const void* data, size_t n) {
        if (ok_ && n) ok_ = BCRYPT_SUCCESS(BCryptHashData(hash_, static_cast<PUCHAR>(const_cast<void*>(data)),
                                                          static_cast<ULONG>(n), 0));
    }
    std::string hex() {      // "" when anything failed
        unsigned char out[32] = {};
        if (!ok_ || !BCRYPT_SUCCESS(BCryptFinishHash(hash_, out, sizeof out, 0))) return {};
        std::string s;
        char b[3];
        for (unsigned char c : out) {
            std::snprintf(b, sizeof b, "%02x", static_cast<unsigned>(c));
            s += b;
        }
        return s;
    }

private:
    BCRYPT_ALG_HANDLE alg_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    bool ok_ = false;
};

std::string hashFile(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    Sha256 sha;
    std::vector<char> buf(1 << 20);
    bool ok = true;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr)) { ok = false; break; }
        if (got == 0) break;
        sha.update(buf.data(), got);
    }
    CloseHandle(h);
    return ok ? sha.hex() : std::string();
}

std::wstring updatesDir() {
    std::wstring d = homeDir() + L"\\updates";
    ensureDir(d);
    return d;
}

std::wstring installerPath(const std::string& version) {
    return updatesDir() + L"\\CobaltConnectorSetup-" + widen(version) + L".exe";
}

bool nameChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

// What follows the releases prefix must be exactly "v<latest>/<name>.exe", the name made of letters, digits,
// '_', '-' and single dots between them. No '%', "..", '?' or '#': GitHub resolves ".." (and "%2e%2e") itself,
// so a looser check would let a URL that starts right fetch another repository's asset.
bool validReleasePath(const std::string& rest, const std::string& latest) {
    std::string tag = "v" + latest + "/";
    if (rest.compare(0, tag.size(), tag) != 0) return false;
    std::string file = rest.substr(tag.size());
    if (file.size() < 5 || file.size() > 200 || file.compare(file.size() - 4, 4, ".exe") != 0) return false;
    bool lastWasDot = true;                            // no leading dot, no ".."
    for (char c : file) {
        if (c == '.') {
            if (lastWasDot) return false;
            lastWasDot = true;
        } else if (nameChar(c)) {
            lastWasDot = false;
        } else {
            return false;
        }
    }
    return true;
}

bool validOffer(const UpdateOffer& o) {
    if (o.url.size() > 1000 || o.url.compare(0, sizeof kReleasePrefix - 1, kReleasePrefix) != 0) return false;
    if (!compareVersions(o.latest, o.latest).has_value()) return false;
    if (!validReleasePath(o.url.substr(sizeof kReleasePrefix - 1), o.latest)) return false;
    return isSha256Hex(o.sha256);
}

// Asks the server what it wants. UpToDate here means "an offer came back" (the caller compares versions).
UpdateOutcome askServer(UpdateOffer& offer, bool& serverRequired, std::wstring& message) {
    std::wstring headers;
    if (!fleetCredential(headers)) {
        message = L"this machine is not signed in to the Cobalt server yet, so no update check was made";
        return UpdateOutcome::Failed;
    }
    headers += L"Accept: application/json\r\n";
    std::string relay = relayUrl();
    std::string url = relay + "/api/fleet/connector?machine=" + percentEncode(machineId()) +
                      "&version=" COBALT_CONNECTOR_VERSION;
    ServerReply r;
    if (!serverRequest(url, L"GET", headers, "", r, 30000)) {     // wipes `headers`
        message = L"the Cobalt server could not be reached for the update check (" + r.error + L")";
        return UpdateOutcome::Failed;
    }
    DWORD status = r.status;
    const std::string& reply = r.body;
    if (status == 404) {
        message = L"the Cobalt server does not offer updates yet";
        return UpdateOutcome::NoInfo;
    }
    if (status != 200) {
        message = L"the update check was answered with " + std::to_wstring(status);
        return UpdateOutcome::Failed;
    }
    Json j;
    if (!parseJson(reply, j) || !j.isObject()) {
        message = L"the update check's answer could not be read";
        return UpdateOutcome::Failed;
    }
    const Json* block = j.get("connector");
    if (block && block->type == Json::Type::Null) {
        message = L"the Cobalt server names no release";
        return UpdateOutcome::NoInfo;
    }
    const Json& src = block && block->isObject() ? *block : j;
    offer.latest = trim(src.str("latest"));
    offer.minimum = trim(src.str("minimum"));
    offer.url = trim(src.str("url"));
    offer.sha256 = lower(trim(src.str("sha256")));
    offer.notes = src.str("notes");
    serverRequired = j.flag("required");
    if (offer.latest.empty() || offer.url.empty() || offer.sha256.empty()) {
        message = L"the Cobalt server names no release yet";
        return UpdateOutcome::NoInfo;
    }
    if (!validOffer(offer)) {
        message = L"the release the server named is not on the Cobalt Connector releases page, or has no proper "
                  L"SHA-256: it was ignored";
        return UpdateOutcome::NoInfo;
    }
    return UpdateOutcome::UpToDate;
}

// Downloads the installer into home\updates and checks it. Ready, Rejected (wrong hash or over 300 MB: the
// release itself is wrong) or Failed (the network, a download cut short, or a redirect off GitHub's download
// hosts: try again later).
UpdateOutcome download(const UpdateOffer& offer, std::wstring& file, std::wstring& message) {
    std::wstring done = installerPath(offer.latest);
    if (fileExists(done) && hashFile(done) == offer.sha256) {       // downloaded before and still good
        file = done;
        message = L"the installer for " + widen(offer.latest) + L" is already here and checked";
        return UpdateOutcome::Ready;
    }
    DeleteFileW(done.c_str());
    std::wstring part = done + L".part";

    Handle session(WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        message = L"the network could not be opened for the download";
        return UpdateOutcome::Failed;
    }
    WinHttpSetTimeouts(session.h, 15000, 20000, 30000, 60000);
    std::wstring url = widen(offer.url);
    for (int hop = 0; hop <= kMaxRedirects; ++hop) {
        if (g_cancel) { message = L"the download was stopped"; return UpdateOutcome::Failed; }
        Url u;
        if (!crack(url, u) || !allowedDownload(u)) {
            // Failed, not Rejected: GitHub may move its download host, and the release itself is not at fault
            message = L"the download was sent to an address that is not GitHub's (" + url.substr(0, 120) +
                      L"): nothing was downloaded";
            return UpdateOutcome::Failed;
        }
        Handle conn(WinHttpConnect(session.h, u.host.c_str(), u.port, 0));
        Handle req(conn ? WinHttpOpenRequest(conn.h, L"GET", u.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                        : nullptr);
        if (!req) {
            message = L"the download could not be started (error " + std::to_wstring(GetLastError()) + L")";
            return UpdateOutcome::Failed;
        }
        DWORD off = WINHTTP_DISABLE_REDIRECTS;            // every hop is checked here instead
        WinHttpSetOption(req.h, WINHTTP_OPTION_DISABLE_FEATURE, &off, sizeof off);
        if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(req.h, nullptr)) {
            message = L"the download could not reach GitHub (error " + std::to_wstring(GetLastError()) + L")";
            return UpdateOutcome::Failed;
        }
        DWORD status = statusOf(req.h);
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            std::wstring next = headerOf(req.h, WINHTTP_QUERY_LOCATION);
            if (next.empty()) {
                message = L"GitHub sent the download on without saying where";
                return UpdateOutcome::Failed;
            }
            if (next[0] == L'/') next = L"https://" + u.host + next;
            url = next;
            continue;
        }
        if (status != 200) {
            message = L"GitHub answered the download with " + std::to_wstring(status);
            return UpdateOutcome::Failed;
        }
        std::wstring length = headerOf(req.h, WINHTTP_QUERY_CONTENT_LENGTH);
        if (!length.empty() && _wcstoui64(length.c_str(), nullptr, 10) > kMaxDownload) {
            message = L"the installer is larger than 300 MB: not downloaded";
            return UpdateOutcome::Rejected;
        }
        HANDLE out = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (out == INVALID_HANDLE_VALUE) {
            message = L"the installer could not be saved (error " + std::to_wstring(GetLastError()) + L")";
            return UpdateOutcome::Failed;
        }
        Sha256 sha;
        unsigned long long total = 0;
        std::vector<char> buf(256 * 1024);
        UpdateOutcome result = UpdateOutcome::Ready;
        for (;;) {
            if (g_cancel) { message = L"the download was stopped"; result = UpdateOutcome::Failed; break; }
            DWORD got = 0;
            if (!WinHttpReadData(req.h, buf.data(), static_cast<DWORD>(buf.size()), &got)) {
                message = L"the download broke off (error " + std::to_wstring(GetLastError()) + L")";
                result = UpdateOutcome::Failed;
                break;
            }
            if (got == 0) break;
            total += got;
            if (total > kMaxDownload) {
                message = L"the installer is larger than 300 MB: not kept";
                result = UpdateOutcome::Rejected;
                break;
            }
            sha.update(buf.data(), got);
            DWORD put = 0;
            if (!WriteFile(out, buf.data(), got, &put, nullptr) || put != got) {
                message = L"the installer could not be saved (the disk may be full)";
                result = UpdateOutcome::Failed;
                break;
            }
        }
        CloseHandle(out);
        if (result == UpdateOutcome::Ready) {
            std::string got = sha.hex();
            if (got.empty()) {
                message = L"the installer's SHA-256 could not be computed";
                result = UpdateOutcome::Failed;
            } else if (!length.empty() && _wcstoui64(length.c_str(), nullptr, 10) != total) {
                // cut short: the network's fault, not the release's, so not marked bad
                message = L"the download ended early; trying again later";
                result = UpdateOutcome::Failed;
            } else if (got != offer.sha256) {
                message = L"the installer's SHA-256 does not match what the Cobalt server said: deleted, not run";
                result = UpdateOutcome::Rejected;
            }
        }
        if (result != UpdateOutcome::Ready) {
            DeleteFileW(part.c_str());
            return result;
        }
        if (!MoveFileExW(part.c_str(), done.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileW(part.c_str());
            message = L"the installer could not be put in place";
            return UpdateOutcome::Failed;
        }
        file = done;
        message = L"downloaded and checked the installer for " + widen(offer.latest) + L" (" +
                  std::to_wstring(total / (1024 * 1024)) + L" MB)";
        return UpdateOutcome::Ready;
    }
    message = L"the download was redirected too many times";
    return UpdateOutcome::Failed;
}

UpdateResult runJob(bool allowDownload, bool forced) {
    UpdateResult r;
    r.forced = forced;
    bool serverRequired = false;
    UpdateOutcome asked = askServer(r.offer, serverRequired, r.message);
    if (asked != UpdateOutcome::UpToDate) {
        r.outcome = asked;
        return r;
    }
    std::optional<int> vsLatest = compareVersions(r.offer.latest, COBALT_CONNECTOR_VERSION);
    if (!vsLatest || *vsLatest <= 0) {               // the same or older: never downgrade
        r.outcome = UpdateOutcome::UpToDate;
        return r;
    }
    r.offer.newer = true;
    std::optional<int> vsMinimum = compareVersions(r.offer.minimum, COBALT_CONNECTOR_VERSION);
    r.offer.required = (vsMinimum && *vsMinimum > 0) || serverRequired;
    if (isBadRelease(readUpdateState(), r.offer.latest, r.offer.sha256)) {
        r.outcome = UpdateOutcome::Skipped;
        return r;
    }
    if (!allowDownload && !forced && !r.offer.required) {
        r.outcome = UpdateOutcome::Available;
        return r;
    }
    r.outcome = download(r.offer, r.file, r.message);
    return r;
}

}  // namespace

std::optional<int> compareVersions(const std::string& a, const std::string& b) {
    auto parse = [](const std::string& v, long long out[3]) {
        int part = 0, digits = 0;
        long long cur = 0;
        for (char c : v) {
            if (c >= '0' && c <= '9') {
                if (++digits > 6) return false;
                cur = cur * 10 + (c - '0');
            } else if (c == '.') {
                if (!digits || part >= 2) return false;
                out[part++] = cur;
                cur = 0;
                digits = 0;
            } else {
                return false;
            }
        }
        if (!digits || part != 2) return false;
        out[2] = cur;
        return true;
    };
    long long x[3] = {}, y[3] = {};
    if (!parse(a, x) || !parse(b, y)) return std::nullopt;
    for (int i = 0; i < 3; ++i)
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}

bool startUpdateThread(HWND notify, bool allowDownload, bool forced) {
    bool expected = false;
    if (!g_busy.compare_exchange_strong(expected, true)) return false;
    g_cancel = false;
    try {
        std::thread([notify, allowDownload, forced] {
            auto* r = new UpdateResult(runJob(allowDownload, forced));
            g_busy = false;
            if (g_cancel || !PostMessageW(notify, WM_APP_UPDATE, 0, reinterpret_cast<LPARAM>(r))) delete r;
        }).detach();
    } catch (...) {
        g_busy = false;
        return false;
    }
    return true;
}

bool updateThreadBusy() { return g_busy; }

void cancelUpdates() { g_cancel = true; }

bool launchInstaller(const UpdateResult& r, std::wstring& message) {
    if (r.outcome != UpdateOutcome::Ready || r.file.empty() || !fileExists(r.file)) {
        message = L"there is no checked installer to run";
        return false;
    }
    if (hashFile(r.file) != r.offer.sha256) {        // once more, right before it runs
        DeleteFileW(r.file.c_str());
        message = L"the installer changed after it was checked: deleted, not run";
        return false;
    }
    Json st = readUpdateState();
    st.set("target", Json::string(r.offer.latest));
    st.set("sha256", Json::string(r.offer.sha256));
    st.set("started", Json::number(static_cast<double>(std::time(nullptr))));
    writeUpdateState(st);

    // /UPDATE: keep the keys and every setting; /DIR: this install's own folder; /STARTNOW=1: the installer
    // starts the new version (in the tray only, as the install is silent) once it is in place.
    std::wstring params = L"/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS /UPDATE /STARTNOW=1 /DIR=\"" +
                          exeDir() + L"\" /LOG=\"" + logsDir() + L"\\update-setup.log\"";
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof sei;
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"open";
    sei.lpFile = r.file.c_str();
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) {
        DWORD err = GetLastError();
        st.set("target", Json::string(""));
        writeUpdateState(st);
        message = L"the installer could not be started (error " + std::to_wstring(err) + L")";
        return false;
    }
    message = L"installing " + widen(r.offer.latest) + L"; the installer starts the new version";
    return true;
}

Json readUpdateState() { return readJsonFile(homeFile(L"update_state.json")); }

void writeUpdateState(const Json& st) { writeJsonFile(homeFile(L"update_state.json"), st); }

bool isBadRelease(const Json& st, const std::string& version, const std::string& sha256) {
    const Json* bad = st.get("bad");
    if (!bad || bad->type != Json::Type::Array) return false;
    std::string id = version + "|" + lower(sha256);
    for (const Json& b : bad->a)
        if (b.type == Json::Type::String && b.s == id) return true;
    return false;
}

bool strikeRelease(Json& st, const std::string& version, const std::string& sha256) {
    if (version.empty()) return false;
    std::string id = version + "|" + lower(sha256);
    Json strikes = Json::object();
    if (const Json* s = st.get("strikes"); s && s->isObject()) strikes = *s;
    int n = static_cast<int>(strikes.num(id)) + 1;
    if (n >= kMaxReleaseStrikes) {
        addBadRelease(st, version, sha256);
        st.set("strikes", Json::object());
        return true;
    }
    strikes.set(id, Json::number(n));
    st.set("strikes", strikes);
    return false;
}

void addBadRelease(Json& st, const std::string& version, const std::string& sha256) {
    if (version.empty() || isBadRelease(st, version, sha256)) return;
    Json list = Json::array();
    if (const Json* bad = st.get("bad"); bad && bad->type == Json::Type::Array) list = *bad;
    list.a.push_back(Json::string(version + "|" + lower(sha256)));
    while (list.a.size() > 20) list.a.erase(list.a.begin());
    st.set("bad", list);
}

}  // namespace cw
