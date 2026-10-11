// The machine's own device key (Cobalt Connector 1.3.0): an ECDSA P-256 key pair made once per installation by
// Windows' CNG, named "CobaltConnector-device-<machine id>", for the current Windows user only.
//
// - Where it lives: the PC's security chip (the Microsoft Platform Crypto Provider, i.e. the TPM) when it has one that
//   can make the key; otherwise Windows' software key store (the Microsoft Software Key Storage Provider), which
//   protects it with DPAPI for this user. COBALT_CONNECTOR_KEY_STORE=software skips the TPM (a test, or a TPM that
//   misbehaves).
// - It cannot be copied out: no export policy is set (the software store's default is "not exportable"; a TPM key
//   never leaves the chip), and it is usable only for signing. Every call passes NCRYPT_SILENT_FLAG and no UI policy
//   is set, so nothing ever prompts.
// - Only the public half leaves the machine: the 65-byte uncompressed point 0x04 || X || Y, base64url. A signature is
//   the 64-byte r || s that NCryptSignHash gives over the SHA-256 of the text, base64url; the server checks it with
//   Python's cryptography library (--sign-test-vector writes vectors its tests check).
// Nothing here logs a signature, a nonce or a key's private half (which the app never sees).
#include "app.hpp"

#include <bcrypt.h>
#include <ncrypt.h>

#include <ctime>
#include <vector>

