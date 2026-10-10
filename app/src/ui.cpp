// The tray icon (idle / working / paused / error) and the small window: the machine's limits, the owner's
// Anthropic key and the "run my model work on this machine" option, what the pull loop reports in status.json
// (refreshed every 2 s) and the tail of its log.
#include "app.hpp"

#include <commctrl.h>
#include <windowsx.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace cw {

namespace {

enum class TrayState { Idle, Working, Paused, Error };

enum : int {
    IDC_STATUS = 100, IDC_DETAIL, IDC_LIMITS_GROUP, IDC_CORES_LBL, IDC_CORES, IDC_CORES_OF, IDC_MEM_LBL, IDC_MEM,
    IDC_MEM_OF, IDC_HOURS_LBL, IDC_HOURS, IDC_HOURS_HINT, IDC_SAVE, IDC_PAUSE, IDC_LOGON, IDC_JOBS_LBL, IDC_JOBS,
    IDC_LOG_LBL, IDC_LOG, IDC_VIEWLOG, IDC_HIDE, IDC_MODEL_GROUP, IDC_LOCAL, IDC_KEY_STATE, IDC_KEY_EDIT, IDC_KEY_SAVE,
    IDC_KEY_CLEAR,
};
enum : UINT { CMD_OPEN = 1, CMD_PAUSE, CMD_LOGON, CMD_VIEWLOG, CMD_QUIT };
enum : UINT_PTR { TIMER_SUPERVISE = 1, TIMER_STATUS = 2 };

struct App {
    HINSTANCE inst = nullptr;
    HWND wnd = nullptr;
    UINT taskbarCreated = 0;
    UINT dpi = 96;
    HFONT font = nullptr, bold = nullptr, mono = nullptr;
    HICON icons[4] = {};
    HICON appIcon = nullptr, appIconSmall = nullptr;
    TrayState state = TrayState::Idle;
    bool trayAdded = false;
    std::wstring headline, detail, jobsText, logText, tip;
    Supervisor sup;
} g;

int S(int v) { return MulDiv(v, static_cast<int>(g.dpi), 96); }

HWND ctl(int id) { return GetDlgItem(g.wnd, id); }

void setText(int id, const std::wstring& text) {
    HWND h = ctl(id);
    if (!h) return;
    int n = GetWindowTextLengthW(h);
    std::wstring cur(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(h, cur.data(), n + 1);
    cur.resize(static_cast<size_t>(n));
    if (cur != text) SetWindowTextW(h, text.c_str());
}

std::wstring getText(int id) {
    HWND h = ctl(id);
    int n = GetWindowTextLengthW(h);
    std::wstring s(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(h, s.data(), n + 1);
    s.resize(static_cast<size_t>(n));
    return s;
}

std::wstring crlf(const std::string& utf8) {
    std::wstring w = widen(utf8), out;
    out.reserve(w.size() + w.size() / 20);
    for (size_t i = 0; i < w.size(); ++i) {
        if (w[i] == L'\n' && (i == 0 || w[i - 1] != L'\r')) out += L'\r';
        out += w[i];
    }
    return out;
}

std::wstring fmtNum(double v, int decimals = 0) {
    wchar_t buf[64];
    swprintf_s(buf, L"%.*f", decimals, v);
    return buf;
}

// ---------------------------------------------------------------- icons drawn at runtime

HICON makeIcon(TrayState st, int size) {
    COLORREF colors[] = {RGB(37, 99, 235), RGB(22, 163, 74), RGB(217, 119, 6), RGB(220, 38, 38)};
    COLORREF c = colors[static_cast<int>(st)];
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof bi;
    bi.bV5Width = size;
    bi.bV5Height = -size;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color || !bits) return nullptr;
    auto* px = static_cast<DWORD*>(bits);
    const double cx = (size - 1) / 2.0, r = size * 0.46, ring = std::max(1.0, size / 12.0);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            double d = std::hypot(x - cx, y - cx);
            double cover = std::clamp(r + 0.5 - d, 0.0, 1.0);
            BYTE red = GetRValue(c), grn = GetGValue(c), blu = GetBValue(c);
            if (d > r - ring) { red = static_cast<BYTE>(red * 0.7); grn = static_cast<BYTE>(grn * 0.7); blu = static_cast<BYTE>(blu * 0.7); }
            // the state's glyph, in white
            double u = (x - cx) / r, v = (y - cx) / r;
            bool white = false;
            switch (st) {
            case TrayState::Working: white = std::hypot(u, v) < 0.38; break;
            case TrayState::Paused: white = std::fabs(v) < 0.45 && ((u > -0.42 && u < -0.12) || (u > 0.12 && u < 0.42)); break;
            case TrayState::Error: white = std::fabs(u) < 0.14 && ((v > -0.55 && v < 0.15) || (v > 0.3 && v < 0.55)); break;
            case TrayState::Idle: white = std::fabs(std::hypot(u, v) - 0.42) < 0.12; break;
            }
            if (white && cover >= 1.0) { red = grn = blu = 255; }
            BYTE a = static_cast<BYTE>(std::lround(cover * 255.0));
            px[y * size + x] = (static_cast<DWORD>(a) << 24) | (static_cast<DWORD>(red) << 16) | (static_cast<DWORD>(grn) << 8) | blu;
        }
    }
    std::vector<BYTE> zeros(static_cast<size_t>(((size + 15) / 16) * 2) * static_cast<size_t>(size), 0);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, zeros.data());
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

