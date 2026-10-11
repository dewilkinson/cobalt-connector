// --check-key: the installer's partner-key check. One POST to the fleet server's /api/fleet/hello
// with the key in X-Cobalt-Contributor, exactly as the bootstrap and the pull loop say hello (with the app's
// version and the machine's chosen name, when one is set). Exit 0: accepted. 1: refused (or not a key at
// all). 2: could not check (no network, or the server does not answer the fleet route), which the installer
// offers to continue past offline. A server address that is not https (secureRelay) is refused (exit 1)
// before anything is sent.
#include "app.hpp"

#include <winhttp.h>

#include <vector>

namespace cw {

namespace {

struct Handle {
    HINTERNET h = nullptr;
    explicit Handle(HINTERNET v) : h(v) {}
    ~Handle() { if (h) WinHttpCloseHandle(h); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    explicit operator bool() const { return h != nullptr; }
};

std::string jsonEscape(const std::string& s) {
    Json j = Json::string(s);
    return j.dump(0);
}

std::wstring winError(DWORD e) {
    wchar_t buf[512] = {};
    HMODULE mod = GetModuleHandleW(L"winhttp.dll");
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             mod, e, 0, buf, static_cast<DWORD>(std::size(buf)), nullptr);
    std::wstring s = n ? std::wstring(buf, n) : L"";
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ' || s.back() == L'.')) s.pop_back();
    return s.empty() ? L"error " + std::to_wstring(e) : s + L" (" + std::to_wstring(e) + L")";
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

    std::wstring url = widen(relay) + L"/api/fleet/hello";
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof uc;
    wchar_t host[256] = {}, path[1024] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = static_cast<DWORD>(std::size(host));
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
        message = L"The server address " + widen(relay) + L" is not a web address.";
        return 2;
    }

    Handle session(WinHttpOpen(L"CobaltConnector/" COBALT_CONNECTOR_VERSION_W, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        message = L"The network could not be opened: " + winError(GetLastError());
        return 2;
    }
    WinHttpSetTimeouts(session.h, 10000, 15000, 20000, 20000);
    Handle conn(WinHttpConnect(session.h, host, uc.nPort, 0));
    if (!conn) {
        message = L"Could not reach " + widen(relay) + L": " + winError(GetLastError());
        return 2;
    }
    DWORD flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    Handle req(WinHttpOpenRequest(conn.h, L"POST", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!req) {
        message = L"Could not reach " + widen(relay) + L": " + winError(GetLastError());
        return 2;
    }
    // the request carries the partner key: never follow a redirect (WinHTTP would resend the header to its address);
    // a 3xx is answered below as any other status the check does not expect
    DWORD noRedirects = WINHTTP_DISABLE_REDIRECTS;
    if (!WinHttpSetOption(req.h, WINHTTP_OPTION_DISABLE_FEATURE, &noRedirects, sizeof noRedirects)) {
        message = L"The key check could not turn off redirects: " + winError(GetLastError());
        return 2;                    // the key is never sent on a request that might follow a redirect
    }

    std::string name = machineName();
    std::string body = "{\"machine\":" + jsonEscape(machineId()) + ",\"host\":" + jsonEscape(narrow(computerName())) +
                       ",\"cores\":" + std::to_string(cpuCount()) + ",\"version\":\"app-check\",\"code\":\"\",\"paused\":true" +
                       ",\"app_version\":\"" COBALT_CONNECTOR_VERSION "\"" +
                       ",\"name\":" + jsonEscape(validMachineName(name) ? name : std::string()) + "}";
    std::wstring headers = L"Content-Type: application/json\r\nX-Cobalt-Contributor: " + widen(key) + L"\r\n";
    BOOL sent = WinHttpSendRequest(req.h, headers.c_str(), static_cast<DWORD>(headers.size()), body.data(),
                                   static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
    SecureZeroMemory(headers.data(), headers.size() * sizeof(wchar_t));
    SecureZeroMemory(key.data(), key.size());
    if (!sent || !WinHttpReceiveResponse(req.h, nullptr)) {
        message = L"Could not reach " + widen(relay) + L": " + winError(GetLastError());
        return 2;
    }
    DWORD status = 0, size = sizeof status;
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    std::string reply;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req.h, &avail) || avail == 0 || reply.size() > 65536) break;
        std::vector<char> buf(avail);
        DWORD got = 0;
        if (!WinHttpReadData(req.h, buf.data(), avail, &got) || got == 0) break;
        reply.append(buf.data(), got);
    }
    Json j;
    bool parsed = parseJson(reply, j) && j.isObject();
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