namespace cw {

namespace {

constexpr wchar_t kKeyPrefix[] = L"CobaltConnector-device-";

struct Prov {
    NCRYPT_PROV_HANDLE h = 0;
    Prov() = default;
    ~Prov() { if (h) NCryptFreeObject(h); }
    Prov(const Prov&) = delete;
    Prov& operator=(const Prov&) = delete;
};

struct Key {
    NCRYPT_KEY_HANDLE h = 0;
    Key() = default;
    ~Key() { if (h) NCryptFreeObject(h); }
    Key(const Key&) = delete;
    Key& operator=(const Key&) = delete;
};

const wchar_t* providerName(const std::string& provider) {
    return provider == "tpm" ? MS_PLATFORM_CRYPTO_PROVIDER : MS_KEY_STORAGE_PROVIDER;
}

std::wstring hr(SECURITY_STATUS s) {
    wchar_t buf[16];
    swprintf_s(buf, L"0x%08lX", static_cast<unsigned long>(s));
    return buf;
}

bool softwareOnly() {
    wchar_t buf[32];
    DWORD n = GetEnvironmentVariableW(L"COBALT_CONNECTOR_KEY_STORE", buf, static_cast<DWORD>(std::size(buf)));
    return n > 0 && n < std::size(buf) && _wcsicmp(buf, L"software") == 0;
}

bool sha256(const std::string& text, unsigned char out[32]) {
    return BCRYPT_SUCCESS(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
                                     reinterpret_cast<PUCHAR>(const_cast<char*>(text.data())),
                                     static_cast<ULONG>(text.size()), out, 32));
}

// The public half as the server takes it: 0x04 || X || Y (from the BCRYPT_ECCPUBLIC_BLOB CNG exports).
bool exportPublic(NCRYPT_KEY_HANDLE key, std::string& raw, std::wstring& err) {
    DWORD size = 0;
    SECURITY_STATUS s = NCryptExportKey(key, 0, BCRYPT_ECCPUBLIC_BLOB, nullptr, nullptr, 0, &size, 0);
    if (s != ERROR_SUCCESS) { err = L"the device key's public half could not be read (" + hr(s) + L")"; return false; }
    std::vector<unsigned char> blob(size);
    s = NCryptExportKey(key, 0, BCRYPT_ECCPUBLIC_BLOB, nullptr, blob.data(), size, &size, 0);
    if (s != ERROR_SUCCESS || size < sizeof(BCRYPT_ECCKEY_BLOB)) {
        err = L"the device key's public half could not be read (" + hr(s) + L")";
        return false;
    }
    const auto* head = reinterpret_cast<const BCRYPT_ECCKEY_BLOB*>(blob.data());
    if (head->dwMagic != BCRYPT_ECDSA_PUBLIC_P256_MAGIC || head->cbKey != 32 || size < sizeof *head + 64) {
        err = L"the device key is not a P-256 key";
        return false;
    }
    raw.assign(1, '\x04');
    raw.append(reinterpret_cast<const char*>(blob.data() + sizeof *head), 64);
    return true;
}

bool signHash(NCRYPT_KEY_HANDLE key, const std::string& text, std::string& sig, std::wstring& err) {
    unsigned char digest[32];
    if (!sha256(text, digest)) { err = L"SHA-256 is not available"; return false; }
    unsigned char out[64];
    DWORD got = 0;
    SECURITY_STATUS s = NCryptSignHash(key, nullptr, digest, sizeof digest, out, sizeof out, &got, NCRYPT_SILENT_FLAG);
    SecureZeroMemory(digest, sizeof digest);
    if (s != ERROR_SUCCESS || got != 64) { err = L"the device key could not sign (" + hr(s) + L")"; return false; }
    sig.assign(reinterpret_cast<const char*>(out), 64);
    return true;
}

bool openIn(const std::string& provider, const std::wstring& name, Key& key) {
    Prov prov;
    if (NCryptOpenStorageProvider(&prov.h, providerName(provider), 0) != ERROR_SUCCESS) return false;
    return NCryptOpenKey(prov.h, &key.h, name.c_str(), 0, NCRYPT_SILENT_FLAG) == ERROR_SUCCESS;
}

// Flags 0, not NCRYPT_SILENT_FLAG: the TPM's provider refuses that flag on a delete (NTE_BAD_FLAGS) and keeps the key.
// Nothing can prompt here anyway: these keys have no UI protection policy.
bool deleteIn(const std::string& provider, const std::wstring& name) {
    Key key;
    if (!openIn(provider, name, key)) return false;
    if (NCryptDeleteKey(key.h, 0) != ERROR_SUCCESS) return false;
    key.h = 0;                            // NCryptDeleteKey frees the handle when it succeeds
    return true;
}

// Makes a persisted ECDSA P-256 key in one provider: signing only, not exportable, no UI, this Windows user.
bool createIn(const std::string& provider, const std::wstring& name, bool overwrite, Key& key, std::wstring& err) {
    Prov prov;
    SECURITY_STATUS s = NCryptOpenStorageProvider(&prov.h, providerName(provider), 0);
    if (s != ERROR_SUCCESS) { err = L"the " + widen(provider) + L" key store is not available (" + hr(s) + L")"; return false; }
    s = NCryptCreatePersistedKey(prov.h, &key.h, BCRYPT_ECDSA_P256_ALGORITHM, name.c_str(), 0,
                                 overwrite ? NCRYPT_OVERWRITE_KEY_FLAG : 0);
    if (s != ERROR_SUCCESS) { err = L"the " + widen(provider) + L" key store could not make a key (" + hr(s) + L")"; return false; }
    DWORD usage = NCRYPT_ALLOW_SIGNING_FLAG;
    NCryptSetProperty(key.h, NCRYPT_KEY_USAGE_PROPERTY, reinterpret_cast<PBYTE>(&usage), sizeof usage, NCRYPT_SILENT_FLAG);
    if (provider == "software") {
        DWORD exportPolicy = 0;           // not exportable, in plain text or wrapped (the default, said explicitly)
        s = NCryptSetProperty(key.h, NCRYPT_EXPORT_POLICY_PROPERTY, reinterpret_cast<PBYTE>(&exportPolicy),
                              sizeof exportPolicy, NCRYPT_PERSIST_FLAG | NCRYPT_SILENT_FLAG);
        if (s != ERROR_SUCCESS) {
            NCryptFreeObject(key.h);
            key.h = 0;
            err = L"the key's export policy could not be set (" + hr(s) + L")";
            return false;
        }
    }
    s = NCryptFinalizeKey(key.h, NCRYPT_SILENT_FLAG);
    if (s != ERROR_SUCCESS) {
        NCryptFreeObject(key.h);
        key.h = 0;
        deleteIn(provider, name);         // a half-made key is removed, so a later try starts clean
        err = L"the " + widen(provider) + L" key store could not finish the key (" + hr(s) + L")";
        return false;
    }
    return true;
}

std::wstring keyName(const std::string& machine) { return kKeyPrefix + widen(machine); }

std::wstring throwawayName(const wchar_t* what) {
    unsigned char rnd[6] = {};
    BCryptGenRandom(nullptr, rnd, sizeof rnd, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    wchar_t hex[16];
    swprintf_s(hex, L"%02x%02x%02x%02x%02x%02x", rnd[0], rnd[1], rnd[2], rnd[3], rnd[4], rnd[5]);
    return std::wstring(L"CobaltConnector-") + what + L"-" + hex;
}

// Checks a signature with BCrypt and the public half alone, as a stranger would.
bool verifyWithPublic(const std::string& raw, const std::string& text, const std::string& sig) {
    if (raw.size() != 65 || sig.size() != 64) return false;
    std::vector<unsigned char> blob(sizeof(BCRYPT_ECCKEY_BLOB) + 64);
    auto* head = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
    head->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    head->cbKey = 32;
    memcpy(blob.data() + sizeof *head, raw.data() + 1, 64);
    BCRYPT_KEY_HANDLE k = nullptr;
    if (!BCRYPT_SUCCESS(BCryptImportKeyPair(BCRYPT_ECDSA_P256_ALG_HANDLE, nullptr, BCRYPT_ECCPUBLIC_BLOB, &k, blob.data(),
                                            static_cast<ULONG>(blob.size()), 0)))
        return false;
    unsigned char digest[32];
    bool ok = sha256(text, digest) &&
              BCRYPT_SUCCESS(BCryptVerifySignature(k, nullptr, digest, sizeof digest,
                                                   reinterpret_cast<PUCHAR>(const_cast<char*>(sig.data())), 64, 0));
    BCryptDestroyKey(k);
    return ok;
}

}  // namespace

std::string base64url(const std::string& bytes) {
    static const char a[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    size_t i = 0;
    const auto* p = reinterpret_cast<const unsigned char*>(bytes.data());
    for (; i + 2 < bytes.size(); i += 3) {
        unsigned v = (p[i] << 16) | (p[i + 1] << 8) | p[i + 2];
        out += a[(v >> 18) & 63]; out += a[(v >> 12) & 63]; out += a[(v >> 6) & 63]; out += a[v & 63];
    }
    if (i + 1 == bytes.size()) {
        unsigned v = p[i] << 16;
        out += a[(v >> 18) & 63]; out += a[(v >> 12) & 63];
    } else if (i + 2 == bytes.size()) {
        unsigned v = (p[i] << 16) | (p[i + 1] << 8);
        out += a[(v >> 18) & 63]; out += a[(v >> 12) & 63]; out += a[(v >> 6) & 63];
    }
    return out;
}

std::string randomNonce() {
    unsigned char rnd[16] = {};
    BCryptGenRandom(nullptr, rnd, sizeof rnd, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    std::string s = base64url(std::string(reinterpret_cast<const char*>(rnd), sizeof rnd));
    SecureZeroMemory(rnd, sizeof rnd);
    return s;
}

bool openDeviceKey(const std::string& machine, const std::string& provider, DeviceKey& out, std::wstring& err) {
    std::wstring name = keyName(machine);
    for (const std::string& p : {provider, std::string(provider == "tpm" ? "software" : "tpm")}) {
        if (p != "tpm" && p != "software") continue;
        Key key;
        if (!openIn(p, name, key)) continue;
        std::string raw;
        if (!exportPublic(key.h, raw, err)) return false;
        out.machine = machine;
        out.provider = p;
        out.publicKey = base64url(raw);
        return true;
    }
    err = L"this machine's device key is not in Windows' key store";
    return false;
}

bool createDeviceKey(const std::string& machine, DeviceKey& out, std::wstring& err) {
    std::wstring name = keyName(machine);
    std::wstring tpmErr;
    for (const char* p : {"tpm", "software"}) {
        if (std::string(p) == "tpm" && softwareOnly()) continue;
        Key key;
        std::wstring e;
        if (!createIn(p, name, true, key, e)) {
            if (std::string(p) == "tpm") tpmErr = e;
            else err = e;
            continue;
        }
        std::string raw;
        if (!exportPublic(key.h, raw, e)) { err = e; return false; }
        out.machine = machine;
        out.provider = p;
        out.publicKey = base64url(raw);
        if (std::string(p) == "software" && !tpmErr.empty()) logLine(L"device key: no TPM key (" + tpmErr + L"); using Windows' software key store");
        return true;
    }
    return false;
}

bool signWithDeviceKey(const DeviceKey& k, const std::string& text, std::string& sigB64u, std::wstring& err) {
    Key key;
    if (!openIn(k.provider, keyName(k.machine), key)) {
        err = L"this machine's device key could not be opened";
        return false;
    }
    std::string sig;
    if (!signHash(key.h, text, sig, err)) return false;
    sigB64u = base64url(sig);
    return true;
}

bool deleteDeviceKey(const std::string& machine) {
    std::wstring name = keyName(machine);
    bool a = deleteIn("tpm", name), b = deleteIn("software", name);
    return a || b;
}

// One provider's round trip with a throwaway key, through the same create and delete the app uses. "" when it passed.
static std::wstring selfTestIn(const std::string& provider) {
    std::wstring name = throwawayName(L"selftest");
    std::wstring err;
    std::string raw, sig;
    const std::string text = "cobalt-connector/self-test\nline two";
    bool exportable = false, ok = false;
    {
        Key key;
        if (!createIn(provider, name, false, key, err)) return L"could not make a key: " + err;
        ok = exportPublic(key.h, raw, err) && signHash(key.h, text, sig, err);
        // the key must refuse to leave in any form
        DWORD size = 0;
        if (NCryptExportKey(key.h, 0, BCRYPT_ECCPRIVATE_BLOB, nullptr, nullptr, 0, &size, 0) == ERROR_SUCCESS && size > 0) {
            std::vector<unsigned char> priv(size);
            exportable =
                NCryptExportKey(key.h, 0, BCRYPT_ECCPRIVATE_BLOB, nullptr, priv.data(), size, &size, 0) == ERROR_SUCCESS;
            SecureZeroMemory(priv.data(), priv.size());
        }
    }
    bool verified = ok && verifyWithPublic(raw, text, sig);
    bool tamperRefused = ok && !verifyWithPublic(raw, text + "x", sig);
    bool deleted = deleteIn(provider, name);
    Key gone;
    bool stillThere = openIn(provider, name, gone);
    if (!ok) return err;
    if (exportable) return L"the private key could be exported";
    if (!verified || !tamperRefused) return L"the signature did not check out";
    if (!deleted || stillThere) return L"the throwaway key was not deleted";
    return {};
}

int selfTestDeviceKey(std::wstring& message) {
    std::wstring soft = selfTestIn("software");
    if (!soft.empty()) { message = L"device key self-test failed (software key store): " + soft; return 1; }
    // the TPM is tested where there is one: a key it cannot make is the fallback's case, but a key it made and the app
    // could not delete again (or that signed wrongly) is a failure
    std::wstring tpm = selfTestIn("tpm");
    bool noTpm = tpm.rfind(L"could not make a key", 0) == 0;
    if (!tpm.empty() && !noTpm) { message = L"device key self-test failed (TPM): " + tpm; return 1; }
    message = L"device key self-test passed: in the software key store" +
              std::wstring(noTpm ? L" (no TPM key here: " + tpm + L")" : L" and in the TPM") +
              L", a non-exportable P-256 key signed, BCrypt verified the signature with the public half alone, refused "
              L"a changed text, and the throwaway key was deleted.";
    return 0;
}

int signTestVectors(const std::wstring& outFile, std::wstring& message) {
    Json list = Json::array();
    std::wstring notes;
    for (const char* p : {"software", "tpm"}) {
        std::wstring name = throwawayName(L"vector");
        Key key;
        std::wstring err;
        if (!createIn(p, name, false, key, err)) {
            notes += L" " + widen(p) + L": " + err + L".";
            continue;
        }
        std::string raw, sigE, sigT;
        bool ok = exportPublic(key.h, raw, err);
        std::string pub = base64url(raw), machine = "cc-1a2b3c4d5e6f", device = "dv_0123456789abcdef";
        long long ts = static_cast<long long>(std::time(nullptr));
        std::string n1 = randomNonce(), n2 = randomNonce();
        std::string enrolText = "cobalt-connector/enrol/v1\napi.bluesec.ai\n" + machine + "\n" + pub + "\n" +
                                std::to_string(ts) + "\n" + n1;
        std::string tokenText = "cobalt-connector/token/v1\n127.0.0.1:8000\n" + device + "\n" + machine + "\n" +
                                std::to_string(ts + 60) + "\n" + n2;
        ok = ok && signHash(key.h, enrolText, sigE, err) && signHash(key.h, tokenText, sigT, err);
        NCryptFreeObject(key.h);
        key.h = 0;
        if (!deleteIn(p, name)) {
            message = L"the throwaway " + widen(p) + L" key " + name + L" could not be deleted";
            return 1;
        }
        if (!ok) { notes += L" " + widen(p) + L": " + err + L"."; continue; }
        std::string source = std::string("Windows CNG, ") +
                             (std::string(p) == "tpm" ? "Microsoft Platform Crypto Provider (TPM)"
                                                      : "Microsoft Software Key Storage Provider") +
                             " (throwaway key, deleted; CobaltConnector.exe " COBALT_CONNECTOR_VERSION
                             " --sign-test-vector)";
        Json e = Json::object();
        e.set("kind", Json::string("enrol"));
        e.set("source", Json::string(source));
        e.set("audience", Json::string("api.bluesec.ai"));
        e.set("machine", Json::string(machine));
        e.set("public_key", Json::string(pub));
        e.set("ts", Json::number(static_cast<double>(ts)));
        e.set("nonce", Json::string(n1));
        e.set("canonical", Json::string(enrolText));
        e.set("signature", Json::string(base64url(sigE)));
        list.a.push_back(e);
        Json t = Json::object();
        t.set("kind", Json::string("token"));
        t.set("source", Json::string(source));
        t.set("audience", Json::string("127.0.0.1:8000"));
        t.set("device", Json::string(device));
        t.set("machine", Json::string(machine));
        t.set("public_key", Json::string(pub));
        t.set("ts", Json::number(static_cast<double>(ts + 60)));
        t.set("nonce", Json::string(n2));
        t.set("canonical", Json::string(tokenText));
        t.set("signature", Json::string(base64url(sigT)));
        list.a.push_back(t);
    }
    if (list.a.empty()) { message = L"no test vector could be signed:" + notes; return 1; }
    if (outFile.empty() || !writeFileAtomic(outFile, list.dump(2))) {
        message = L"the test vectors could not be written (pass --sign-test-vector <file>)";
        return 1;
    }
    message = L"wrote " + std::to_wstring(list.a.size()) + L" test vectors to " + outFile +
              (notes.empty() ? std::wstring() : L"; skipped:" + notes);
    return 0;
}

}  // namespace cw
