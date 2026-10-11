// Signing this machine in to the Cobalt server (Cobalt Connector 1.3.0; docs: README "Your keys").
//
// - Enrol, once: the stored partner key goes to POST /api/fleet/enrol one time, with the public half of the machine's
//   own device key (devkey.cpp) and a signature. The server answers with a device id and the first pass (a token that
//   lasts one hour). From then on the partner key is never sent again - unless the server forgets the machine (it
//   answers "device_unknown"), the device key is lost (a cleared TPM), or the owner stores a different partner key.
// - Renew: before the pass has 20 minutes left (about every 40 minutes), on resume from sleep, and when the work
//   process leaves token.stale (the server refused its pass), the app signs a fresh request to POST /api/fleet/token
//   (no key, no pass: the signature is the credential). Failures back off 1, 2, 4, 8, 16, then 30 minutes.
// - Hand-off: the pass goes into %LOCALAPPDATA%\CobaltConnector\token.json, written atomically with a protected
//   access list that grants only this Windows user. The work process is started with COBALT_TOKEN_FILE and never
//   with the partner key; it reads the file again whenever it changes, so a renewal never restarts it.
// - Removed by Cobalt's owner: the app stops the work process and says so. It does not register again by itself
//   (that would undo the removal): every 30 minutes it asks the token route, signed with its own key and without the
//   partner key. The server answers "device_revoked" while the machine stays removed and "device_unknown" once the
//   owner allows it again; only then does the app register again, sending the partner key that one time.
// - The device key not opening (the TPM not ready at logon or after resume, a lockout) is waited out: the key, the
//   device and the pass are kept. A new key is made only when Windows says the key no longer exists (a cleared TPM).
// - An older server (no /enrol route, before this machine ever enrolled): the app runs as 1.2.1 did, with the
//   partner key in the work process's environment, and asks again every 30 minutes. Once enrolled it never falls back.
// Nothing here logs a key, a pass, a nonce or a signature: the log names the device, the machine and the outcome.
#include "app.hpp"

#include <bcrypt.h>
#include <sddl.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <thread>

namespace cw {

namespace {

constexpr long long kRenewBefore = 20 * 60;     // renew when under 20 minutes are left
constexpr long long kRemovedRetry = 30 * 60;    // a removed machine (or a revoked key) asks again this often
constexpr long long kRefusedRetry = 5 * 60;     // a refused partner key
constexpr long long kLegacyRecheck = 30 * 60;   // an older server: does it offer device keys yet?
constexpr long long kMinEnrolGap = 60;          // two enrols are at least a minute apart
constexpr ULONGLONG kStaleGapMs = 60000;        // token.stale is acted on at most once a minute

const wchar_t kRemovedText[] = L"Cobalt's owner has removed this machine from the fleet. Ask them to allow it again; "
                               L"Cobalt Connector checks every 30 minutes and starts again by itself once they have.";
const wchar_t kKeyRevokedText[] = L"Your partner key has been revoked on the Cobalt server. Run the Cobalt Connector "
                                  L"installer again and enter a new key.";
const wchar_t kNoKeyText[] = L"No partner key is stored. Run the Cobalt Connector installer again and enter your "
                             L"partner key.";

std::mutex g_state;
SignIn g_mode = SignIn::Waiting;
std::wstring g_problem;
unsigned g_generation = 0;

std::mutex g_cfg;                                  // contributor.json's "device" is changed only under this
std::atomic<long long> g_offset{0};                // server time - this PC's time, from the last enrol/token reply
std::atomic<bool> g_stop{false};
std::atomic<bool> g_force{false};
HANDLE g_wake = nullptr;
std::thread g_thread;
ULONGLONG g_lastForced = 0;

long long serverNow() { return static_cast<long long>(std::time(nullptr)) + g_offset.load(); }

void setMode(SignIn mode, const std::wstring& problem = L"") {
    std::lock_guard<std::mutex> lock(g_state);
    if (mode == g_mode && problem == g_problem) return;
    if (mode != g_mode) ++g_generation;
    static const wchar_t* const names[] = {L"waiting", L"device key", L"partner key (older server)", L"stopped"};
    logLine(L"sign-in: " + std::wstring(names[static_cast<int>(mode)]) + (problem.empty() ? L"" : L" - " + problem));
    g_mode = mode;
    g_problem = problem;
}

SignIn currentMode() {
    std::lock_guard<std::mutex> lock(g_state);
    return g_mode;
}

// ---------------------------------------------------------------- contributor.json "device"

Json deviceRecord() {
    Json cfg = readJsonFile(homeFile(L"contributor.json"));
    const Json* d = cfg.get("device");
    return d && d->isObject() ? *d : Json::object();
}

// The server has given this machine a device before: from then on an answer that looks like an older server is an
// outage, never a reason to hand the partner key to the work process again.
bool everEnrolled() { return readJsonFile(homeFile(L"contributor.json")).flag("device_keys"); }

void saveDevice(const Json& dev) {
    std::lock_guard<std::mutex> lock(g_cfg);
    std::wstring path = homeFile(L"contributor.json");
    Json cfg = readJsonFile(path);
    if (dev.isObject() && !dev.o.empty()) {
        cfg.set("device", dev);
        cfg.set("device_keys", Json::boolean(true));
    } else {
        cfg.set("device", Json::object());
    }
    writeJsonFile(path, cfg);
}

// The server should forget this device (it no longer knows it, the server address or the partner key changed): the
// id goes, but the record keeps which key store holds the machine's key, so a TPM that is only slow to answer is
// never taken for a machine without a key.
void forgetDevice(const Json& dev) {
    Json d = Json::object();
    std::string provider = dev.str("provider");
    if (!provider.empty()) d.set("provider", Json::string(provider));
    saveDevice(d);
}

// A short tag of the partner key the device was enrolled with (12 hex of its SHA-256), so a different key stored
// later (a reinstall with a new key) is noticed and the machine registers again with it. Not the key, and not enough
// of its hash to check a guess against the server's.
std::string keyTag(const std::string& key) {
    unsigned char d[32];
    if (!BCRYPT_SUCCESS(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
                                   reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())),
                                   static_cast<ULONG>(key.size()), d, sizeof d)))
        return {};
    char hex[13];
    std::snprintf(hex, sizeof hex, "%02x%02x%02x%02x%02x%02x", d[0], d[1], d[2], d[3], d[4], d[5]);
    return hex;
}

