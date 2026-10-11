// One WinHTTP request to the Cobalt server, shared by every call the app makes there itself: the installer's key
// check and the enrol (check_key.cpp, devsign.cpp), the token renewal and the uninstall's retire (devsign.cpp), and
// the update check (update.cpp). Every one of them carries a credential (the partner key once, at enrol; a pass
// otherwise), so a redirect is never followed: WinHTTP would resend the headers to the address a 3xx names. If
// redirects cannot be turned off, nothing is sent. Windows' own TLS validation applies (no pinning), and a server
// address that is not https (secureRelay) never gets this far. The app's own downloads from GitHub (update.cpp)
// carry no credential and follow redirects by hand instead.
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

std::wstring customHeader(HINTERNET req, const wchar_t* name) {
    DWORD size = 0;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_CUSTOM, name, WINHTTP_NO_OUTPUT_BUFFER, &size, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0 || size > 4096) return {};
    std::wstring v(size / sizeof(wchar_t) + 1, L'\0');
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_CUSTOM, name, v.data(), &size, WINHTTP_NO_HEADER_INDEX)) return {};
    v.resize(size / sizeof(wchar_t));
    return v;
}

}  // namespace

std::wstring winHttpError(DWORD e) {
    wchar_t buf[512] = {};
    HMODULE mod = GetModuleHandleW(L"winhttp.dll");
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             mod, e, 0, buf, static_cast<DWORD>(std::size(buf)), nullptr);
    std::wstring s = n ? std::wstring(buf, n) : L"";
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ' || s.back() == L'.')) s.pop_back();
    return s.empty() ? L"error " + std::to_wstring(e) : s + L" (" + std::to_wstring(e) + L")";
}

bool serverRequest(const std::string& url, const wchar_t* method, std::wstring& headers, const std::string& body,
                   ServerReply& out, DWORD timeoutMs) {
    out = ServerReply{};
    auto fail = [&](const std::wstring& what) {
        out.error = what;
        SecureZeroMemory(headers.data(), headers.size() * sizeof(wchar_t));   // it may hold a key or a pass
        return false;
    };
    std::wstring wurl = widen(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof uc;
    uc.dwHostNameLength = static_cast<DWORD>(-1);
    uc.dwUrlPathLength = static_cast<DWORD>(-1);
    uc.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (wurl.empty() || !WinHttpCrackUrl(wurl.c_str(), static_cast<DWORD>(wurl.size()), 0, &uc) || !uc.lpszHostName ||
        uc.dwHostNameLength == 0)
        return fail(L"the server address is not a web address");
    std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path(uc.lpszUrlPath ? uc.lpszUrlPath : L"", uc.lpszUrlPath ? uc.dwUrlPathLength : 0);
    if (uc.lpszExtraInfo && uc.dwExtraInfoLength) path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (path.empty()) path = L"/";

    Handle session(WinHttpOpen(L"CobaltConnector/" COBALT_CONNECTOR_VERSION_W, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) return fail(L"the network could not be opened: " + winHttpError(GetLastError()));
    int t = static_cast<int>(timeoutMs);
    WinHttpSetTimeouts(session.h, t / 2, t, t, t);
    Handle conn(WinHttpConnect(session.h, host.c_str(), uc.nPort, 0));
    if (!conn) return fail(L"could not reach the server: " + winHttpError(GetLastError()));
    Handle req(WinHttpOpenRequest(conn.h, method, path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                  uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));
    if (!req) return fail(L"could not reach the server: " + winHttpError(GetLastError()));
    DWORD noRedirects = WINHTTP_DISABLE_REDIRECTS;
    if (!WinHttpSetOption(req.h, WINHTTP_OPTION_DISABLE_FEATURE, &noRedirects, sizeof noRedirects))
        return fail(L"redirects could not be turned off (" + winHttpError(GetLastError()) + L"), so nothing was sent");

    BOOL sent = WinHttpSendRequest(req.h, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                   static_cast<DWORD>(headers.size()),
                                   body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                                   static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
    SecureZeroMemory(headers.data(), headers.size() * sizeof(wchar_t));
    if (!sent || !WinHttpReceiveResponse(req.h, nullptr))
        return fail(L"could not reach the server: " + winHttpError(GetLastError()));
    DWORD status = 0, size = sizeof status;
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    out.status = status;
    out.reason = narrow(customHeader(req.h, L"X-Cobalt-Reason"));
    std::wstring retry = customHeader(req.h, L"Retry-After");
    out.retryAfter = retry.empty() ? 0 : _wtoi(retry.c_str());
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req.h, &avail) || avail == 0 || out.body.size() > 65536) break;
        std::vector<char> buf(avail);
        DWORD got = 0;
        if (!WinHttpReadData(req.h, buf.data(), avail, &got) || got == 0) break;
        out.body.append(buf.data(), got);
    }
    Json j;
    if (parseJson(out.body, j) && j.isObject()) {
        out.json = j;
        if (out.reason.empty()) out.reason = j.str("reason");
    }
    return true;
}

std::string relayAudience(const std::string& relay) {
    std::wstring w = widen(normalRelay(relay));
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof uc;
    uc.dwHostNameLength = static_cast<DWORD>(-1);
    if (w.empty() || !WinHttpCrackUrl(w.c_str(), static_cast<DWORD>(w.size()), 0, &uc) || !uc.lpszHostName) return {};
    std::string host = narrow(std::wstring(uc.lpszHostName, uc.dwHostNameLength));
    for (char& c : host)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;
    if ((https && uc.nPort != INTERNET_DEFAULT_HTTPS_PORT) || (!https && uc.nPort != INTERNET_DEFAULT_HTTP_PORT))
        host += ":" + std::to_string(uc.nPort);
    return host;
}

}  // namespace cw
