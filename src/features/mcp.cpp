// Client MCP côté Windows : processus enfants sans fenêtre, un job par serveur (tué avec le dock), tubes stdin/stdout/stderr.
#include "features/mcp.h"

#include "common/util.h"
#include "web/bridge.h"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cwchar>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace mcp {
namespace {

constexpr size_t kMaxServers = 8;
constexpr size_t kMaxLine = 16u << 20; // une réponse plus grosse : le serveur est arrêté plutôt que de saturer la mémoire
constexpr size_t kMaxQueue = 64;       // messages en attente d'écriture ; au-delà, le serveur ne lit plus rien
constexpr size_t kErrTail = 3000;

struct Server {
    std::string id;
    long long gen = 0;
    HANDLE job = nullptr;
    HANDLE proc = nullptr;
    HANDLE inW = nullptr;
    std::mutex m; // protège queue, stderr et closed
    std::condition_variable cv;
    std::deque<std::string> queue;
    std::string stderrTail;
    bool closed = false; // plus d'écriture possible : le processus est fini ou arrêté

    ~Server() {
        if (inW) CloseHandle(inW);
        if (proc) CloseHandle(proc);
        if (job) CloseHandle(job); // KILL_ON_JOB_CLOSE : plus aucun processus orphelin
    }
    void Close() {
        {
            std::lock_guard lock(m);
            closed = true;
        }
        cv.notify_all();
    }
    void Kill() {
        Close();
        if (job) TerminateJobObject(job, 1);
    }
};

std::map<std::string, std::shared_ptr<Server>> g_servers;
std::mutex g_mutex;
long long g_gen = 0;

std::string Event(const Server& s, const std::string& ev, const std::string& extra = "") {
    return "{\"t\":\"mcp\",\"id\":" + util::Json(s.id) + ",\"gen\":" + std::to_string(s.gen) + ",\"ev\":\"" + ev + "\"" +
           (extra.empty() ? "" : "," + extra) + "}";
}

void Fail(const std::string& id, long long gen, const std::string& msg) {
    bridge::PostToPage("{\"t\":\"mcp\",\"id\":" + util::Json(id) + ",\"gen\":" + std::to_string(gen) + ",\"ev\":\"fail\",\"msg\":" +
                       util::Json(msg) + "}");
}

bool ValidId(const std::string& id) {
    if (id.empty() || id.size() > 40) return false;
    for (unsigned char c : id)
        if (!(isalnum(c) || c == '-' || c == '_')) return false;
    return true;
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

bool EndsWithNoCase(const std::wstring& s, const wchar_t* suffix) {
    size_t n = wcslen(suffix);
    return s.size() >= n && !_wcsicmp(s.c_str() + s.size() - n, suffix);
}

// « npx » → npx.cmd du PATH ; un chemin complet est pris tel quel. Les extensions de PATHEXT utiles, dans cet ordre.
std::wstring Resolve(const std::wstring& command) {
    wchar_t buf[MAX_PATH];
    const size_t slash = command.find_last_of(L"\\/"), dot = command.find_last_of(L'.');
    const bool hasExt = dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash);
    if (hasExt) {
        DWORD n = SearchPathW(nullptr, command.c_str(), nullptr, MAX_PATH, buf, nullptr);
        if (n && n < MAX_PATH) return buf;
    }
    for (const wchar_t* ext : {L".exe", L".com", L".cmd", L".bat"}) {
        DWORD n = SearchPathW(nullptr, command.c_str(), ext, MAX_PATH, buf, nullptr);
        if (n && n < MAX_PATH) return buf;
    }
    return {};
}

// Environnement hérité, sans les clés de Claude ; les variables du serveur remplacent celles qui portent le même nom.
std::vector<wchar_t> ChildEnvironment(const std::vector<std::string>& extra) {
    std::vector<std::wstring> own;
    for (const auto& e : extra) own.push_back(util::Widen(e));
    auto nameOf = [](const std::wstring& s) { return s.substr(0, s.find(L'=', 1)); };
    std::vector<wchar_t> env;
    LPWCH block = GetEnvironmentStringsW();
    for (LPWCH p = block; *p; p += wcslen(p) + 1) {
        if (!_wcsnicmp(p, L"ANTHROPIC_API_KEY=", 18) || !_wcsnicmp(p, L"ANTHROPIC_AUTH_TOKEN=", 21) ||
            !_wcsnicmp(p, L"WEBVIEW2_", 9))
            continue;
        bool replaced = false;
        std::wstring n = nameOf(p);
        for (const auto& o : own)
            if (!_wcsicmp(nameOf(o).c_str(), n.c_str())) replaced = true;
        if (!replaced) env.insert(env.end(), p, p + wcslen(p) + 1);
    }
    FreeEnvironmentStringsW(block);
    for (const auto& o : own) env.insert(env.end(), o.c_str(), o.c_str() + o.size() + 1);
    env.push_back(L'\0');
    return env;
}

std::wstring WorkDir() {
    std::wstring dir = util::LocalDataDir() + L"\\mcp";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

void WriterLoop(std::shared_ptr<Server> s) {
    for (;;) {
        std::string line;
        {
            std::unique_lock lock(s->m);
            s->cv.wait(lock, [&] { return s->closed || !s->queue.empty(); });
            if (s->closed) return;
            line = std::move(s->queue.front());
            s->queue.pop_front();
        }
        line += '\n';
        size_t off = 0;
        while (off < line.size()) {
            DWORD w = 0;
            if (!WriteFile(s->inW, line.data() + off, (DWORD)(line.size() - off), &w, nullptr) || !w) return;
            off += w;
        }
    }
}

void ErrReader(std::shared_ptr<Server> s, HANDLE errR) {
    char buf[2048];
    DWORD n = 0;
    while (ReadFile(errR, buf, sizeof buf, &n, nullptr) && n) {
        std::lock_guard lock(s->m);
        s->stderrTail.append(buf, n);
        if (s->stderrTail.size() > kErrTail * 2) s->stderrTail.erase(0, s->stderrTail.size() - kErrTail);
    }
    CloseHandle(errR);
}

void Reader(std::shared_ptr<Server> s, HANDLE outR, HANDLE errR) {
    std::thread writer(WriterLoop, s);
    std::thread err(ErrReader, s, errR);
    std::string pending;
    char buf[16384];
    DWORD n = 0;
    while (ReadFile(outR, buf, sizeof buf, &n, nullptr) && n) {
        pending.append(buf, n);
        size_t from = 0, nl;
        while ((nl = pending.find('\n', from)) != std::string::npos) {
            std::string line = pending.substr(from, nl - from);
            from = nl + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (!line.empty()) bridge::PostToPage(Event(*s, "line", "\"data\":" + util::Json(line)));
        }
        pending.erase(0, from);
        if (pending.size() > kMaxLine) {
            s->Kill();
            break;
        }
    }
    CloseHandle(outR);
    s->Close();
    WaitForSingleObject(s->proc, 3000);
    DWORD code = 0;
    GetExitCodeProcess(s->proc, &code);
    if (code == STILL_ACTIVE) { // a fermé stdout mais tourne encore : on l'arrête
        TerminateJobObject(s->job, 1);
        code = 1;
    }
    writer.join();
    err.join();
    std::string tail;
    {
        std::lock_guard lock(s->m);
        tail = util::Tail(s->stderrTail, kErrTail);
    }
    {
        std::lock_guard lock(g_mutex);
        auto it = g_servers.find(s->id);
        if (it != g_servers.end() && it->second == s) g_servers.erase(it);
    }
    bridge::PostToPage(Event(*s, "exit", "\"code\":" + std::to_string((int)code) + ",\"stderr\":" + util::Json(tail)));
}

} // namespace

void Start(const std::string& payload) {
    auto f = util::Split(payload, '\x1f', 4);
    if (f.size() != 4 || !ValidId(f[0])) return;
    const std::string id = f[0];
    std::vector<std::string> args = f[2].empty() ? std::vector<std::string>{} : util::Split(f[2], '\x1e');
    std::vector<std::string> envs = f[3].empty() ? std::vector<std::string>{} : util::Split(f[3], '\x1e');
    for (const auto& e : envs)
        if (e.find('=') == std::string::npos || e[0] == '=') return;

    long long gen;
    std::shared_ptr<Server> old;
    {
        std::lock_guard lock(g_mutex);
        auto it = g_servers.find(id);
        if (it != g_servers.end()) {
            old = it->second;
            g_servers.erase(it);
        } else if (g_servers.size() >= kMaxServers) {
            Fail(id, 0, "Trop de serveurs MCP lancés à la fois (8 au plus).");
            return;
        }
        gen = ++g_gen;
    }
    if (old) old->Kill();

    const std::wstring command = util::Widen(f[1]);
    if (command.empty()) { Fail(id, gen, "Aucune commande."); return; }
    std::wstring exe = Resolve(command);
    if (exe.empty()) { Fail(id, gen, "Commande introuvable dans le PATH : " + f[1]); return; }

    // Un .cmd/.bat passe par cmd.exe : on refuse les caractères que cmd interpréterait (aucune injection possible).
    std::wstring appName = exe, cmdline;
    if (EndsWithNoCase(exe, L".cmd") || EndsWithNoCase(exe, L".bat")) {
        for (const auto& a : args)
            if (a.find_first_of("&|<>^%!\"\r\n") != std::string::npos) {
                Fail(id, gen, "Un argument contient un caractère non pris en charge pour une commande .cmd (& | < > ^ % ! \").");
                return;
            }
        wchar_t sysDir[MAX_PATH];
        GetSystemDirectoryW(sysDir, MAX_PATH);
        appName = std::wstring(sysDir) + L"\\cmd.exe";
        cmdline = L"\"" + appName + L"\" /d /s /c \"\"" + exe + L"\"";
        for (const auto& a : args) cmdline += L" " + QuoteArg(util::Widen(a));
        cmdline += L"\"";
    } else {
        cmdline = QuoteArg(exe);
        for (const auto& a : args) cmdline += L" " + QuoteArg(util::Widen(a));
    }
    std::vector<wchar_t> cmdBuf(cmdline.begin(), cmdline.end());
    cmdBuf.push_back(L'\0');
    auto env = ChildEnvironment(envs);
    std::wstring cwd = WorkDir();

    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr, errR = nullptr, errW = nullptr;
    if (!CreatePipe(&inR, &inW, &sa, 0) || !CreatePipe(&outR, &outW, &sa, 0) || !CreatePipe(&errR, &errW, &sa, 0)) {
        Fail(id, gen, "Impossible de créer les tubes.");
        return;
    }
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inR;
    si.hStdOutput = outW;
    si.hStdError = errW;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(appName.c_str(), cmdBuf.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED, env.data(), cwd.c_str(), &si, &pi);
    const DWORD lastError = GetLastError();
    CloseHandle(inR);
    CloseHandle(outW);
    CloseHandle(errW);
    if (!ok) {
        CloseHandle(inW);
        CloseHandle(outR);
        CloseHandle(errR);
        Fail(id, gen, "Impossible de lancer la commande (code " + std::to_string(lastError) + ").");
        return;
    }

    auto s = std::make_shared<Server>();
    s->id = id;
    s->gen = gen;
    s->proc = pi.hProcess;
    s->inW = inW;
    s->job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(s->job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
    AssignProcessToJobObject(s->job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    {
        std::lock_guard lock(g_mutex);
        g_servers[id] = s;
    }
    bridge::PostToPage(Event(*s, "started"));
    std::thread(Reader, s, outR, errR).detach();
}

void Send(const std::string& payload) {
    size_t nl = payload.find('\n');
    if (nl == std::string::npos) return;
    std::string id = payload.substr(0, nl);
    std::string line = payload.substr(nl + 1);
    if (line.empty() || line.find('\n') != std::string::npos) return; // une ligne exactement : c'est le format du transport stdio
    std::shared_ptr<Server> s;
    {
        std::lock_guard lock(g_mutex);
        auto it = g_servers.find(id);
        if (it == g_servers.end()) return;
        s = it->second;
    }
    {
        std::lock_guard lock(s->m);
        if (s->closed) return;
        if (s->queue.size() >= kMaxQueue) return;
        s->queue.push_back(std::move(line));
    }
    s->cv.notify_one();
}

void Stop(const std::string& id) {
    std::vector<std::shared_ptr<Server>> victims;
    {
        std::lock_guard lock(g_mutex);
        for (auto it = g_servers.begin(); it != g_servers.end();) {
            if (id.empty() || it->first == id) {
                victims.push_back(it->second);
                it = g_servers.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& s : victims) s->Kill();
}

} // namespace mcp