// ---------------------------------------------------------------- token.json, readable by this Windows user only

std::wstring userSid() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return {};
    DWORD size = 0;
    GetTokenInformation(tok, TokenUser, nullptr, 0, &size);
    std::string buf(size, '\0');
    std::wstring sid;
    if (size && GetTokenInformation(tok, TokenUser, buf.data(), size, &size)) {
        LPWSTR s = nullptr;
        if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &s)) {
            sid = s;
            LocalFree(s);
        }
    }
    CloseHandle(tok);
    return sid;
}

bool writeTokenFile(const std::string& token, long long expires, const std::string& device, const std::string& machine) {
    std::wstring sid = userSid();
    if (sid.empty()) return false;
    // protected (no inherited entries), full control for this user, nobody else
    std::wstring sddl = L"D:P(A;;FA;;;" + sid + L")";
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) return false;
    SECURITY_ATTRIBUTES sa{sizeof sa, sd, FALSE};
    Json j = Json::object();
    j.set("token", Json::string(token));
    j.set("expires", Json::number(static_cast<double>(expires)));
    j.set("device", Json::string(device));
    j.set("machine", Json::string(machine));
    std::string text = j.dump(2);
    std::wstring path = tokenFilePath(), tmp = path + L".tmp";
    DeleteFileW(tmp.c_str());              // CREATE_NEW: the new file gets this access list, never an old file's
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, &sa, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    LocalFree(sd);
    bool ok = h != INVALID_HANDLE_VALUE;
    if (ok) {
        DWORD put = 0;
        ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &put, nullptr) && put == text.size();
        FlushFileBuffers(h);
        CloseHandle(h);
    }
    SecureZeroMemory(text.data(), text.size());
    if (!ok) { DeleteFileW(tmp.c_str()); return false; }
    // a rename keeps the file's own access list; the work process may be reading the old file for a moment
    for (int i = 0; i < 40; ++i) {
        if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
        Sleep(50);
    }
    DeleteFileW(tmp.c_str());
    return false;
}

struct HeldToken {
    std::string token, device;
    long long expires = 0;
};

