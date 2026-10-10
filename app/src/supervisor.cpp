// The supervised Python process (the pull loop). The app starts
//     <install>\python\python.exe -I -X utf8 <install>\bootstrap\cobalt_bootstrap.py --pull
// in %LOCALAPPDATA%\CobaltConnector with a minimal environment built here - nothing inherited from the user's own
// environment beyond what Windows itself needs, so no model key or Python setting of theirs leaks in.
// The partner key is read from its DPAPI file and passed through the child's environment only. The Anthropic key
// is read and passed (as ANTHROPIC_API_KEY, with COBALT_LOCAL_MODEL=1) only when the owner chose "Run my model work
// on this machine" and a key is stored; with that option off the key is not even read, and the child's environment
// has no ANTHROPIC_API_KEY at all (it is built from scratch below, never inherited). The key itself goes to
// Anthropic only. The child and every process it starts (its process pools) run in a job object that is closed with
// the app, so Quit leaves nothing behind. COBALT_APP_VERSION and COBALT_MACHINE_NAME (when the
// owner chose a name) are sent by the bootstrap in its hello. A loop that exits is started
// again with backoff: 2 s after a clean exit (the loop stops itself when the fleet moves to new code,
// and the bootstrap then fetches that build), doubling to 60 s while it keeps failing.
#include "app.hpp"

#include <algorithm>
#include <vector>

namespace cw {

namespace {

std::wstring envVar(const wchar_t* name) {
    wchar_t buf[4096];
    DWORD n = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
    return n > 0 && n < std::size(buf) ? std::wstring(buf, n) : std::wstring();
}

// What Windows and Python need to run, and nothing else of the user's environment.
const wchar_t* const kPassThrough[] = {
    L"SystemRoot", L"SystemDrive", L"windir", L"ComSpec", L"TEMP", L"TMP", L"USERPROFILE", L"USERNAME",
    L"USERDOMAIN", L"COMPUTERNAME", L"LOCALAPPDATA", L"APPDATA", L"HOMEDRIVE", L"HOMEPATH", L"PROGRAMDATA",
    L"NUMBER_OF_PROCESSORS", L"PROCESSOR_ARCHITECTURE", L"PROCESSOR_IDENTIFIER", L"PROCESSOR_LEVEL",
    L"PROCESSOR_REVISION", L"OS", L"PATHEXT",
};

// `anthropic` is set only when the owner chose model work on this machine (Supervisor::start); the check here is the
// second guard, so the key never reaches the child with the option off.
std::vector<wchar_t> buildEnvironment(const std::string& partner, const std::optional<std::string>& anthropic,
                                      bool localModel) {
    std::vector<std::pair<std::wstring, std::wstring>> vars;
    for (const wchar_t* name : kPassThrough) {
        std::wstring v = envVar(name);
        if (!v.empty()) vars.emplace_back(name, v);
    }
    std::wstring sysroot = envVar(L"SystemRoot");
    if (sysroot.empty()) sysroot = L"C:\\Windows";
    std::wstring py = exeDir() + L"\\python";
    vars.emplace_back(L"PATH", py + L";" + sysroot + L"\\System32;" + sysroot);
    vars.emplace_back(L"COBALT_CONTRIB_HOME", homeDir());
    vars.emplace_back(L"COBALT_FLEET_URL", widen(relayUrl()));
    vars.emplace_back(L"COBALT_CONTRIB_KEY", widen(partner));
    if (anthropic && localModel) {
        vars.emplace_back(L"ANTHROPIC_API_KEY", widen(*anthropic));
        vars.emplace_back(L"COBALT_LOCAL_MODEL", L"1");
    }
    if (std::string test = anthropicTestUrl(); !test.empty()) vars.emplace_back(L"COBALT_ANTHROPIC_URL", widen(test));
    vars.emplace_back(L"NUMBA_CACHE_DIR", homeDir() + L"\\numba");      // numba's compiled cache, out of the install
    vars.emplace_back(L"COBALT_APP_VERSION", COBALT_CONNECTOR_VERSION_W);
    // always set, empty when no name is chosen, so clearing the name reaches the server too
    std::string name = machineName();
    vars.emplace_back(L"COBALT_MACHINE_NAME", validMachineName(name) ? widen(name) : std::wstring());
    std::sort(vars.begin(), vars.end(), [](const auto& a, const auto& b) {
        return CompareStringOrdinal(a.first.c_str(), -1, b.first.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    std::vector<wchar_t> block;
    for (auto& kv : vars) {
        std::wstring entry = kv.first + L"=" + kv.second;
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
        SecureZeroMemory(entry.data(), entry.size() * sizeof(wchar_t));
        SecureZeroMemory(kv.second.data(), kv.second.size() * sizeof(wchar_t));
    }
    block.push_back(L'\0');
    return block;
}

HANDLE openChildLog() {
    std::wstring path = logsDir() + L"\\python.log";
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d) && (d.nFileSizeHigh || d.nFileSizeLow > 2000000)) {
        std::wstring old = path + L".1";
        MoveFileExW(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    return CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa,
                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

}  // namespace

Supervisor::Supervisor() {
    job_ = CreateJobObjectW(nullptr, nullptr);
    if (job_) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &info, sizeof info);
    }
}

Supervisor::~Supervisor() {
    stop();
    if (job_) CloseHandle(job_);
}

double Supervisor::runningSeconds() const {
    return process_ ? static_cast<double>(GetTickCount64() - startedAt_) / 1000.0 : 0.0;
}

bool Supervisor::start() {
    if (!fileExists(pythonExe())) {
        problem_ = L"The embedded Python is missing (" + pythonExe() + L"): reinstall Cobalt Connector.";
        return false;
    }
    if (!fileExists(bootstrapScript())) {
        problem_ = L"The bootstrap is missing (" + bootstrapScript() + L"): reinstall Cobalt Connector.";
        return false;
    }
    std::optional<std::string> partner = loadSecret(L"partner");
    if (!partner) {
        problem_ = L"No partner key is stored. Run the Cobalt Connector installer again and enter your partner key.";
        return false;
    }
    // the Anthropic key is read only for model work on this machine: with the option off it stays in its DPAPI file
    const bool localModel = localModelChosen();
    std::optional<std::string> anthropic = localModel ? loadSecret(L"anthropic") : std::nullopt;
    machineId();
    problem_.clear();

    std::vector<wchar_t> env = buildEnvironment(*partner, anthropic, localModel);
    SecureZeroMemory(partner->data(), partner->size());
    if (anthropic) SecureZeroMemory(anthropic->data(), anthropic->size());

    HANDLE log = openChildLog();
    std::wstring cmd = L"\"" + pythonExe() + L"\" -I -X utf8 \"" + bootstrapScript() + L"\" --pull";
    std::wstring cwd = homeDir();

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nullptr;
    si.StartupInfo.hStdOutput = log;
    si.StartupInfo.hStdError = log;
    // inherit the log handle and nothing else
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<BYTE> attrBuf(attrSize);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    bool haveAttrs = log != INVALID_HANDLE_VALUE && InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize) &&
                     UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &log, sizeof log, nullptr, nullptr);
    if (haveAttrs) si.lpAttributeList = attrs;
    else { si.StartupInfo.hStdOutput = nullptr; si.StartupInfo.hStdError = nullptr; }

