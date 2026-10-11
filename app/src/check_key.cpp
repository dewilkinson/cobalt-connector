// --check-key: the installer's partner-key check. From 1.3.0 it IS the enrol (token.cpp): the key goes to
// POST /api/fleet/enrol with the public half of the machine's new device key, the server makes the device and hands
// back its first pass, and the key is not sent again after setup. Only a server without device keys (it answers the
// enrol with 404) is asked the old way: one POST to /api/fleet/hello with the key in X-Cobalt-Contributor, exactly as
// the bootstrap says hello. Exit 0: accepted. 1: refused (or not a key at all). 2: could not check (no network, or
// the server does not answer the fleet route), which the installer offers to continue past offline. A server address
// that is not https (secureRelay) is refused (exit 1) before anything is sent. Neither request follows a redirect
// (net.cpp).
#include "app.hpp"

namespace cw {

namespace {

std::string jsonEscape(const std::string& s) {
    Json j = Json::string(s);
    return j.dump(0);
}

}  // namespace

int checkKey(const std::string& rawKey, const std::string& rawRelay, std::wstring& message) {
    std::string relay = normalRelay(rawRelay);
    if (!secureRelay(relay)) {       // the key would go in clear text: refused, never sent
        message = insecureRelayMessage(relay);
        return 1;
    }
    std::string key = trim(rawKey);
    bool printable = !key.empty();
    for (unsigned char c : key)
        if (c <= ' ' || c >= 0x7F || c == '"' || c == '\\') printable = false;
    if (!printable || key.size() < 16 || key.size() > 200) {
        message = L"That is not a partner key. A partner key is about 44 characters long; "
                  L"ask whoever runs your fleet for yours.";
        return 1;
    }

    int enrolled = enrolForKeyCheck(key, relay, message);
    if (enrolled != 3) {
        SecureZeroMemory(key.data(), key.size());
        return enrolled;
    }

    std::string name = machineName();
    std::string body = "{\"machine\":" + jsonEscape(machineId()) + ",\"host\":" + jsonEscape(narrow(computerName())) +
                       ",\"cores\":" + std::to_string(cpuCount()) + ",\"version\":\"app-check\",\"code\":\"\",\"paused\":true" +
                       ",\"app_version\":\"" COBALT_CONNECTOR_VERSION "\"" +
                       ",\"name\":" + jsonEscape(validMachineName(name) ? name : std::string()) + "}";
    std::wstring headers = L"Content-Type: application/json\r\nX-Cobalt-Contributor: " + widen(key) + L"\r\n";
    SecureZeroMemory(key.data(), key.size());
    ServerReply r;
    if (!serverRequest(relay + "/api/fleet/hello", L"POST", headers, body, r, 20000)) {   // wipes `headers`
        message = L"Could not reach " + widen(relay) + L": " + r.error;
        return 2;
    }
    DWORD status = r.status;
    const Json& j = r.json;
    bool parsed = j.isObject();
    std::wstring detail = parsed ? widen(j.str("detail")) : L"";

    if (status == 200) {
        std::wstring owner = parsed ? widen(j.str("owner")) : L"";
        std::wstring machine = parsed ? widen(j.str("machine")) : L"";
        message = L"Your partner key was accepted";
        if (!owner.empty()) message += L": this machine joins the fleet under " + owner + L"'s name";
        if (!machine.empty()) message += L" (" + machine + L")";
        message += L".";
        return 0;
    }
    if (status == 401 || status == 403) {
        message = L"The server refused that key";
        message += detail.empty() ? L"." : L": " + detail + L".";
        message += L" Contributed cores are for partners: check the key with whoever gave it to you.";
        return 1;
    }
    message = L"The server could not check the key (it answered " + std::to_wstring(status) + L")";
    message += detail.empty() ? L"." : L": " + detail + L".";
    return 2;
}

}  // namespace cw