HeldToken readTokenFile() {
    HeldToken t;
    Json j = readJsonFile(tokenFilePath());
    t.token = j.str("token");
    t.device = j.str("device");
    t.expires = static_cast<long long>(j.num("expires"));
    return t;
}

void deleteTokenFile() {
    DeleteFileW(tokenFilePath().c_str());
    DeleteFileW(homeFile(L"token.stale").c_str());
}

// ---------------------------------------------------------------- the two signed calls

enum class Got { Ok, NoRoute, KeyRefused, DeviceRevoked, KeyRevoked, DeviceUnknown, TooMany, RateLimited,
                 BadSignature, KeyUnusable, Failed };

struct Outcome {
    Got got = Got::Failed;
    std::wstring text;             // plain words for the log or the window; never a secret
    long long retryAfter = 0;
    Json reply;
};

void noteServerTime(const Json& j) {
    double t = j.num("server_time");
    if (t > 1e9) g_offset = static_cast<long long>(t) - static_cast<long long>(std::time(nullptr));
}

std::wstring detailOf(const ServerReply& r) {
    std::string d = r.json.isObject() ? r.json.str("detail") : std::string();
    return widen(d.substr(0, 300));
}

Outcome classify(const ServerReply& r) {
    Outcome o;
    o.reply = r.json;
    std::wstring detail = detailOf(r);
    const std::string& why = r.reason;
    if (r.status == 200) {
        if (r.json.isObject() && !r.json.str("token").empty()) { o.got = Got::Ok; return o; }
        o.got = Got::NoRoute;               // an older server's catch-all page
        o.text = L"the server's answer carries no sign-in";
        return o;
    }
    if (r.status == 404 || r.status == 405) { o.got = Got::NoRoute; o.text = L"the server has no device sign-in yet"; return o; }
    if (r.status == 429) {
        o.got = Got::RateLimited;
        o.retryAfter = r.retryAfter > 0 ? r.retryAfter : 3600;
        o.text = detail.empty() ? L"too many sign-ins for now" : detail;
        return o;
    }
    if (why == "key_refused") { o.got = Got::KeyRefused; o.text = detail; return o; }
    if (why == "device_revoked") { o.got = Got::DeviceRevoked; o.text = kRemovedText; return o; }
    if (why == "key_revoked") { o.got = Got::KeyRevoked; o.text = kKeyRevokedText; return o; }
    if (why == "device_unknown" || why == "machine_mismatch") { o.got = Got::DeviceUnknown; o.text = widen(why); return o; }
    if (why == "too_many_devices" || r.status == 409) { o.got = Got::TooMany; o.text = detail; return o; }
    if (why == "bad_signature") { o.got = Got::BadSignature; o.text = L"the server did not accept this machine's signature"; return o; }
    o.got = Got::Failed;
    o.text = L"the server answered " + std::to_wstring(r.status) + (why.empty() ? L"" : L" (" + widen(why) + L")") +
             (detail.empty() ? L"" : L": " + detail);
    return o;
}

// One signed call; a clock refusal is retried once at once with the server's time.
template <typename MakeBody>
Outcome signedCall(const std::string& url, const std::string* partnerKey, const DeviceKey& k, MakeBody makeBody) {
    Outcome o;
    for (int attempt = 0; attempt < 2; ++attempt) {
        long long ts = serverNow();
        std::string nonce = randomNonce();
        std::string text;
        Json body = makeBody(ts, nonce, text);
        std::string sig;
        std::wstring err;
        if (!signWithDeviceKey(k, text, sig, err)) {
            o.got = Got::KeyUnusable;
            o.text = err;
            return o;
        }
        body.set("signature", Json::string(sig));
        std::string payload = body.dump(0);
        std::wstring headers = L"Content-Type: application/json\r\nAccept: application/json\r\n";
        if (partnerKey) headers += L"X-Cobalt-Contributor: " + widen(*partnerKey) + L"\r\n";
        ServerReply r;
        bool reached = serverRequest(url, L"POST", headers, payload, r, 20000);   // serverRequest wipes `headers`
        if (!reached) {
            o.got = Got::Failed;
            o.text = r.error;
            return o;
        }
        noteServerTime(r.json);
        if (r.status == 400 && r.reason == "clock" && attempt == 0) {
            logLine(L"sign-in: this PC's clock is off by " + std::to_wstring(g_offset.load()) +
                    L" s from the server's; signing again with the server's time");
            continue;
        }
        return classify(r);
    }
    o.got = Got::Failed;
    o.text = L"the server refused this PC's time twice";
    return o;
}