void makeIcons() {
    for (HICON& i : g.icons) if (i) DestroyIcon(i);
    int size = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem());
    for (int s = 0; s < 4; ++s) g.icons[s] = makeIcon(static_cast<TrayState>(s), size);
}

// ---------------------------------------------------------------- the tray icon

void trayUpdate(bool add) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof nid;
    nid.hWnd = g.wnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = g.icons[static_cast<int>(g.state)];
    wcsncpy_s(nid.szTip, g.tip.c_str(), _TRUNCATE);
    if (add) {
        g.trayAdded = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
        nid.uVersion = NOTIFYICON_VERSION_4;
        if (g.trayAdded) Shell_NotifyIconW(NIM_SETVERSION, &nid);
    } else if (g.trayAdded) {
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    } else {
        trayUpdate(true);
    }
}

void trayRemove() {
    if (!g.trayAdded) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof nid;
    nid.hWnd = g.wnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g.trayAdded = false;
}

// ---------------------------------------------------------------- what the loop reports

void readStatus() {
    std::wstring statusPath = homeFile(L"status.json");
    Json st = readJsonFile(statusPath);
    double age = fileAgeSeconds(statusPath);
    bool paused = limitsPaused();
    double ran = g.sup.runningSeconds();
    bool fresh = g.sup.running() && age < 30.0 && age <= ran + 1.0;
    std::string s = st.str("state");
    int inFlight = static_cast<int>(st.num("in_flight"));

    TrayState state = TrayState::Idle;
    std::wstring headline, detail;
    if (!g.sup.problem().empty()) {
        state = TrayState::Error;
        headline = L"Not running";
        detail = g.sup.problem();
    } else if (!g.sup.running()) {
        ULONGLONG now = GetTickCount64(), next = g.sup.nextStartTick();
        std::wstring in = next > now ? L" Starting again in " + std::to_wstring((next - now + 999) / 1000) + L" s." : L"";
        if (g.sup.failures() >= 2) {
            state = TrayState::Error;
            headline = L"The worker stopped";
            detail = L"It exited unexpectedly; see the log." + in;
        } else {
            headline = L"Starting";
            detail = L"Starting the worker." + in;
        }
    } else if (!fresh) {
        if (ran > 90.0) {
            state = TrayState::Error;
            headline = L"Not reporting";
            detail = L"The worker has not reported for a while; see the log below.";
        } else {
            state = paused ? TrayState::Paused : TrayState::Idle;
            headline = L"Starting";
            detail = L"Starting the worker (the first start takes a little longer).";
        }
    } else if (s == "working" || inFlight > 0) {
        state = TrayState::Working;
        headline = L"Working";
        std::string answering = st.get("model") ? st.get("model")->str("now") : std::string();
        detail = !answering.empty() ? widen(answering)
                                    : std::to_wstring(inFlight) + (inFlight == 1 ? L" job" : L" jobs") + L" in flight.";
        if (paused) detail += L" Paused: no new compute job is taken; these finish first.";
    } else if (paused || s == "paused") {
        state = TrayState::Paused;
        headline = L"Paused";
        detail = localModelChosen() ? L"No compute job is taken until you resume; your own model work still runs."
                                    : L"No new job is taken until you resume.";
    } else if (s == "away") {
        state = TrayState::Paused;
        headline = L"Outside your hours";
        detail = L"It works only " + widen(st.get("limits") ? st.get("limits")->str("hours") : "") + L", by this computer's clock.";
    } else if (s == "offline") {
        state = TrayState::Error;
        headline = L"Offline";
        detail = L"The Cobalt server could not be reached; the worker keeps trying every minute.";
    } else if (s == "benched") {
        state = TrayState::Error;
        headline = L"Benched";
        detail = L"The Cobalt server is not sending this machine work at the moment; its owner can see why.";
    } else if (s == "refused") {
        state = TrayState::Error;
        headline = L"Partner key refused";
        detail = L"The Cobalt server refused the partner key (it may have been revoked). Reinstall with a live key.";
    } else if (s == "starting" || s.empty()) {
        headline = L"Starting";
        detail = L"Saying hello to the Cobalt server.";
    } else if (s == "code") {
        headline = L"Updating";
        detail = L"Downloading the worker code the fleet runs.";
    } else {
        headline = L"Waiting for work";
        detail = L"Connected; no job for this machine right now.";
    }

    // the jobs and today's totals
    std::wstring jobs;
    if (const Json* list = st.get("jobs"); fresh && list && list->type == Json::Type::Array) {
        for (const Json& j : list->a) {
            jobs += widen(j.str("label"));
            double done = j.num("done"), total = j.num("total");
            if (total > 0) jobs += L"    " + fmtNum(done) + L" / " + fmtNum(total) + L" (" + fmtNum(100.0 * done / total) + L"%)";
            jobs += L"    " + fmtNum(j.num("elapsed")) + L" s\r\n";
        }
    }
    const Json* model = fresh ? st.get("model") : nullptr;
    std::string answering = model ? model->str("now") : std::string();
    if (!answering.empty()) jobs = widen(answering) + L"\r\n" + jobs;
    if (jobs.empty()) jobs = L"No job in flight.\r\n";
    // the owner's model work, answered here with their own key
    if (model && model->flag("chosen")) {
        const Json* mt = model->get("today");
        double answers = mt ? mt->num("answers") : 0.0, failedM = mt ? mt->num("failed") : 0.0;
        jobs += L"\r\nModel work today: " + fmtNum(answers) + (answers == 1.0 ? L" answer, " : L" answers, ") +
                fmtNum(mt ? mt->num("tokens_in") : 0.0) + L" tokens in, " + fmtNum(mt ? mt->num("tokens_out") : 0.0) +
                L" tokens out" + (failedM > 0 ? L", " + fmtNum(failedM) + L" failed." : std::wstring(L".")) + L"\r\n";
        if (!model->flag("key"))
            jobs += L"No Anthropic key is stored: add one below to run your model work here.\r\n";
        else if (model->flag("refused"))
            jobs += L"Anthropic refused your Anthropic key: paste a new one below.\r\n";
    }
    if (const Json* today = st.get("today")) {
        jobs += L"\r\nToday: " + fmtNum(today->num("done")) + L" done, " + fmtNum(today->num("failed")) + L" failed, " +
                fmtNum(today->num("seconds") / 60.0) + L" min of work.\r\n";
    }
    std::string machine = st.str("machine");
    if (!machine.empty()) jobs += L"This machine: " + widen(machine) + L"    code " + widen(st.str("code")) + L"\r\n";
    std::string err = st.str("last_error");
    if (!err.empty()) jobs += L"Last error: " + widen(err) + L"\r\n";

    std::string tail = tailFile(logsDir() + L"\\worker.log", 48 * 1024, 200);
    if (tail.empty() || state == TrayState::Error) {
        std::string py = tailFile(logsDir() + L"\\python.log", 16 * 1024, 40);
        if (!py.empty()) tail += (tail.empty() ? "" : "\n--- python.log ---\n") + py;
    }

    g.headline = headline;
    g.detail = detail;
    g.jobsText = jobs;
    g.logText = crlf(tail);
    std::wstring tip = std::wstring(kAppName) + L": " + headline;
    if (state == TrayState::Working) tip += L" (" + std::to_wstring(inFlight) + L")";
    bool changed = state != g.state || tip != g.tip;
    g.state = state;
    g.tip = tip;
    if (changed) {
        trayUpdate(false);
        static const wchar_t* const names[] = {L"idle", L"working", L"paused", L"error"};
        logLine(L"tray: " + std::wstring(names[static_cast<int>(state)]) + L" - " + headline + L": " + detail);
    }
}

