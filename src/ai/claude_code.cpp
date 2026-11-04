#include "ai/claude_code.h"

#include "common/util.h"
#include "web/bridge.h"

#include <windows.h>

#include <atomic>
#include <cctype>
#include <cwchar>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace claude_code {
namespace {

struct ProcJob {
    std::string id;
    std::mutex mutex; // protège job
    HANDLE job = nullptr;
    std::atomic<bool> cancelled{false};
};

std::map<std::string, std::shared_ptr<ProcJob>> g_jobs;
std::mutex g_jobsMutex;

std::wstring FindExe() {
    const std::wstring candidates[] = {
        util::KnownFolder(FOLDERID_Profile) + L"\\.local\\bin\\claude.exe",            // installateur officiel
        util::KnownFolder(FOLDERID_LocalAppData) + L"\\Microsoft\\WinGet\\Links\\claude.exe", // winget
    };
    for (const auto& c : candidates)
        if (util::FileExists(c)) return c;
    wchar_t buf[MAX_PATH];
    if (SearchPathW(nullptr, L"claude.exe", nullptr, MAX_PATH, buf, nullptr)) return buf;
    return {};
}

// Règles de citation de CommandLineToArgvW.
std::wstring QuoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
    std::wstring o = L"\"";
    for (size_t i = 0;; ++i) {
        size_t slashes = 0;
        while (i < a.size() && a[i] == L'\\') ++i, ++slashes;
        if (i == a.size()) {
            o.append(slashes * 2, L'\\');
            break;
        }
        if (a[i] == L'"') o.append(slashes * 2 + 1, L'\\');
        else o.append(slashes, L'\\');
        o += a[i];
    }
    return o + L"\"";
}

// Environnement hérité, sans clé API : Claude Code doit utiliser le compte connecté (abonnement).
std::vector<wchar_t> ChildEnvironment() {
    std::vector<wchar_t> env;
    LPWCH block = GetEnvironmentStringsW();
    for (LPWCH p = block; *p; p += wcslen(p) + 1) {
        if (!_wcsnicmp(p, L"ANTHROPIC_API_KEY=", 18) || !_wcsnicmp(p, L"ANTHROPIC_AUTH_TOKEN=", 21) ||
            !_wcsnicmp(p, L"WEBVIEW2_", 9))
            continue;
        env.insert(env.end(), p, p + wcslen(p) + 1);
    }
    FreeEnvironmentStringsW(block);
    env.push_back(L'\0');
    return env;
}

// Dossier vide dédié : pas de CLAUDE.md ni de réglages de projet chargés.
std::wstring WorkDir() {
    std::wstring dir = util::LocalDataDir() + L"\\claude-chat";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::string ReadAll(HANDLE h) {
    std::string out;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(h, buf, sizeof buf, &n, nullptr) && n) out.append(buf, n);
    return out;
}

// Lance claude.exe ; onStdout reçoit la sortie au fil de l'eau. Retourne le code de sortie (-1 si échec).
int RunProcess(ProcJob& pj, const std::wstring& exe, const std::vector<std::wstring>& args, const std::string& input,
               const std::function<void(const char*, size_t)>& onStdout, std::string& errText, DWORD timeoutMs) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE inR, inW, outR, outW, errR, errW;
    if (!CreatePipe(&inR, &inW, &sa, 0)) return -1;
    CreatePipe(&outR, &outW, &sa, 0);
    CreatePipe(&errR, &errW, &sa, 0);
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);

    std::wstring cmd = QuoteArg(exe);
    for (const auto& a : args) cmd += L" " + QuoteArg(a);
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');
    auto env = ChildEnvironment();
    std::wstring cwd = WorkDir();

    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inR;
    si.hStdOutput = outW;
    si.hStdError = errW;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(exe.c_str(), cmdBuf.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED, env.data(), cwd.c_str(),
                             &si, &pi);
    CloseHandle(inR);
    CloseHandle(outW);
    CloseHandle(errW);
    if (!ok) {
        errText = "Impossible de lancer Claude Code (code " + std::to_string(GetLastError()) + ").";
        CloseHandle(inW);
        CloseHandle(outR);
        CloseHandle(errR);
        return -1;
    }

    // Le job tue claude.exe (et ses enfants) si on annule ou si le dock se ferme.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
    AssignProcessToJobObject(job, pi.hProcess);
    {
        std::lock_guard lock(pj.mutex);
        pj.job = job;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    if (pj.cancelled) TerminateJobObject(job, 1);

    DWORD written = 0;
    if (!input.empty()) WriteFile(inW, input.data(), (DWORD)input.size(), &written, nullptr);
    CloseHandle(inW);

    std::thread errReader([&] { errText = ReadAll(errR); });
    std::thread killer;
    if (timeoutMs != INFINITE) {
        killer = std::thread([job, timeoutMs, h = pi.hProcess] {
            if (WaitForSingleObject(h, timeoutMs) == WAIT_TIMEOUT) TerminateJobObject(job, 1);
        });
    }
    char buf[8192];
    DWORD n = 0;
    while (ReadFile(outR, buf, sizeof buf, &n, nullptr) && n) onStdout(buf, n);
    WaitForSingleObject(pi.hProcess, INFINITE);
    errReader.join();
    if (killer.joinable()) killer.join();
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(outR);
    CloseHandle(errR);
    {
        std::lock_guard lock(pj.mutex);
        pj.job = nullptr;
    }
    CloseHandle(job);
    return (int)code;
}