Outcome callEnrol(const std::string& relay, const std::string& key, const std::string& machine, const DeviceKey& k) {
    std::string aud = relayAudience(relay);
    return signedCall(relay + "/api/fleet/enrol", &key, k, [&](long long ts, const std::string& nonce, std::string& text) {
        text = "cobalt-connector/enrol/v1\n" + aud + "\n" + machine + "\n" + k.publicKey + "\n" + std::to_string(ts) +
               "\n" + nonce;
        Json b = Json::object();
        b.set("machine", Json::string(machine));
        b.set("public_key", Json::string(k.publicKey));
        b.set("alg", Json::string("ES256"));
        b.set("provider", Json::string(k.provider));
        b.set("ts", Json::number(static_cast<double>(ts)));
        b.set("nonce", Json::string(nonce));
        b.set("app_version", Json::string(COBALT_CONNECTOR_VERSION));
        return b;
    });
}

Outcome callToken(const std::string& relay, const std::string& device, const std::string& machine, const DeviceKey& k) {
    std::string aud = relayAudience(relay);
    return signedCall(relay + "/api/fleet/token", nullptr, k, [&](long long ts, const std::string& nonce, std::string& text) {
        text = "cobalt-connector/token/v1\n" + aud + "\n" + device + "\n" + machine + "\n" + std::to_string(ts) + "\n" + nonce;
        Json b = Json::object();
        b.set("device", Json::string(device));
        b.set("machine", Json::string(machine));
        b.set("ts", Json::number(static_cast<double>(ts)));
        b.set("nonce", Json::string(nonce));
        return b;
    });
}

enum class Made { Ready, Wait, CannotMake };

// The machine's device key: the one already made, else a new one (TPM first). A key that exists but cannot be opened
// just now is waited for (Wait) - a new one is made only when there never was one or Windows says it is gone, so a
// slow TPM never pushes the machine into the software key store.
Made deviceKey(const std::string& machine, const Json& dev, DeviceKey& k, std::wstring& err) {
    bool hadKey = !dev.str("provider").empty();
    KeyState st = openDeviceKey(machine, dev.str("provider", "tpm"), k, err);
    if (st == KeyState::Ok) return Made::Ready;
    if (st == KeyState::Unavailable && hadKey) return Made::Wait;
    if (!createDeviceKey(machine, k, err)) return Made::CannotMake;
    logLine(L"device key made for machine " + widen(machine) + L" (" +
            (k.provider == "tpm" ? L"in the TPM" : L"in Windows' software key store") + L")");
    return Made::Ready;
}

// Saves what an enrol gave: the device, its first pass. True when both are in place.
bool keepEnrolment(const Outcome& o, const DeviceKey& k, const std::string& machine, const std::string& relay,
                   const std::string& key) {
    Json dev = Json::object();
    dev.set("id", Json::string(o.reply.str("device")));
    dev.set("key", Json::string("CobaltConnector-device-" + machine));
    dev.set("provider", Json::string(k.provider));
    dev.set("enrolled", Json::number(static_cast<double>(std::time(nullptr))));
    dev.set("relay", Json::string(normalRelay(relay)));
    dev.set("key_tag", Json::string(keyTag(key)));
    saveDevice(dev);
    std::string token = o.reply.str("token");
    bool ok = writeTokenFile(token, static_cast<long long>(o.reply.num("expires")), dev.str("id"), machine);
    SecureZeroMemory(token.data(), token.size());
    logLine(L"sign-in: this machine is registered as device " + widen(dev.str("id")) + L" of " +
            widen(o.reply.str("owner")) + L" (" + widen(k.provider) + L" key); the partner key is not sent again");
    return ok;
}

// ---------------------------------------------------------------- the sign-in thread

class Manager {
public:
    long long step(bool forced);

private:
    long long enrolStep(const std::string& relay, const std::string& machine, const Json& dev);
    long long removedStep(const std::string& relay, const std::string& machine, const Json& dev);
    long long tokenStep(const std::string& relay, const std::string& machine, const Json& dev, bool forced);
    long long backoff() {
        static const long long minutes[] = {1, 2, 4, 8, 16, 30};
        int i = std::min(failures_++, 5);
        return minutes[i] * 60;
    }
    void offlineIsDevice() {   // unreachable before any decision: the work process starts with the token file
        if (currentMode() == SignIn::Waiting) setMode(SignIn::Device);
    }
    int failures_ = 0;
    bool checkedTag_ = false;
    long long lastEnrol_ = 0;
};