void showStatus() {
    if (!IsWindowVisible(g.wnd)) return;
    setText(IDC_STATUS, g.headline);
    setText(IDC_DETAIL, g.detail);
    setText(IDC_JOBS, g.jobsText);
    HWND log = ctl(IDC_LOG);
    if (getText(IDC_LOG) != g.logText) {
        SetWindowTextW(log, g.logText.c_str());
        int len = GetWindowTextLengthW(log);
        SendMessageW(log, EM_SETSEL, static_cast<WPARAM>(len), static_cast<LPARAM>(len));
        SendMessageW(log, EM_SCROLLCARET, 0, 0);
    }
    setText(IDC_PAUSE, limitsPaused() ? L"Resume" : L"Pause");
    Button_SetCheck(ctl(IDC_LOGON), runAtLogon() ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(ctl(IDC_LOCAL), localModelChosen() ? BST_CHECKED : BST_UNCHECKED);
    bool haveKey = fileExists(homeFile(L"anthropic.key"));
    setText(IDC_KEY_STATE, haveKey ? L"An Anthropic key is stored, encrypted for this Windows user. It is sent only to Anthropic."
                                   : L"No Anthropic key is stored.");
    EnableWindow(ctl(IDC_KEY_CLEAR), haveKey);
}

void refresh() {
    readStatus();
    showStatus();
}

// ---------------------------------------------------------------- the limits

void loadLimits() {
    Json lim = readJsonFile(homeFile(L"limits.json"));
    int cpus = cpuCount();
    double mem = totalMemoryGB();
    double cores = lim.num("cores", std::max(1, cpus - 2));
    double memGB = lim.num("memory_gb", std::max(4.0, std::floor(mem - 4.0)));
    std::string hours = lim.str("hours");
    if (hours == "any") hours.clear();
    setText(IDC_CORES, fmtNum(cores));
    setText(IDC_MEM, fmtNum(memGB, memGB == std::floor(memGB) ? 0 : 1));
    setText(IDC_HOURS, widen(hours));
    setText(IDC_CORES_OF, L"of " + std::to_wstring(cpus));
    setText(IDC_MEM_OF, L"of " + fmtNum(mem, 0) + L" GB");
}

bool validHours(const std::string& h) {
    if (h.empty()) return true;
    int a = 0, b = 0, c = 0, d = 0;
    char tail = 0;
    if (sscanf_s(h.c_str(), "%d:%d-%d:%d%c", &a, &b, &c, &d, &tail, 1) != 4) return false;
    return a >= 0 && a < 24 && c >= 0 && c < 24 && b >= 0 && b < 60 && d >= 0 && d < 60;
}

void saveLimits() {
    std::string coresS = trim(narrow(getText(IDC_CORES))), memS = trim(narrow(getText(IDC_MEM)));
    std::string hours = trim(narrow(getText(IDC_HOURS)));
    char* end = nullptr;
    long cores = std::strtol(coresS.c_str(), &end, 10);
    bool coresOk = !coresS.empty() && *end == '\0' && cores >= 1 && cores <= cpuCount();
    double mem = std::strtod(memS.c_str(), &end);
    bool memOk = !memS.empty() && *end == '\0' && mem >= 1.0 && mem <= std::ceil(totalMemoryGB());
    std::wstring problem;
    if (!coresOk) problem = L"Cores must be a whole number from 1 to " + std::to_wstring(cpuCount()) + L".";
    else if (!memOk) problem = L"Memory must be a number of GB from 1 to " + fmtNum(std::ceil(totalMemoryGB())) + L".";
    else if (!validHours(hours)) problem = L"Hours must look like 22:00-07:00, or be left empty for any hour.";
    if (!problem.empty()) {
        MessageBoxW(g.wnd, problem.c_str(), kAppName, MB_OK | MB_ICONWARNING);
        return;
    }
    std::wstring path = homeFile(L"limits.json");
    Json lim = readJsonFile(path);
    lim.set("cores", Json::number(static_cast<double>(cores)));
    lim.set("memory_gb", Json::number(std::round(mem * 10.0) / 10.0));
    lim.set("hours", Json::string(hours.empty() ? "any" : hours));
    if (writeJsonFile(path, lim)) {
        logLine(L"limits saved: " + std::to_wstring(cores) + L" cores, " + fmtNum(mem, 1) + L" GB, hours " +
                widen(hours.empty() ? "any" : hours));
        setText(IDC_DETAIL, L"Saved. The worker uses the new limits on its next pass.");
    } else {
        MessageBoxW(g.wnd, L"The limits could not be saved.", kAppName, MB_OK | MB_ICONERROR);
    }
}

void togglePause() {
    bool now = !limitsPaused();
    setPaused(now);
    logLine(now ? L"paused by the owner" : L"resumed by the owner");
    refresh();
}

void toggleLogon() {
    setRunAtLogon(!runAtLogon());
    showStatus();
}

// ---------------------------------------------------------------- the Anthropic key and the model-work option

void toggleLocalModel() {
    bool on = Button_GetCheck(ctl(IDC_LOCAL)) == BST_CHECKED;
    if (!setLocalModel(on)) {
        MessageBoxW(g.wnd, L"The setting could not be saved.", kAppName, MB_OK | MB_ICONERROR);
        return;
    }
    logLine(on ? L"model work on this machine: on" : L"model work on this machine: off");
    bool haveKey = fileExists(homeFile(L"anthropic.key"));
    setText(IDC_DETAIL, on ? (haveKey ? L"Your model work now runs here, with your own Anthropic key. Restarting the worker."
                                      : L"Add your Anthropic key below: your model work runs here once one is stored.")
                           : L"Your model work no longer runs here. Restarting the worker.");
    g.sup.restart();
}

void saveAnthropicKey() {
    HWND edit = ctl(IDC_KEY_EDIT);
    int n = GetWindowTextLengthW(edit);
    std::wstring w(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(edit, w.data(), n + 1);
    w.resize(static_cast<size_t>(n));
    std::string key = trim(narrow(w));
    SecureZeroMemory(w.data(), w.size() * sizeof(wchar_t));
    SetWindowTextW(edit, L"");
    bool plausible = key.size() >= 20 && key.size() <= 400 && key.find_first_of(" \t\r\n\"'") == std::string::npos;
    if (!plausible) {
        bool empty = key.empty();
        SecureZeroMemory(key.data(), key.size());
        MessageBoxW(g.wnd, empty ? L"Paste your Anthropic API key into the box first."
                                 : L"That does not look like an Anthropic API key. Copy it again from the Anthropic Console.",
                    kAppName, MB_OK | MB_ICONWARNING);
        return;
    }
    bool ok = saveSecret(L"anthropic", key);
    SecureZeroMemory(key.data(), key.size());
    if (!ok) {
        MessageBoxW(g.wnd, L"The key could not be stored.", kAppName, MB_OK | MB_ICONERROR);
        return;
    }
    logLine(L"Anthropic key changed (DPAPI, this Windows user)");
    setText(IDC_DETAIL, L"Your Anthropic key is stored. Restarting the worker so it is used.");
    g.sup.restart();
    showStatus();
}

void clearAnthropicKey() {
    if (MessageBoxW(g.wnd, L"Remove your Anthropic key from this computer? Your model work will not run here until you add one again.",
                    kAppName, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return;
    if (!deleteSecret(L"anthropic")) {
        MessageBoxW(g.wnd, L"The key could not be removed.", kAppName, MB_OK | MB_ICONERROR);
        return;
    }
    logLine(L"Anthropic key removed");
    setText(IDC_DETAIL, L"Your Anthropic key is removed. Restarting the worker.");
    g.sup.restart();
    showStatus();
}

void viewLog() {
    std::wstring path = logsDir() + L"\\worker.log";
    if (!fileExists(path)) path = logsDir() + L"\\python.log";
    if (!fileExists(path)) path = logsDir() + L"\\app.log";
    std::wstring arg = L"\"" + path + L"\"";
    ShellExecuteW(g.wnd, L"open", L"notepad.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
}

// ---------------------------------------------------------------- the window

void makeFonts() {
    for (HFONT f : {g.font, g.bold, g.mono}) if (f) DeleteObject(f);
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, g.dpi);
    g.font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW b = ncm.lfMessageFont;
    b.lfWeight = FW_SEMIBOLD;
    b.lfHeight = MulDiv(b.lfHeight, 3, 2);
    g.bold = CreateFontIndirectW(&b);
    LOGFONTW m{};
    m.lfHeight = -MulDiv(9, static_cast<int>(g.dpi), 72);
    m.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(m.lfFaceName, L"Consolas");
    g.mono = CreateFontIndirectW(&m);
}

void applyFonts() {
    for (HWND c = GetWindow(g.wnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        int id = GetDlgCtrlID(c);
        HFONT f = id == IDC_STATUS ? g.bold : (id == IDC_LOG ? g.mono : g.font);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
    }
}

void layout() {
    RECT rc{};
    GetClientRect(g.wnd, &rc);
    int W = rc.right, H = rc.bottom, m = S(14), x = m, w = W - 2 * m;
    int lineH = S(23), y = m;
    auto place = [](int id, int px, int py, int pw, int ph) { MoveWindow(ctl(id), px, py, pw, ph, FALSE); };
    place(IDC_STATUS, x, y, w, S(28)); y += S(30);
    place(IDC_DETAIL, x, y, w, S(36)); y += S(40);

    int gy = y;
    place(IDC_LIMITS_GROUP, x, gy, w, S(128));
    int lx = x + S(12), lw = S(110), ex = lx + lw, ew = S(70);
    y = gy + S(24);
    place(IDC_CORES_LBL, lx, y + S(3), lw, lineH); place(IDC_CORES, ex, y, ew, lineH); place(IDC_CORES_OF, ex + ew + S(8), y + S(3), S(120), lineH);
    y += lineH + S(6);
    place(IDC_MEM_LBL, lx, y + S(3), lw, lineH); place(IDC_MEM, ex, y, ew, lineH); place(IDC_MEM_OF, ex + ew + S(8), y + S(3), S(120), lineH);
    y += lineH + S(6);
    place(IDC_HOURS_LBL, lx, y + S(3), lw, lineH); place(IDC_HOURS, ex, y, S(110), lineH);
    place(IDC_HOURS_HINT, ex + S(118), y + S(3), w - (ex + S(118) - x) - S(12), lineH);
    int bx = x + w - S(12) - S(110);
    place(IDC_SAVE, bx, gy + S(22), S(110), S(28));
    place(IDC_PAUSE, bx, gy + S(56), S(110), S(28));
    y = gy + S(136);
    place(IDC_LOGON, x, y, w, lineH); y += lineH + S(6);

    int my = y;
    place(IDC_MODEL_GROUP, x, my, w, S(118));
    y = my + S(22);
    place(IDC_LOCAL, lx, y, w - S(24), lineH); y += lineH + S(4);
    place(IDC_KEY_STATE, lx, y + S(2), w - S(24), lineH); y += lineH + S(4);
    int kb = S(90);
    place(IDC_KEY_EDIT, lx, y, w - S(24) - 2 * kb - S(16), lineH + S(2));
    place(IDC_KEY_SAVE, x + w - S(12) - 2 * kb - S(8), y - S(2), kb, S(28));
    place(IDC_KEY_CLEAR, x + w - S(12) - kb, y - S(2), kb, S(28));
    y = my + S(126);

    place(IDC_JOBS_LBL, x, y, w, S(18)); y += S(20);
    place(IDC_JOBS, x, y, w, S(110)); y += S(118);
    place(IDC_LOG_LBL, x, y, w, S(18)); y += S(20);
    int bottom = H - m - S(28) - S(8);
    place(IDC_LOG, x, y, w, std::max(S(60), bottom - y));
    place(IDC_VIEWLOG, x, H - m - S(28), S(110), S(28));
    place(IDC_HIDE, x + w - S(110), H - m - S(28), S(110), S(28));
    InvalidateRect(g.wnd, nullptr, TRUE);
}

void createControls() {
    auto make = [](const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
        CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, g.wnd,
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g.inst, nullptr);
    };
    make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IDC_STATUS);
    make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IDC_DETAIL);
    make(L"BUTTON", L"Your limits", BS_GROUPBOX, IDC_LIMITS_GROUP);
    make(L"STATIC", L"Cores", SS_LEFT, IDC_CORES_LBL);
    make(L"EDIT", L"", ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP, IDC_CORES, WS_EX_CLIENTEDGE);
    make(L"STATIC", L"", SS_LEFT, IDC_CORES_OF);
    make(L"STATIC", L"Memory (GB)", SS_LEFT, IDC_MEM_LBL);
    make(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, IDC_MEM, WS_EX_CLIENTEDGE);
    make(L"STATIC", L"", SS_LEFT, IDC_MEM_OF);
    make(L"STATIC", L"Hours", SS_LEFT, IDC_HOURS_LBL);
    make(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, IDC_HOURS, WS_EX_CLIENTEDGE);
    make(L"STATIC", L"e.g. 22:00-07:00; empty = any hour", SS_LEFT, IDC_HOURS_HINT);
    make(L"BUTTON", L"Save limits", BS_PUSHBUTTON | WS_TABSTOP, IDC_SAVE);
    make(L"BUTTON", L"Pause", BS_PUSHBUTTON | WS_TABSTOP, IDC_PAUSE);
    make(L"BUTTON", L"Start Cobalt Worker when I sign in to Windows", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_LOGON);
    make(L"BUTTON", L"Your Anthropic key", BS_GROUPBOX, IDC_MODEL_GROUP);
    make(L"BUTTON", L"Run my model work on this machine (my Anthropic key stays here)", BS_AUTOCHECKBOX | WS_TABSTOP,
         IDC_LOCAL);
    make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IDC_KEY_STATE);
    make(L"EDIT", L"", ES_PASSWORD | ES_AUTOHSCROLL | WS_TABSTOP, IDC_KEY_EDIT, WS_EX_CLIENTEDGE);
    make(L"BUTTON", L"Save key", BS_PUSHBUTTON | WS_TABSTOP, IDC_KEY_SAVE);
    make(L"BUTTON", L"Clear key", BS_PUSHBUTTON | WS_TABSTOP, IDC_KEY_CLEAR);
    Edit_SetCueBannerTextFocused(ctl(IDC_KEY_EDIT), L"Paste a new Anthropic API key", TRUE);
    SendMessageW(ctl(IDC_KEY_EDIT), EM_SETLIMITTEXT, 400, 0);
    make(L"STATIC", L"Jobs", SS_LEFT, IDC_JOBS_LBL);
    make(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, IDC_JOBS, WS_EX_CLIENTEDGE);
    make(L"STATIC", L"Log", SS_LEFT, IDC_LOG_LBL);
    make(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL | WS_VSCROLL | WS_HSCROLL, IDC_LOG,
         WS_EX_CLIENTEDGE);
    make(L"BUTTON", L"View log", BS_PUSHBUTTON | WS_TABSTOP, IDC_VIEWLOG);
    make(L"BUTTON", L"Hide", BS_PUSHBUTTON | WS_TABSTOP, IDC_HIDE);
    SendMessageW(ctl(IDC_LOG), EM_SETLIMITTEXT, 0, 0);
    applyFonts();
    layout();
}

void showWindow() {
    loadLimits();
    ShowWindow(g.wnd, IsIconic(g.wnd) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(g.wnd);
    refresh();
}

void trayMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, CMD_OPEN, L"Open");
    AppendMenuW(m, MF_STRING, CMD_PAUSE, limitsPaused() ? L"Resume" : L"Pause");
    AppendMenuW(m, MF_STRING | (runAtLogon() ? MF_CHECKED : 0), CMD_LOGON, L"Run at logon");
    AppendMenuW(m, MF_STRING, CMD_VIEWLOG, L"View log");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_QUIT, L"Quit");
    SetMenuDefaultItem(m, CMD_OPEN, FALSE);
    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(g.wnd);
    UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, g.wnd, nullptr);
    DestroyMenu(m);
    PostMessageW(g.wnd, WM_NULL, 0, 0);
    switch (cmd) {
    case CMD_OPEN: showWindow(); break;
    case CMD_PAUSE: togglePause(); break;
    case CMD_LOGON: toggleLogon(); break;
    case CMD_VIEWLOG: viewLog(); break;
    case CMD_QUIT: DestroyWindow(g.wnd); break;
    default: break;
    }
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g.taskbarCreated && g.taskbarCreated) {
        g.trayAdded = false;
        trayUpdate(true);
        return 0;
    }
    switch (msg) {
    case WM_CREATE:
        g.wnd = hwnd;
        g.dpi = GetDpiForWindow(hwnd);
        makeFonts();
        createControls();
        return 0;
    case WM_DPICHANGED: {
        g.dpi = HIWORD(wp);
        makeFonts();
        applyFonts();
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        layout();
        return 0;
    }
    case WM_SIZE: layout(); return 0;
    case WM_GETMINMAXINFO: {
        auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
        mm->ptMinTrackSize = {S(560), S(690)};
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_SUPERVISE) g.sup.tick();
        else if (wp == TIMER_STATUS) refresh();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_SAVE: saveLimits(); break;
        case IDC_PAUSE: togglePause(); break;
        case IDC_LOGON: if (HIWORD(wp) == BN_CLICKED) { setRunAtLogon(Button_GetCheck(ctl(IDC_LOGON)) == BST_CHECKED); } break;
        case IDC_LOCAL: if (HIWORD(wp) == BN_CLICKED) toggleLocalModel(); break;
        case IDC_KEY_SAVE: saveAnthropicKey(); break;
        case IDC_KEY_CLEAR: clearAnthropicKey(); break;
        case IDC_VIEWLOG: viewLog(); break;
        case IDC_HIDE: case IDCANCEL: ShowWindow(hwnd, SW_HIDE); break;
        default: break;
        }
        return 0;
    case WM_APP_TRAY:
        switch (LOWORD(lp)) {
        case WM_LBUTTONDBLCLK: case NIN_SELECT: case NIN_KEYSELECT: showWindow(); break;
        case WM_CONTEXTMENU: case WM_RBUTTONUP: trayMenu(); break;
        default: break;
        }
        return 0;
    case WM_APP_SHOW: showWindow(); return 0;
    case WM_APP_QUIT: DestroyWindow(hwnd); return 0;
    case WM_CLOSE: ShowWindow(hwnd, SW_HIDE); return 0;
    case WM_QUERYENDSESSION: return TRUE;
    case WM_ENDSESSION:
        if (wp) { g.sup.stop(); trayRemove(); }
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_SUPERVISE);
        KillTimer(hwnd, TIMER_STATUS);
        trayRemove();
        g.sup.stop();
        logLine(L"app stopped");
        PostQuitMessage(0);
        return 0;
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int runApp(HINSTANCE inst, bool show) {
    g.inst = inst;
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    g.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    LoadIconMetric(inst, MAKEINTRESOURCEW(1), LIM_LARGE, &g.appIcon);
    LoadIconMetric(inst, MAKEINTRESOURCEW(1), LIM_SMALL, &g.appIconSmall);
    makeIcons();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hIcon = g.appIcon;
    wc.hIconSm = g.appIconSmall;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    UINT sysDpi = GetDpiForSystem();
    int w = MulDiv(640, static_cast<int>(sysDpi), 96), h = MulDiv(810, static_cast<int>(sysDpi), 96);
    HWND hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, kWindowClass, kAppName, WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, w, h, nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;
    logLine(L"app started (version " COBALT_WORKER_VERSION_W L")");
    g.tip = std::wstring(kAppName) + L": starting";
    trayUpdate(true);
    g.sup.tick();
    SetTimer(hwnd, TIMER_SUPERVISE, 1000, nullptr);
    SetTimer(hwnd, TIMER_STATUS, 2000, nullptr);
    refresh();
    if (show) showWindow();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    for (HICON i : g.icons) if (i) DestroyIcon(i);
    return static_cast<int>(msg.wParam);
}

}  // namespace cw
