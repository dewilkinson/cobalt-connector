// The tray icon (idle / working / paused / error) and the small window: the machine's limits, its name and how
// it updates, the owner's Anthropic key and the "run my model work on this machine" option, what the pull loop
// reports in status.json (refreshed every 2 s) and the tail of its log. Notifications (tray toasts) are sent
// once per change and at most one of a kind an hour; the window always shows the same state.
#include "app.hpp"

#include <commctrl.h>
#include <windowsx.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <memory>
#include <vector>

namespace cw {

namespace {

enum class TrayState { Idle, Working, Paused, Error };

enum : int {
    IDC_STATUS = 100, IDC_DETAIL, IDC_LIMITS_GROUP, IDC_CORES_LBL, IDC_CORES, IDC_CORES_OF, IDC_MEM_LBL, IDC_MEM,
    IDC_MEM_OF, IDC_HOURS_LBL, IDC_HOURS, IDC_HOURS_HINT, IDC_SAVE, IDC_PAUSE, IDC_LOGON, IDC_JOBS_LBL, IDC_JOBS,
    IDC_LOG_LBL, IDC_LOG, IDC_VIEWLOG, IDC_HIDE, IDC_MODEL_GROUP, IDC_LOCAL, IDC_KEY_STATE, IDC_KEY_EDIT, IDC_KEY_SAVE,
    IDC_KEY_CLEAR, IDC_MACHINE_GROUP, IDC_NAME_LBL, IDC_NAME, IDC_NAME_SAVE, IDC_UPDATES_LBL, IDC_UPDATES,
    IDC_UPDATE_NOW, IDC_VERSION,
};
enum : UINT { CMD_OPEN = 1, CMD_PAUSE, CMD_LOGON, CMD_VIEWLOG, CMD_QUIT };
enum : UINT_PTR { TIMER_SUPERVISE = 1, TIMER_STATUS = 2, TIMER_UPDATE = 3, TIMER_QUIT = 4 };

// What a notification is about: at most one of each kind an hour.
enum class Notice { KeyRefused, Benched, Stopped, Outdated, UpdateReady, UpdateAsk, UpdateRequired, UpdateFailed,
                    Updated, Count };
// What a click on the notification does.
enum class Click { Open, Update, Download };

constexpr time_t kCheckEvery = 6 * 3600;      // the update check, when all is well
constexpr time_t kFirstCheck = 60;            // after the app starts

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
    // notifications
    ULONGLONG lastNotice[static_cast<int>(Notice::Count)] = {};
    std::string noticeState;          // the status last notified about ("" while all is well)
    Click balloonClick = Click::Open;
    std::wstring pendingNotice;       // sent once the tray icon is up (the result of an update, at start)
    // updates
    std::wstring updateLine = L"Version " COBALT_CONNECTOR_VERSION_W;
    UpdateResult available;           // a newer version, when the owner chose to be asked
    bool haveAvailable = false;
    bool installing = false;
    time_t nextUpdateCheck = 0;
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

// A tray notification (a toast on Windows 10 and 11). Focus Assist may hold it back: it is a courtesy only.
void trayNotify(Notice kind, const std::wstring& text, Click click) {
    ULONGLONG now = GetTickCount64();
    ULONGLONG& last = g.lastNotice[static_cast<int>(kind)];
    if (last && now - last < 3600ull * 1000ull) return;
    if (!g.trayAdded) trayUpdate(true);
    if (!g.trayAdded) return;
    last = now;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof nid;
    nid.hWnd = g.wnd;
    nid.uID = 1;
    nid.uFlags = NIF_INFO;
    wcsncpy_s(nid.szInfoTitle, kAppName, _TRUNCATE);
    wcsncpy_s(nid.szInfo, text.c_str(), _TRUNCATE);
    nid.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON | NIIF_RESPECT_QUIET_TIME;
    nid.hBalloonIcon = g.appIcon;
    g.balloonClick = click;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    logLine(L"notification: " + text);
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
            headline = L"Stopped";
            detail = L"Its work process exited unexpectedly; see the log." + in;
        } else {
            headline = L"Starting";
            detail = L"Starting up." + in;
        }
    } else if (!fresh) {
        if (ran > 90.0) {
            state = TrayState::Error;
            headline = L"Not reporting";
            detail = L"The work process has not reported for a while; see the log below.";
        } else {
            state = paused ? TrayState::Paused : TrayState::Idle;
            headline = L"Starting";
            detail = L"Starting up (the first start takes a little longer).";
        }
    } else if (s == "outdated") {
        state = TrayState::Error;
        headline = L"Update needed";
        std::string why = st.str("why");
        detail = !why.empty() ? widen(why)
                              : L"This Cobalt Connector is too old to take work. It will update itself, or you can "
                                L"download the new one from the Cobalt Connector page.";
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
        detail = L"The Cobalt server could not be reached; it keeps trying every minute.";
    } else if (s == "benched") {
        state = TrayState::Error;
        headline = L"Benched";
        detail = L"The Cobalt server is not sending this machine work at the moment; its owner can see why.";
    } else if (s == "refused") {
        state = TrayState::Error;
        headline = L"Partner key refused";
        detail = L"The Cobalt server refused the partner key (it may have been revoked). Run the Cobalt Connector "
                 L"installer again and enter a new key.";
    } else if (s == "starting" || s.empty()) {
        headline = L"Starting";
        detail = L"Saying hello to the Cobalt server.";
    } else if (s == "code") {
        headline = L"Updating";
        detail = L"Downloading the code the fleet runs.";
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
    if (!machine.empty()) {
        std::string name = machineName();
        jobs += L"This machine: " + (name.empty() ? computerName() : widen(name)) + L"    id " + widen(machine) +
                L"    code " + widen(st.str("code")) + L"\r\n";
    }
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

    // a notification when something needs the owner, once per change
    std::string notice;
    if (fresh && s == "refused") notice = "refused";
    else if (fresh && s == "benched") notice = "benched";
    else if (fresh && s == "outdated") notice = "outdated";
    else if (!g.sup.running() && g.sup.problem().empty() && g.sup.failures() >= 3) notice = "stopped";
    if (notice != g.noticeState) {
        g.noticeState = notice;
        if (notice == "refused") {
            trayNotify(Notice::KeyRefused, L"Cobalt did not accept your partner key. Run the Cobalt Connector installer "
                                           L"again and enter a new key.", Click::Open);
        } else if (notice == "benched") {
            std::string why = st.str("bench_why");
            if (why.empty()) why = st.str("why");
            trayNotify(Notice::Benched,
                       why.empty() ? std::wstring(L"Cobalt has stopped sending work to this machine for now. Its owner can see why.")
                                   : L"Cobalt has stopped sending work to this machine: " + widen(why),
                       Click::Open);
        } else if (notice == "outdated") {
            trayNotify(Notice::Outdated, L"Cobalt has paused work for this machine until it is updated. Click here to "
                                         L"download the new version.", Click::Download);
        } else if (notice == "stopped") {
            trayNotify(Notice::Stopped, L"Cobalt Connector's work process keeps stopping. Open Cobalt Connector to see "
                                        L"the log.", Click::Open);
        }
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
    setText(IDC_VERSION, g.updateLine);
    EnableWindow(ctl(IDC_UPDATE_NOW), g.haveAvailable && !g.installing && !updateThreadBusy());
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
    setText(IDC_NAME, widen(machineName()));
    SendMessageW(ctl(IDC_UPDATES), CB_SETCURSEL, updatesAsk() ? 1 : 0, 0);
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
        setText(IDC_DETAIL, L"Saved. The new limits apply from the next job.");
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
    setText(IDC_DETAIL, on ? (haveKey ? L"Your model work now runs here, with your own Anthropic key. Restarting so it takes effect."
                                      : L"Add your Anthropic key below: your model work runs here once one is stored.")
                           : L"Your model work no longer runs here. Restarting so it takes effect.");
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
    setText(IDC_DETAIL, L"Your Anthropic key is stored. Restarting so it is used.");
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
    setText(IDC_DETAIL, L"Your Anthropic key is removed. Restarting so it takes effect.");
    g.sup.restart();
    showStatus();
}

// ---------------------------------------------------------------- the machine's name

void saveName() {
    std::string name = trim(narrow(getText(IDC_NAME)));
    if (!validMachineName(name)) {
        MessageBoxW(g.wnd, L"A machine name can be up to 32 characters: letters, digits, spaces, - _ and . only.",
                    kAppName, MB_OK | MB_ICONWARNING);
        return;
    }
    if (name == machineName()) {
        setText(IDC_DETAIL, L"That name is already saved.");
        return;
    }
    if (!setMachineName(name)) {
        MessageBoxW(g.wnd, L"The name could not be saved.", kAppName, MB_OK | MB_ICONERROR);
        return;
    }
    logLine(name.empty() ? std::wstring(L"machine name cleared") : L"machine name set to \"" + widen(name) + L"\"");
    setText(IDC_DETAIL, name.empty() ? std::wstring(L"No name chosen: other partners will see this machine as \"<your name>'s machine N\" (you and the fleet's owner see its computer name). Restarting so Cobalt hears it.")
                                     : L"Cobalt will show this machine as \"" + widen(name) + L"\". Restarting so Cobalt hears it.");
    g.sup.restart();
}

// ---------------------------------------------------------------- updates

std::wstring versionLine(const std::wstring& rest) {
    return L"Version " COBALT_CONNECTOR_VERSION_W + (rest.empty() ? std::wstring() : L" - " + rest);
}

// What the owner is told when an update did not install: whether it will be tried again is the truth.
std::wstring notInstalledText(const std::wstring& ver, bool givenUp) {
    if (givenUp)
        return L"The update to " + ver + L" did not install, so Cobalt Connector will try again when a newer version "
               L"is out. You can also download it from the Cobalt Connector page.";
    return L"The update to " + ver + L" did not install. Cobalt Connector will try it again in a few hours.";
}

void quitForUpdate() {
    if (g.installing) DestroyWindow(g.wnd);
}

void installNow(const UpdateResult& r) {
    std::wstring ver = widen(r.offer.latest);
    g.installing = true;
    g.sup.stop();
    std::wstring message;
    if (!launchInstaller(r, message)) {
        g.installing = false;
        logLine(L"update: " + message);
        Json st = readUpdateState();
        bool givenUp = strikeRelease(st, r.offer.latest, r.offer.sha256);
        writeUpdateState(st);
        g.updateLine = versionLine(ver + L" could not be installed");
        trayNotify(Notice::UpdateFailed, notInstalledText(ver, givenUp), Click::Open);
        g.sup.restart();
        showStatus();
        return;
    }
    logLine(L"update: " + message);
    g.updateLine = versionLine(L"installing " + ver);
    showStatus();
    // the installer asks this app to quit too; this is in case it is quicker than that
    SetTimer(g.wnd, TIMER_QUIT, 4000, nullptr);
}

void startDownloadNow() {
    if (!g.haveAvailable || g.installing) return;
    if (!startUpdateThread(g.wnd, true, true)) return;
    g.updateLine = versionLine(L"downloading " + widen(g.available.offer.latest) + L"...");
    setText(IDC_DETAIL, L"Downloading the new version. It installs itself and starts again when it is ready.");
    showStatus();
}

void updateTick() {
    if (g.installing || updateThreadBusy()) return;
    time_t now = std::time(nullptr);
    if (now < g.nextUpdateCheck) return;
    g.nextUpdateCheck = now + kCheckEvery;            // the result sets the real time
    startUpdateThread(g.wnd, !updatesAsk(), false);
}

void onUpdateResult(UpdateResult* raw) {
    std::unique_ptr<UpdateResult> r(raw);
    time_t now = std::time(nullptr);
    Json st = readUpdateState();
    std::wstring ver = widen(r->offer.latest);
    if (!r->message.empty()) logLine(L"update: " + r->message);
    auto fine = [&] {
        st.set("failures", Json::number(0));
        g.nextUpdateCheck = now + kCheckEvery;
        st.set("next_check", Json::number(static_cast<double>(g.nextUpdateCheck)));
    };
    switch (r->outcome) {
    case UpdateOutcome::Failed: {
        int failures = static_cast<int>(st.num("failures")) + 1;
        long long delay = std::min<long long>(24ll * 3600, 3600ll << std::min(failures - 1, 5));   // 1 h, 2 h, ... 24 h
        st.set("failures", Json::number(failures));
        g.nextUpdateCheck = now + static_cast<time_t>(delay);
        st.set("next_check", Json::number(static_cast<double>(g.nextUpdateCheck)));
        g.updateLine = versionLine(L"could not check for updates; trying again later");
        if (r->forced) setText(IDC_DETAIL, L"The update could not be downloaded. Cobalt Connector will try again later.");
        break;
    }
    case UpdateOutcome::NoInfo:
        fine();
        g.updateLine = versionLine(L"");
        break;
    case UpdateOutcome::UpToDate:
        fine();
        g.haveAvailable = false;
        g.updateLine = versionLine(L"up to date");
        break;
    case UpdateOutcome::Available:
        fine();
        g.available = *r;
        g.haveAvailable = true;
        g.updateLine = versionLine(ver + L" is available");
        if (st.str("asked") != r->offer.latest) {
            st.set("asked", Json::string(r->offer.latest));
            trayNotify(Notice::UpdateAsk, L"A new version of Cobalt Connector (" + ver + L") is out. Click here to "
                                          L"install it.", Click::Update);
        }
        break;
    case UpdateOutcome::Skipped:
        fine();
        g.haveAvailable = false;
        g.updateLine = versionLine(ver + L" could not be installed on this machine");
        if (r->offer.required && now - static_cast<time_t>(st.num("required_notified")) > 24 * 3600) {
            st.set("required_notified", Json::number(static_cast<double>(now)));
            trayNotify(Notice::UpdateRequired, L"Cobalt has paused work for this machine until it is updated. Click "
                                               L"here to download the new version.", Click::Download);
        }
        break;
    case UpdateOutcome::Rejected:
        fine();
        g.haveAvailable = false;
        addBadRelease(st, r->offer.latest, r->offer.sha256);
        g.updateLine = versionLine(ver + L" did not download correctly");
        trayNotify(Notice::UpdateFailed, L"The update to " + ver + L" did not download correctly, so it was not "
                                         L"installed. Cobalt Connector will try again when a newer version is out. "
                                         L"You can also download it from the Cobalt Connector page.",
                   Click::Open);
        break;
    case UpdateOutcome::Ready:
        fine();
        g.haveAvailable = false;
        writeUpdateState(st);
        trayNotify(Notice::UpdateReady, L"A new version of Cobalt Connector (" + ver + L") is ready. It will install "
                                        L"itself now and start again in a minute.", Click::Open);
        installNow(*r);
        return;
    }
    writeUpdateState(st);
    showStatus();
}

void changeUpdates() {
    bool ask = SendMessageW(ctl(IDC_UPDATES), CB_GETCURSEL, 0, 0) == 1;
    if (ask == updatesAsk()) return;
    if (!setUpdatesAsk(ask)) {
        MessageBoxW(g.wnd, L"The setting could not be saved.", kAppName, MB_OK | MB_ICONERROR);
        return;
    }
    logLine(ask ? L"updates: ask first" : L"updates: install automatically");
    setText(IDC_DETAIL, ask ? L"Saved. Cobalt Connector will ask you before it installs a new version."
                            : L"Saved. Cobalt Connector will install new versions by itself.");
    if (!ask && g.haveAvailable) startDownloadNow();
}

// At start: did the update this app last ran finish?
void checkFinishedUpdate() {
    Json st = readUpdateState();
    std::string target = st.str("target");
    if (target.empty()) return;
    st.set("target", Json::string(""));
    std::wstring ver = widen(target);
    if (target == COBALT_CONNECTOR_VERSION) {
        logLine(L"Updated to " + ver + L".");
        deleteTree(homeDir() + L"\\updates");
        st.set("failures", Json::number(0));
        st.set("strikes", Json::object());
        g.pendingNotice = L"Cobalt Connector updated itself to version " + ver + L".";
    } else {
        // a reboot, a power cut or a file held open mid-install looks the same as a bad release here: counted
        bool givenUp = strikeRelease(st, target, st.str("sha256"));
        logLine(L"The update to " + ver + L" did not install" + (givenUp ? L"; it is not tried again." : L"."));
        g.pendingNotice = notInstalledText(ver, givenUp);
    }
    writeUpdateState(st);
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

    // this machine: its name, how it updates, the version
    int ty = y;
    place(IDC_MACHINE_GROUP, x, ty, w, S(108));
    y = ty + S(22);
    place(IDC_NAME_LBL, lx, y + S(3), lw, lineH); place(IDC_NAME, ex, y, S(200), lineH);
    place(IDC_NAME_SAVE, bx, y - S(2), S(110), S(28));
    y += lineH + S(6);
    place(IDC_UPDATES_LBL, lx, y + S(3), lw, lineH); place(IDC_UPDATES, ex, y, S(200), S(160));
    place(IDC_UPDATE_NOW, bx, y - S(2), S(110), S(28));
    y += lineH + S(6);
    place(IDC_VERSION, lx, y + S(2), std::max(S(60), bx - lx - S(8)), lineH);
    y = ty + S(116);

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
    place(IDC_JOBS, x, y, w, S(90)); y += S(98);
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
    make(L"BUTTON", L"This machine", BS_GROUPBOX, IDC_MACHINE_GROUP);
    make(L"STATIC", L"Machine name", SS_LEFT, IDC_NAME_LBL);
    make(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, IDC_NAME, WS_EX_CLIENTEDGE);
    make(L"BUTTON", L"Save name", BS_PUSHBUTTON | WS_TABSTOP, IDC_NAME_SAVE);
    make(L"STATIC", L"Updates", SS_LEFT, IDC_UPDATES_LBL);
    make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_UPDATES);
    make(L"BUTTON", L"Update now", BS_PUSHBUTTON | WS_TABSTOP, IDC_UPDATE_NOW);
    make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IDC_VERSION);
    SendMessageW(ctl(IDC_NAME), EM_SETLIMITTEXT, 32, 0);
    Edit_SetCueBannerText(ctl(IDC_NAME), computerName().c_str());
    SendMessageW(ctl(IDC_UPDATES), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Install automatically"));
    SendMessageW(ctl(IDC_UPDATES), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Ask me first"));
    SendMessageW(ctl(IDC_UPDATES), CB_SETCURSEL, updatesAsk() ? 1 : 0, 0);
    EnableWindow(ctl(IDC_UPDATE_NOW), FALSE);
    make(L"BUTTON", L"Start Cobalt Connector when I sign in to Windows", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_LOGON);
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
        mm->ptMinTrackSize = {S(560), S(800)};
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_SUPERVISE) { if (!g.installing) g.sup.tick(); }
        else if (wp == TIMER_STATUS) refresh();
        else if (wp == TIMER_UPDATE) updateTick();
        else if (wp == TIMER_QUIT) { KillTimer(hwnd, TIMER_QUIT); quitForUpdate(); }
        return 0;
    case WM_APP_UPDATE:
        onUpdateResult(reinterpret_cast<UpdateResult*>(lp));
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_SAVE: saveLimits(); break;
        case IDC_PAUSE: togglePause(); break;
        case IDC_LOGON: if (HIWORD(wp) == BN_CLICKED) { setRunAtLogon(Button_GetCheck(ctl(IDC_LOGON)) == BST_CHECKED); } break;
        case IDC_LOCAL: if (HIWORD(wp) == BN_CLICKED) toggleLocalModel(); break;
        case IDC_KEY_SAVE: saveAnthropicKey(); break;
        case IDC_KEY_CLEAR: clearAnthropicKey(); break;
        case IDC_NAME_SAVE: saveName(); break;
        case IDC_UPDATES: if (HIWORD(wp) == CBN_SELCHANGE) changeUpdates(); break;
        case IDC_UPDATE_NOW: startDownloadNow(); break;
        case IDC_VIEWLOG: viewLog(); break;
        case IDC_HIDE: case IDCANCEL: ShowWindow(hwnd, SW_HIDE); break;
        default: break;
        }
        return 0;
    case WM_APP_TRAY:
        switch (LOWORD(lp)) {
        case WM_LBUTTONDBLCLK: case NIN_SELECT: case NIN_KEYSELECT: showWindow(); break;
        case WM_CONTEXTMENU: case WM_RBUTTONUP: trayMenu(); break;
        case NIN_BALLOONUSERCLICK:
            if (g.balloonClick == Click::Update && g.haveAvailable) startDownloadNow();
            else if (g.balloonClick == Click::Download) ShellExecuteW(hwnd, L"open", kDownloadPage, nullptr, nullptr, SW_SHOWNORMAL);
            else showWindow();
            break;
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
        KillTimer(hwnd, TIMER_UPDATE);
        KillTimer(hwnd, TIMER_QUIT);
        cancelUpdates();
        for (int i = 0; i < 30 && updateThreadBusy(); ++i) Sleep(100);   // let a download stop cleanly
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
    int w = MulDiv(640, static_cast<int>(sysDpi), 96), h = MulDiv(900, static_cast<int>(sysDpi), 96);
    RECT work{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0) && work.bottom - work.top > 200)
        h = std::min(h, static_cast<int>(work.bottom - work.top));
    HWND hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, kWindowClass, kAppName, WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, w, h, nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;
    logLine(L"app started (version " COBALT_CONNECTOR_VERSION_W L")");
    g.tip = std::wstring(kAppName) + L": starting";
    trayUpdate(true);
    checkFinishedUpdate();
    if (!g.pendingNotice.empty()) {
        trayNotify(Notice::Updated, g.pendingNotice, Click::Open);
        g.pendingNotice.clear();
    }
    {   // the first update check a minute after start; a back-off after failures is kept
        Json st = readUpdateState();
        time_t now = std::time(nullptr), next = now + kFirstCheck;
        if (st.num("failures") > 0) next = std::max(next, static_cast<time_t>(st.num("next_check")));
        g.nextUpdateCheck = next;
    }
    g.sup.tick();
    SetTimer(hwnd, TIMER_SUPERVISE, 1000, nullptr);
    SetTimer(hwnd, TIMER_STATUS, 2000, nullptr);
    SetTimer(hwnd, TIMER_UPDATE, 30000, nullptr);
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