long long Manager::step(bool forced) {
    std::string relay = relayUrl(), machine = machineId();
    Json dev = deviceRecord();
    bool have = !dev.str("id").empty();
    if (have && normalRelay(dev.str("relay", relay)) != normalRelay(relay)) {
        logLine(L"sign-in: the server address changed; this machine registers with the new one");
        forgetDevice(dev);
        deleteTokenFile();
        have = false;
    }
    if (have && !checkedTag_) {
        checkedTag_ = true;
        if (std::optional<std::string> key = loadSecret(L"partner")) {
            bool changed = !dev.str("key_tag").empty() && keyTag(*key) != dev.str("key_tag");
            SecureZeroMemory(key->data(), key->size());
            if (changed) {
                logLine(L"sign-in: a different partner key is stored; this machine registers again with it");
                forgetDevice(dev);
                deleteTokenFile();
                have = false;
            }
        }
    }
    if (!have) return enrolStep(relay, machine, dev);
    if (dev.flag("revoked")) return removedStep(relay, machine, dev);
    return tokenStep(relay, machine, dev, forced);
}

// A machine Cobalt's owner removed asks the token route, signed and without the partner key, whether it is allowed
// back: "device_revoked" while it stays removed; "device_unknown" once the owner allowed it again, and only then does
// it register again (the one time the partner key is sent).
long long Manager::removedStep(const std::string& relay, const std::string& machine, const Json& dev) {
    setMode(SignIn::Blocked, kRemovedText);
    std::string device = dev.str("id");
    DeviceKey k;
    std::wstring err;
    KeyState st = openDeviceKey(machine, dev.str("provider", "tpm"), k, err);
    if (st == KeyState::Missing) {
        logLine(L"sign-in: " + err + L"; the removed machine can only ask by registering again");
        forgetDevice(dev);
        return kRemovedRetry;
    }
    if (st == KeyState::Unavailable) {
        logLine(L"sign-in: " + err + L"; asking again later whether this machine is allowed back");
        return kRemovedRetry;
    }
    Outcome o = callToken(relay, device, machine, k);
    switch (o.got) {
    case Got::DeviceRevoked:
        return kRemovedRetry;
    case Got::DeviceUnknown:
        logLine(L"sign-in: device " + widen(device) + L" is no longer removed; this machine registers again");
        forgetDevice(dev);
        return 1;
    case Got::Ok: {
        std::string token = o.reply.str("token");
        long long expires = static_cast<long long>(o.reply.num("expires"));
        bool ok = writeTokenFile(token, expires, device, machine);
        SecureZeroMemory(token.data(), token.size());
        if (!ok) return backoff();
        Json d = dev;
        std::erase_if(d.o, [](const auto& kv) { return kv.first == "revoked"; });
        saveDevice(d);
        failures_ = 0;
        logLine(L"sign-in: device " + widen(device) + L" is signed in again");
        setMode(SignIn::Device);
        return std::max<long long>(60, expires - kRenewBefore - serverNow());
    }
    case Got::KeyRevoked:
        setMode(SignIn::Blocked, kKeyRevokedText);
        return kRemovedRetry;
    case Got::RateLimited:
        return std::max<long long>(60, o.retryAfter);
    default:
        logLine(L"sign-in: asking whether this machine is allowed back did not work: " + o.text);
        return kRemovedRetry;
    }
}

