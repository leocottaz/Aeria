// Client MCP côté Linux : fork/exec, un groupe de processus par serveur (tué avec lui), tubes stdin/stdout/stderr.
#include "features/mcp.h"

#include "common/util.h"
#include "web/bridge.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

extern char** environ;

namespace mcp {
namespace {

constexpr size_t kMaxServers = 8;
constexpr size_t kMaxLine = 16u << 20;
constexpr size_t kMaxQueue = 64;
constexpr size_t kErrTail = 3000;

struct Server : std::enable_shared_from_this<Server> {
    std::string id;
    long long gen = 0;
    pid_t pid = 0;
    int inFd = -1;
    std::atomic<bool> alive{true};
    std::mutex m; // protège queue, stderr et closed
    std::condition_variable cv;
    std::deque<std::string> queue;
    std::string stderrTail;
    bool closed = false;

    ~Server() {
        if (inFd >= 0) close(inFd);
    }
    void Close() {
        {
            std::lock_guard lock(m);
            closed = true;
        }
        cv.notify_all();
    }
    // Arrêt poli (SIGTERM, comme à la fermeture d'un terminal), puis SIGKILL du groupe s'il tarde.
    void Kill() {
        Close();
        if (!alive) return;
        kill(-pid, SIGTERM);
        std::thread([self = shared_from_this()] { // le serveur reste en vie (shared_ptr) jusqu'à la fin de l'attente
            for (int i = 0; i < 20 && self->alive; ++i) usleep(100000);
            if (self->alive) kill(-self->pid, SIGKILL);
        }).detach();
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

// Un chemin est pris tel quel ; sinon on cherche dans le PATH (comme un shell, sans en lancer un).
std::string Resolve(const std::string& command) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (command.find('/') != std::string::npos) return access(command.c_str(), X_OK) == 0 && !fs::is_directory(command, ec) ? command : "";
    if (const char* path = std::getenv("PATH")) {
        for (const auto& dir : util::Split(path, ':')) {
            fs::path p = fs::path(dir.empty() ? "." : dir) / command;
            if (access(p.c_str(), X_OK) == 0 && !fs::is_directory(p, ec)) return p.string();
        }
    }
    return {};
}

std::vector<std::string> ChildEnvironment(const std::vector<std::string>& extra) {
    auto nameOf = [](const std::string& s) { return s.substr(0, s.find('=')); };
    std::vector<std::string> env;
    for (char** e = environ; e && *e; ++e) {
        if (!strncmp(*e, "ANTHROPIC_API_KEY=", 18) || !strncmp(*e, "ANTHROPIC_AUTH_TOKEN=", 21)) continue;
        bool replaced = false;
        for (const auto& o : extra)
            if (nameOf(o) == nameOf(*e)) replaced = true;
        if (!replaced) env.emplace_back(*e);
    }
    env.insert(env.end(), extra.begin(), extra.end());
    return env;
}

std::string WorkDir() {
    std::string dir = util::DataDir() + "/mcp";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
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
            ssize_t w = write(s->inFd, line.data() + off, line.size() - off);
            if (w <= 0) return;
            off += (size_t)w;
        }
    }
}

void ErrReader(std::shared_ptr<Server> s, int errFd) {
    char buf[2048];
    ssize_t n;
    while ((n = read(errFd, buf, sizeof buf)) > 0) {
        std::lock_guard lock(s->m);
        s->stderrTail.append(buf, (size_t)n);
        if (s->stderrTail.size() > kErrTail * 2) s->stderrTail.erase(0, s->stderrTail.size() - kErrTail);
    }
    close(errFd);
}

void Reader(std::shared_ptr<Server> s, int outFd, int errFd) {
    std::thread writer(WriterLoop, s);
    std::thread err(ErrReader, s, errFd);
    std::string pending;
    char buf[16384];
    ssize_t n;
    while ((n = read(outFd, buf, sizeof buf)) > 0) {
        pending.append(buf, (size_t)n);
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
    close(outFd);
    s->Close();
    int status = 0;
    pid_t r = 0;
    for (int i = 0; i < 30 && !(r = waitpid(s->pid, &status, WNOHANG)); ++i) usleep(100000); // a fermé stdout : on lui laisse 3 s
    if (r == 0) {
        kill(-s->pid, SIGKILL);
        waitpid(s->pid, &status, 0);
    }
    s->alive = false;
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
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    bridge::PostToPage(Event(*s, "exit", "\"code\":" + std::to_string(code) + ",\"stderr\":" + util::Json(tail)));
}

// À la fermeture du dock il n'y a plus de temps pour l'arrêt poli : on tue les groupes tout de suite.
void KillAllAtExit() {
    std::lock_guard lock(g_mutex);
    for (auto& [id, s] : g_servers)
        if (s->alive) kill(-s->pid, SIGKILL);
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

    static std::once_flag once;
    std::call_once(once, [] {
        signal(SIGPIPE, SIG_IGN); // écrire dans un serveur mort ne doit pas tuer le dock
        std::atexit(KillAllAtExit);
    });

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

    if (f[1].empty()) { Fail(id, gen, "Aucune commande."); return; }
    std::string exe = Resolve(f[1]);
    if (exe.empty()) { Fail(id, gen, "Commande introuvable dans le PATH : " + f[1]); return; }

    // Tout ce que l'enfant utilise est préparé avant le fork (pas d'allocation entre fork et exec).
    std::vector<std::string> argStrings = {exe};
    argStrings.insert(argStrings.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& a : argStrings) argv.push_back(a.data());
    argv.push_back(nullptr);
    std::vector<std::string> envStrings = ChildEnvironment(envs);
    std::vector<char*> envp;
    for (auto& e : envStrings) envp.push_back(e.data());
    envp.push_back(nullptr);
    std::string cwd = WorkDir();

    int in[2], out[2], err[2];
    if (pipe2(in, O_CLOEXEC) || pipe2(out, O_CLOEXEC) || pipe2(err, O_CLOEXEC)) {
        Fail(id, gen, "Impossible de créer les tubes.");
        return;
    }
    pid_t pid = fork();
    if (pid < 0) {
        for (int fd : {in[0], in[1], out[0], out[1], err[0], err[1]}) close(fd);
        Fail(id, gen, "Impossible de lancer la commande.");
        return;
    }
    if (pid == 0) {
        setpgid(0, 0);
        dup2(in[0], 0), dup2(out[1], 1), dup2(err[1], 2); // dup2 retire O_CLOEXEC sur 0, 1 et 2 ; les autres se ferment à l'exec
        if (chdir(cwd.c_str())) _exit(126);
        execve(exe.c_str(), argv.data(), envp.data());
        _exit(127);
    }
    setpgid(pid, pid);
    close(in[0]), close(out[1]), close(err[1]);

    auto s = std::make_shared<Server>();
    s->id = id;
    s->gen = gen;
    s->pid = pid;
    s->inFd = in[1];
    {
        std::lock_guard lock(g_mutex);
        g_servers[id] = s;
    }
    bridge::PostToPage(Event(*s, "started"));
    std::thread(Reader, s, out[0], err[0]).detach();
}

void Send(const std::string& payload) {
    size_t nl = payload.find('\n');
    if (nl == std::string::npos) return;
    std::string id = payload.substr(0, nl);
    std::string line = payload.substr(nl + 1);
    if (line.empty() || line.find('\n') != std::string::npos) return;
    std::shared_ptr<Server> s;
    {
        std::lock_guard lock(g_mutex);
        auto it = g_servers.find(id);
        if (it == g_servers.end()) return;
        s = it->second;
    }
    {
        std::lock_guard lock(s->m);
        if (s->closed || s->queue.size() >= kMaxQueue) return;
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