void RunChat(std::shared_ptr<ProcJob> pj, std::vector<std::string> f) {
    const std::string id = util::Json(pj->id);
    std::wstring exe = FindExe();
    if (exe.empty()) {
        bridge::PostToPage("{\"t\":\"error\",\"id\":" + id + ",\"status\":-3,\"body\":\"Claude Code est introuvable.\"}");
    } else {
        // Aucun outil (fichiers, commandes, MCP) : Claude Code sert uniquement à discuter. Seule exception, voulue par la
        // page (champ « outils » = web) : la recherche sur internet, sans lecture de page ni accès au PC.
        const bool web = f[4] == "web";
        std::vector<std::wstring> args = {L"-p", L"--output-format", L"stream-json", L"--verbose",
                                          L"--include-partial-messages", L"--tools", web ? L"WebSearch" : L"", L"--disallowedTools",
                                          L"mcp__*", L"--strict-mcp-config", L"--disable-slash-commands",
                                          L"--system-prompt", util::Widen(f[5])};
        if (web) args.insert(args.end(), {L"--allowedTools", L"WebSearch"});
        if (!f[1].empty() && f[1] != "default") args.insert(args.end(), {L"--model", util::Widen(f[1])});
        if (!f[2].empty()) args.insert(args.end(), {L"--effort", util::Widen(f[2])});
        if (!f[3].empty()) args.insert(args.end(), {L"--resume", util::Widen(f[3])});

        std::string pending, err;
        int code = RunProcess(*pj, exe, args, f[6], [&](const char* d, size_t n) {
            pending.append(d, n);
            size_t cut = pending.rfind('\n');
            if (cut != std::string::npos) {
                bridge::PostToPage("{\"t\":\"chunk\",\"id\":" + id + ",\"data\":" + util::Json(pending.substr(0, cut + 1)) + "}");
                pending.erase(0, cut + 1);
            }
        }, err, INFINITE);
        if (!pending.empty())
            bridge::PostToPage("{\"t\":\"chunk\",\"id\":" + id + ",\"data\":" + util::Json(pending + "\n") + "}");
        bridge::PostToPage("{\"t\":\"done\",\"id\":" + id + ",\"cancelled\":" + (pj->cancelled ? "true" : "false") +
                           ",\"exit\":" + std::to_string(code) + ",\"stderr\":" + util::Json(util::Tail(err, 1500)) + "}");
    }
    std::lock_guard lock(g_jobsMutex);
    g_jobs.erase(pj->id);
}

} // namespace

void StartChat(const std::string& payload) {
    auto f = util::Split(payload, '\x1f', 7);
    if (f.size() != 7 || f[0].empty()) return;
    auto pj = std::make_shared<ProcJob>();
    pj->id = f[0];
    {
        std::lock_guard lock(g_jobsMutex);
        g_jobs[pj->id] = pj;
    }
    std::thread(RunChat, pj, std::move(f)).detach();
}

bool Cancel(const std::string& id) {
    std::shared_ptr<ProcJob> pj;
    {
        std::lock_guard lock(g_jobsMutex);
        auto it = g_jobs.find(id);
        if (it == g_jobs.end()) return false;
        pj = it->second;
    }
    pj->cancelled = true;
    std::lock_guard lock(pj->mutex);
    if (pj->job) TerminateJobObject(pj->job, 1);
    return true;
}

void CheckInstalled() {
    std::thread([] {
        std::wstring exe = FindExe();
        std::string version, err;
        if (!exe.empty()) {
            ProcJob pj;
            RunProcess(pj, exe, {L"--version"}, "", [&](const char* d, size_t n) { version.append(d, n); }, err, 20000);
            while (!version.empty() && std::isspace((unsigned char)version.back())) version.pop_back();
        }
        bridge::PostToPage(std::string("{\"t\":\"ccstatus\",\"found\":") + (exe.empty() ? "false" : "true") +
                           ",\"path\":" + util::Json(util::Narrow(exe)) + ",\"version\":" + util::Json(version) + "}");
    }).detach();
}

void OpenLogin() {
    std::wstring exe = FindExe();
    if (exe.empty()) return;
    std::wstring cmd = QuoteArg(exe);
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');
    auto env = ChildEnvironment();
    std::wstring cwd = WorkDir();
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE | CREATE_UNICODE_ENVIRONMENT,
                       env.data(), cwd.c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

} // namespace claude_code