long long Manager::enrolStep(const std::string& relay, const std::string& machine, const Json& dev) {
    long long now = static_cast<long long>(std::time(nullptr));
    if (now - lastEnrol_ < kMinEnrolGap) return kMinEnrolGap - (now - lastEnrol_);
    std::optional<std::string> key = loadSecret(L"partner");
    if (!key) {
        setMode(SignIn::Blocked, kNoKeyText);
        return 60;
    }
    DeviceKey k;
    std::wstring err;
    Made made = deviceKey(machine, dev, k, err);
    if (made == Made::Wait) {
        SecureZeroMemory(key->data(), key->size());
        logLine(L"sign-in: " + err + L"; the key is kept and Cobalt Connector tries again");
        setMode(SignIn::Blocked, L"Windows cannot open this machine's sign-in key just now (" + err + L"). Cobalt "
                                 L"Connector keeps trying.");
        return backoff();
    }
    if (made == Made::CannotMake) {
        SecureZeroMemory(key->data(), key->size());
        if (!everEnrolled()) {
            // Windows cannot make a key at all here: run as 1.2.1 did rather than not at all
            logLine(L"sign-in: no device key could be made (" + err + L"); using the partner key as before");
            setMode(SignIn::Legacy);
            return kLegacyRecheck;
        }
        setMode(SignIn::Blocked, L"Windows could not make this machine's sign-in key (" + err + L"). Cobalt Connector "
                                 L"keeps trying.");
        return backoff();
    }
    lastEnrol_ = now;
    Outcome o = callEnrol(relay, *key, machine, k);
    if (o.got == Got::Ok) {
        bool kept = keepEnrolment(o, k, machine, relay, *key);
        SecureZeroMemory(key->data(), key->size());
        if (!kept) {
            logLine(L"sign-in: the pass could not be saved; trying again");
            return backoff();
        }
        failures_ = 0;
        setMode(SignIn::Device);
        return std::max<long long>(60, static_cast<long long>(o.reply.num("expires")) - kRenewBefore - serverNow());
    }
    SecureZeroMemory(key->data(), key->size());
    logLine(L"sign-in: registering this machine did not work: " + o.text);
    switch (o.got) {
    case Got::NoRoute:
        if (everEnrolled()) { offlineIsDevice(); return backoff(); }
        setMode(SignIn::Legacy);
        return kLegacyRecheck;
    case Got::BadSignature:
        // a server that cannot check this machine's signature (a proxy that renames the host, say) before the
        // machine ever enrolled: it keeps working as 1.2.1 did rather than not at all, and asks again later
        if (everEnrolled()) { offlineIsDevice(); return backoff(); }
        setMode(SignIn::Legacy);
        return kLegacyRecheck;
    case Got::KeyRefused:
        setMode(SignIn::Blocked, L"The Cobalt server refused your partner key" + (o.text.empty() ? L"." : L": " + o.text) +
                                 L" Run the Cobalt Connector installer again and enter a new key.");
        return kRefusedRetry;
    case Got::DeviceRevoked: {
        Json d = dev;
        if (!d.str("id").empty()) { d.set("revoked", Json::boolean(true)); saveDevice(d); }
        deleteTokenFile();
        setMode(SignIn::Blocked, kRemovedText);
        return kRemovedRetry;
    }
    case Got::TooMany:
        setMode(SignIn::Blocked, o.text.empty() ? std::wstring(L"Your partner key has too many machines registered. Ask "
                                                               L"Cobalt's owner to remove one you no longer use.")
                                                : o.text);
        return kRemovedRetry;
    case Got::RateLimited:
        offlineIsDevice();
        return std::max<long long>(60, o.retryAfter);
    case Got::KeyUnusable:                  // the key opened but did not sign: kept, and tried again (never deleted)
        offlineIsDevice();
        return backoff();
    default:
        offlineIsDevice();
        return backoff();
    }
}