    PROCESS_INFORMATION pi{};
    DWORD flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS |
                  (haveAttrs ? EXTENDED_STARTUPINFO_PRESENT : 0);
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, haveAttrs ? TRUE : FALSE, flags, env.data(),
                             cwd.c_str(), &si.StartupInfo, &pi);
    DWORD err = GetLastError();
    SecureZeroMemory(env.data(), env.size() * sizeof(wchar_t));
    if (haveAttrs) DeleteProcThreadAttributeList(attrs);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (!ok) {
        problem_ = L"Cobalt Connector could not start its work process (error " + std::to_wstring(err) + L").";
        logLine(problem_);
        return false;
    }
    if (job_) AssignProcessToJobObject(job_, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    startedAt_ = GetTickCount64();
    logLine(L"pull loop started (pid " + std::to_wstring(pi.dwProcessId) + L")" +
            (anthropic && localModel ? L", model work on this machine" : L""));
    return true;
}

void Supervisor::tick() {
    ULONGLONG now = GetTickCount64();
    if (process_) {
        if (WaitForSingleObject(process_, 0) != WAIT_OBJECT_0) return;
        DWORD code = 0;
        GetExitCodeProcess(process_, &code);
        double ran = runningSeconds();
        CloseHandle(process_);
        process_ = nullptr;
        if (ran > 300.0) failures_ = 0;              // it worked a good while: a fresh start
        ULONGLONG delay;
        if (code == 0) {
            delay = 2000;                             // a clean stop: usually a new build was unpacked
        } else {
            ++failures_;
            delay = std::min<ULONGLONG>(60000, 1000ull << std::min(failures_, 6));
        }
        nextStart_ = now + delay;
        logLine(L"pull loop exited with code " + std::to_wstring(code) + L" after " +
                std::to_wstring(static_cast<long long>(ran)) + L" s; starting again in " +
                std::to_wstring(delay / 1000) + L" s");
        return;
    }
    if (now < nextStart_) return;
    if (!start()) {
        ++failures_;
        nextStart_ = now + 30000;                     // a missing key or file: look again, quietly
    }
}

void Supervisor::restart() {
    stop();
    failures_ = 0;
    nextStart_ = 0;
    tick();
}

void Supervisor::stop() {
    if (job_) TerminateJobObject(job_, 0);
    if (process_) {
        WaitForSingleObject(process_, 5000);
        CloseHandle(process_);
        process_ = nullptr;
        logLine(L"pull loop stopped");
    }
}

}  // namespace cw
