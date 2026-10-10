// The partner key and the optional Anthropic key, encrypted with DPAPI (CryptProtectData) for the
// current Windows user, in %LOCALAPPDATA%\CobaltConnector\<name>.key. A key's value is never logged.
#include "app.hpp"

#include <dpapi.h>

#include <vector>

namespace cw {

static std::wstring secretPath(const wchar_t* name) { return homeDir() + L"\\" + name + L".key"; }

// Part of how every stored key is sealed, from the app's first version (when it was called Cobalt Worker).
// It must never change: a key sealed with one value cannot be opened with another, so renaming it would
// make every partner.key and anthropic.key already on a machine unreadable.
static constexpr char kLegacyKeyEntropy[] = "CobaltWorker/";

static DATA_BLOB entropyFor(const wchar_t* name, std::string& holder) {
    holder = kLegacyKeyEntropy + narrow(name);
    DATA_BLOB e{};
    e.cbData = static_cast<DWORD>(holder.size());
    e.pbData = reinterpret_cast<BYTE*>(holder.data());
    return e;
}

bool saveSecret(const wchar_t* name, const std::string& value) {
    std::string holder;
    DATA_BLOB entropy = entropyFor(name, holder);
    DATA_BLOB in{};
    std::string copy = value;
    in.cbData = static_cast<DWORD>(copy.size());
    in.pbData = reinterpret_cast<BYTE*>(copy.data());
    DATA_BLOB out{};
    BOOL ok = CryptProtectData(&in, L"Cobalt Connector key", &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out);
    SecureZeroMemory(copy.data(), copy.size());
    if (!ok) return false;
    std::string blob(reinterpret_cast<const char*>(out.pbData), out.cbData);
    LocalFree(out.pbData);
    return writeFileAtomic(secretPath(name), blob);
}

std::optional<std::string> loadSecret(const wchar_t* name) {
    std::string blob;
    if (!readFile(secretPath(name), blob, 1u << 16) || blob.empty()) return std::nullopt;
    std::string holder;
    DATA_BLOB entropy = entropyFor(name, holder);
    DATA_BLOB in{};
    in.cbData = static_cast<DWORD>(blob.size());
    in.pbData = reinterpret_cast<BYTE*>(blob.data());
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return std::nullopt;
    std::string value(reinterpret_cast<const char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    value = trim(value);
    if (value.empty()) return std::nullopt;
    return value;
}

bool deleteSecret(const wchar_t* name) {
    std::wstring p = secretPath(name);
    return DeleteFileW(p.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND;
}

// The installer writes `partner=...`, `anthropic=...` and optionally `relay=...` into a file in its own
// temporary folder (so no key is ever on a command line) and calls --set-keys <file>. The file is
// overwritten and deleted once read. An empty `anthropic=` removes a stored Anthropic key. `partner=` may be
// left out when a partner key is already stored (a reinstall that only changes the Anthropic key).
int setKeysFromFile(const std::wstring& file, std::wstring& message) {
    std::string text;
    if (!readFile(file, text, 1u << 16)) {
        message = L"The keys file could not be read.";
        return 1;
    }
    {   // wipe it before anything else can fail
        std::string zeros(text.size(), '\0');
        writeFileAtomic(file, zeros);
        DeleteFileW(file.c_str());
    }
    std::string partner, anthropic, relay;
    bool sawAnthropic = false;
    size_t pos = 0;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF) pos = 3;   // a byte order mark
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = trim(text.substr(pos, end - pos));
        pos = end + 1;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (k == "partner") partner = v;
        else if (k == "anthropic") { anthropic = v; sawAnthropic = true; }
        else if (k == "relay") relay = v;
    }
    SecureZeroMemory(text.data(), text.size());
    bool ok = true;
    if (partner.empty()) {
        std::optional<std::string> stored = loadSecret(L"partner");
        if (stored) SecureZeroMemory(stored->data(), stored->size());
        if (!stored) {
            message = L"No partner key was given: the app does not run without one.";
            return 1;
        }
    } else {
        ok = saveSecret(L"partner", partner);
        SecureZeroMemory(partner.data(), partner.size());
    }
    if (sawAnthropic) {
        ok = (anthropic.empty() ? deleteSecret(L"anthropic") : saveSecret(L"anthropic", anthropic)) && ok;
        SecureZeroMemory(anthropic.data(), anthropic.size());
    }
    if (!relay.empty()) {
        std::wstring cfgPath = homeFile(L"contributor.json");
        Json cfg = readJsonFile(cfgPath);
        while (!relay.empty() && relay.back() == '/') relay.pop_back();
        cfg.set("relay", Json::string(relay));
        writeJsonFile(cfgPath, cfg);
    }
    machineId();     // made now, so the key check and the loop use the same id
    logLine(ok ? L"keys stored (DPAPI, this Windows user)" : L"the keys could not be stored");
    message = ok ? L"The keys are stored, encrypted for this Windows user." : L"The keys could not be stored.";
    return ok ? 0 : 1;
}

}  // namespace cw