long long Manager::tokenStep(const std::string& relay, const std::string& machine, const Json& dev, bool forced) {
    std::string device = dev.str("id");
    HeldToken held = readTokenFile();
    long long left = held.expires - serverNow();
    if (!forced && !held.token.empty() && held.device == device && left > kRenewBefore) {
        setMode(SignIn::Device);
        return left - kRenewBefore;
    }
    SecureZeroMemory(held.token.data(), held.token.size());
    DeviceKey k;
    std::wstring err;
    KeyState st = openDeviceKey(machine, dev.str("provider", "tpm"), k, err);
    if (st == KeyState::Missing) {           // Windows says the key is gone (a cleared TPM): a new one, a new device
        logLine(L"sign-in: " + err + L"; a new key is made and the machine registers again");
        forgetDevice(dev);
        deleteTokenFile();
        return 1;
    }
    if (st == KeyState::Unavailable) {       // not ready, locked out, its service not up: wait, keep everything
        logLine(L"sign-in: " + err + L"; the key and the pass are kept and Cobalt Connector tries again");
        setMode(SignIn::Device);            // the work process keeps the pass it has; it may still be good
        return backoff();
    }
    Outcome o = callToken(relay, device, machine, k);
    switch (o.got) {
    case Got::Ok: {
        std::string token = o.reply.str("token");
        long long expires = static_cast<long long>(o.reply.num("expires"));
        bool ok = writeTokenFile(token, expires, device, machine);
        SecureZeroMemory(token.data(), token.size());
        if (!ok) { logLine(L"sign-in: the new pass could not be saved"); return backoff(); }
        failures_ = 0;
        logLine(L"sign-in: renewed the pass of device " + widen(device));
        setMode(SignIn::Device);
        return std::max<long long>(60, expires - kRenewBefore - serverNow());
    }
    case Got::DeviceUnknown:
        logLine(L"sign-in: the server no longer knows device " + widen(device) + L" (" + o.text + L"); registering again");
        forgetDevice(dev);
        deleteTokenFile();
        return 1;
    case Got::DeviceRevoked: {
        logLine(L"sign-in: Cobalt's owner removed device " + widen(device));
        Json d = dev;
        d.set("revoked", Json::boolean(true));
        saveDevice(d);
        deleteTokenFile();
        setMode(SignIn::Blocked, kRemovedText);
        return kRemovedRetry;
    }
    case Got::KeyRevoked:
        logLine(L"sign-in: the partner key of device " + widen(device) + L" is revoked");
        deleteTokenFile();
        setMode(SignIn::Blocked, kKeyRevokedText);
        return kRemovedRetry;
    case Got::KeyUnusable:                  // opened but did not sign just now: kept, with the device and the pass
        logLine(L"sign-in: " + o.text + L"; the key and the pass are kept and Cobalt Connector tries again");
        setMode(SignIn::Device);
        return backoff();
    case Got::RateLimited:
        logLine(L"sign-in: renewing the pass was refused for now: " + o.text);
        return std::max<long long>(60, o.retryAfter);
    default:
        logLine(L"sign-in: renewing the pass did not work: " + o.text);
        setMode(SignIn::Device);            // the work process keeps the pass it has; it may still be good
        return backoff();
    }
}

void threadMain() {
    Manager m;
    while (!g_stop) {
        long long wait = 60;
        try {
            wait = m.step(g_force.exchange(false));
        } catch (...) {
            logLine(L"sign-in: an unexpected error; trying again in a minute");
        }
        wait = std::clamp<long long>(wait, 1, 24 * 3600);
        if (g_wake) WaitForSingleObject(g_wake, static_cast<DWORD>(wait * 1000));
        else Sleep(1000);
    }
}

}  // namespace

std::wstring tokenFilePath() { return homeFile(L"token.json"); }

void startSignIn() {
    if (g_thread.joinable()) return;
    Json dev = deviceRecord();
    if (!dev.str("id").empty() && dev.flag("revoked")) setMode(SignIn::Blocked, kRemovedText);
    else if (!dev.str("id").empty()) setMode(SignIn::Device);
    g_stop = false;
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_thread = std::thread(threadMain);
}

void stopSignIn() {
    if (!g_thread.joinable()) return;
    g_stop = true;
    if (g_wake) SetEvent(g_wake);
    // a request in flight finishes within its 20-second timeout; the app does not wait that long to quit
    for (int i = 0; i < 30 && g_thread.joinable(); ++i) {
        if (WaitForSingleObject(g_thread.native_handle(), 100) == WAIT_OBJECT_0) { g_thread.join(); break; }
    }
    if (g_thread.joinable()) g_thread.detach();
}

void wakeSignIn() {
    if (g_wake) SetEvent(g_wake);
}

void signInTick() {
    std::wstring stale = homeFile(L"token.stale");
    if (!fileExists(stale)) return;
    ULONGLONG now = GetTickCount64();
    if (g_lastForced && now - g_lastForced < kStaleGapMs) return;     // left in place: acted on once the minute is up
    g_lastForced = now;
    DeleteFileW(stale.c_str());
    logLine(L"sign-in: the work process says its pass was refused; renewing it now");
    g_force = true;
    wakeSignIn();
}

SignInState signInState() {
    std::lock_guard<std::mutex> lock(g_state);
    return {g_mode, g_problem, g_generation};
}

bool fleetCredential(std::wstring& headerLine) {
    headerLine.clear();
    if (signInState().mode == SignIn::Legacy) {
        std::optional<std::string> key = loadSecret(L"partner");
        if (!key) return false;
        headerLine = L"X-Cobalt-Contributor: " + widen(*key) + L"\r\n";
        SecureZeroMemory(key->data(), key->size());
        return true;
    }
    HeldToken t = readTokenFile();
    bool good = !t.token.empty() && t.expires - serverNow() > 60;
    if (good) headerLine = L"X-Cobalt-Device: " + widen(t.token) + L"\r\n";
    SecureZeroMemory(t.token.data(), t.token.size());
    return good;
}

int enrolForKeyCheck(const std::string& key, const std::string& relay, std::wstring& message) {
    std::string machine = machineId();
    Json dev = deviceRecord();
    DeviceKey k;
    std::wstring err;
    if (deviceKey(machine, dev, k, err) != Made::Ready) {
        logLine(L"key check: no device key could be made (" + err + L"); checking the key the old way");
        return 3;
    }
    Outcome o = callEnrol(relay, key, machine, k);
    switch (o.got) {
    case Got::Ok: {
        if (!keepEnrolment(o, k, machine, relay, key)) {
            message = L"Your partner key was accepted, but this machine's sign-in could not be saved. Cobalt Connector "
                      L"registers again when it starts.";
            saveDevice(Json::object());
            return 0;
        }
        std::wstring owner = widen(o.reply.str("owner"));
        message = L"Your partner key was accepted";
        if (!owner.empty()) message += L": this machine joins the fleet under " + owner + L"'s name";
        message += L". It now signs in with a key of its own, kept ";
        message += k.provider == "tpm" ? L"in this PC's security chip (TPM)" : L"in Windows' key store for your user";
        message += L", so your partner key is not sent again.";
        return 0;
    }
    case Got::NoRoute:
    case Got::BadSignature:
        return 3;                            // an older server (or one that cannot check the signature): a hello, as before
    case Got::KeyRefused:
        message = L"The server refused that key" + (o.text.empty() ? L"." : L": " + o.text + L".") +
                  L" Contributed cores are for partners: check the key with whoever gave it to you.";
        return 1;
    case Got::DeviceRevoked:
        message = kRemovedText;
        return 1;
    case Got::TooMany:
        message = o.text.empty() ? std::wstring(L"Your partner key has too many machines registered. Ask Cobalt's owner "
                                                L"to remove one you no longer use.")
                                 : o.text;
        return 1;
    default:
        message = L"The server could not check the key (" + o.text + L").";
        return 2;
    }
}

int retireDevice(std::wstring& message) {
    Json cfg = readJsonFile(homeFile(L"contributor.json"));
    std::string machine = cfg.str("machine");
    Json dev = deviceRecord();
    std::string device = dev.str("id");
    bool told = false;
    if (!device.empty() && !dev.flag("revoked") && !machine.empty()) {
        std::string relay = dev.str("relay", relayUrl());
        if (!secureRelay(relay)) relay = relayUrl();
        HeldToken t = readTokenFile();
        if (t.token.empty() || t.device != device || t.expires - serverNow() < 60) {
            DeviceKey k;
            std::wstring err;
            if (openDeviceKey(machine, dev.str("provider", "tpm"), k, err) == KeyState::Ok) {
                Outcome o = callToken(relay, device, machine, k);
                if (o.got == Got::Ok) t.token = o.reply.str("token");
            }
        }
        if (!t.token.empty()) {
            std::wstring headers = L"X-Cobalt-Device: " + widen(t.token) + L"\r\n";
            SecureZeroMemory(t.token.data(), t.token.size());
            ServerReply r;
            told = serverRequest(relay + "/api/fleet/device/retire", L"POST", headers, "", r, 5000) && r.status == 200;
            logLine(L"uninstall: device " + widen(device) + (told ? L" retired on the server" : L" could not be retired "
                                                                                                L"on the server (it is "
                                                                                                L"retired after 90 idle days)"));
        }
    }
    bool deleted = !machine.empty() && deleteDeviceKey(machine);
    deleteTokenFile();
    saveDevice(Json::object());
    message = std::wstring(L"This machine's sign-in is removed") + (told ? L" from the Cobalt server" : L"") +
              (deleted ? L" and its key deleted from Windows' key store." : L".");
    return 0;
}

}  // namespace cw
