// Mode « Abonnement Pro » sous Linux : `claude -p` lancé par fork/exec, sortie stream-json relayée.
#include "ai/claude_code.h"

#include "common/util.h"
#include "web/bridge.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

extern char** environ;

namespace claude_code {
namespace {

struct ProcJob {
    std::string id;
    std::atomic<pid_t> pid{0};
    std::atomic<bool> cancelled{false};
};

std::map<std::string, std::shared_ptr<ProcJob>> g_jobs;
std::mutex g_jobsMutex;

std::string FindExe() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path local = fs::path(util::HomeDir()) / ".local" / "bin" / "claude"; // installateur officiel
    if (fs::exists(local, ec)) return local.string();
    if (const char* path = std::getenv("PATH")) {
        for (const auto& dir : util::Split(path, ':')) {
            fs::path p = fs::path(dir.empty() ? "." : dir) / "claude";
            if (access(p.c_str(), X_OK) == 0 && !fs::is_directory(p, ec)) return p.string();
        }
    }
    return {};
}

// Environnement hérité, sans clé API : Claude Code doit utiliser le compte connecté (abonnement).
std::vector<std::string> ChildEnvironment() {
    std::vector<std::string> env;
    for (char** e = environ; e && *e; ++e) {
        if (!strncmp(*e, "ANTHROPIC_API_KEY=", 18) || !strncmp(*e, "ANTHROPIC_AUTH_TOKEN=", 21)) continue;
        env.emplace_back(*e);
    }
    return env;
}

// Dossier vide dédié : pas de CLAUDE.md ni de réglages de projet chargés.
std::string WorkDir() {
    std::string dir = util::DataDir() + "/claude-chat";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::string ReadAll(int fd) {
    std::string out;
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) out.append(buf, (size_t)n);
    return out;
}

// Lance claude ; onStdout reçoit la sortie au fil de l'eau. Retourne le code de sortie (-1 si échec).
int RunProcess(ProcJob& pj, const std::string& exe, const std::vector<std::string>& args, const std::string& input,
               const std::function<void(const char*, size_t)>& onStdout, std::string& errText, int timeoutSec) {
    int in[2], out[2], err[2];
    if (pipe(in) || pipe(out) || pipe(err)) {
        errText = "Impossible de créer les tubes.";
        return -1;
    }
    std::vector<std::string> envStrings = ChildEnvironment();
    std::vector<char*> envp, argv;
    for (auto& s : envStrings) envp.push_back(s.data());
    envp.push_back(nullptr);
    std::vector<std::string> argStrings = {exe};
    argStrings.insert(argStrings.end(), args.begin(), args.end());
    for (auto& s : argStrings) argv.push_back(s.data());
    argv.push_back(nullptr);
    std::string cwd = WorkDir();

    pid_t pid = fork();
    if (pid < 0) {
        errText = "Impossible de lancer Claude Code.";
        for (int fd : {in[0], in[1], out[0], out[1], err[0], err[1]}) close(fd);
        return -1;
    }
    if (pid == 0) {
        setpgid(0, 0); // son propre groupe : on tue aussi les enfants à l'annulation
        dup2(in[0], 0), dup2(out[1], 1), dup2(err[1], 2);
        for (int fd : {in[0], in[1], out[0], out[1], err[0], err[1]}) close(fd);
        if (chdir(cwd.c_str())) _exit(126);
        execve(exe.c_str(), argv.data(), envp.data());
        _exit(127);
    }
    setpgid(pid, pid);
    close(in[0]), close(out[1]), close(err[1]);
    pj.pid = pid;
    if (pj.cancelled) kill(-pid, SIGKILL);

    if (!input.empty()) {
        signal(SIGPIPE, SIG_IGN);
        size_t off = 0;
        while (off < input.size()) {
            ssize_t w = write(in[1], input.data() + off, input.size() - off);
            if (w <= 0) break;
            off += (size_t)w;
        }
    }
    close(in[1]);

    std::thread errReader([&] { errText = ReadAll(err[0]); });
    std::atomic<bool> finished{false};
    std::thread killer;
    if (timeoutSec > 0) {
        killer = std::thread([&] {
            for (int i = 0; i < timeoutSec * 10 && !finished; ++i) usleep(100000);
            if (!finished) kill(-pid, SIGKILL);
        });
    }
    char buf[8192];
    ssize_t n;
    while ((n = read(out[0], buf, sizeof buf)) > 0) onStdout(buf, (size_t)n);
    int status = 0;
    waitpid(pid, &status, 0);
    finished = true;
    errReader.join();
    if (killer.joinable()) killer.join();
    close(out[0]), close(err[0]);
    pj.pid = 0;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

void RunChat(std::shared_ptr<ProcJob> pj, std::vector<std::string> f) {
    const std::string id = util::Json(pj->id);
    std::string exe = FindExe();
    if (exe.empty()) {
        bridge::PostToPage("{\"t\":\"error\",\"id\":" + id + ",\"status\":-3,\"body\":\"Claude Code est introuvable.\"}");
    } else {
        // Aucun outil (fichiers, commandes, MCP) : Claude Code sert uniquement à discuter. Seule exception, voulue par la
        // page (champ « outils », liste séparée par des virgules) : « web » = la recherche sur internet (sans lecture de
        // page), « screen » = lire les captures d'écran du dossier screens/ (Read, rien d'autre). Le reste est ignoré.
        const std::string& wanted = f[4];
        const bool web = ("," + wanted + ",").find(",web,") != std::string::npos;
        const bool screen = ("," + wanted + ",").find(",screen,") != std::string::npos;
        const char* tools = web && screen ? "WebSearch,Read" : web ? "WebSearch" : screen ? "Read" : "";
        std::vector<std::string> args = {"-p", "--output-format", "stream-json", "--verbose",
                                         "--include-partial-messages", "--tools", tools, "--disallowedTools",
                                         "mcp__*", "--strict-mcp-config", "--disable-slash-commands",
                                         "--system-prompt", f[5]};
        if (web) args.insert(args.end(), {"--allowedTools", "WebSearch"});
        if (screen) args.insert(args.end(), {"--allowedTools", "Read(./screens/**)"}); // le dossier de travail ne contient que ça
        if (!f[1].empty() && f[1] != "default") args.insert(args.end(), {"--model", f[1]});
        if (!f[2].empty()) args.insert(args.end(), {"--effort", f[2]});
        if (!f[3].empty()) args.insert(args.end(), {"--resume", f[3]});

        std::string pending, err;
        int code = RunProcess(*pj, exe, args, f[6], [&](const char* d, size_t n) {
            pending.append(d, n);
            size_t cut = pending.rfind('\n');
            if (cut != std::string::npos) {
                bridge::PostToPage("{\"t\":\"chunk\",\"id\":" + id + ",\"data\":" + util::Json(pending.substr(0, cut + 1)) + "}");
                pending.erase(0, cut + 1);
            }
        }, err, 0);
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
    if (pid_t pid = pj->pid) kill(-pid, SIGKILL);
    return true;
}

void CheckInstalled() {
    std::thread([] {
        std::string exe = FindExe();
        std::string version, err;
        if (!exe.empty()) {
            ProcJob pj;
            RunProcess(pj, exe, {"--version"}, "", [&](const char* d, size_t n) { version.append(d, n); }, err, 20);
            while (!version.empty() && std::isspace((unsigned char)version.back())) version.pop_back();
        }
        bridge::PostToPage(std::string("{\"t\":\"ccstatus\",\"found\":") + (exe.empty() ? "false" : "true") +
                           ",\"path\":" + util::Json(exe) + ",\"version\":" + util::Json(version) + "}");
    }).detach();
}

void OpenLogin() {
    // Pas de console portable sous Linux : on essaie les terminaux courants.
    std::string exe = FindExe();
    if (exe.empty()) return;
    // Chemin cité pour le shell (les apostrophes sont échappées).
    std::string quoted = "'";
    for (char c : exe) quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
    quoted += "'";
    std::string script =
        "for t in 'x-terminal-emulator -e' 'gnome-terminal --' 'konsole -e' 'xfce4-terminal -e' 'xterm -e'; do "
        "set -- $t; if command -v \"$1\" >/dev/null 2>&1; then \"$@\" " + quoted + "; exit; fi; done";
    std::string line = "sh -c " + std::string("'") + [&] {
        std::string s;
        for (char c : script) s += c == '\'' ? std::string("'\\''") : std::string(1, c);
        return s;
    }() + "' >/dev/null 2>&1 &";
    std::system(line.c_str()); // en arrière-plan : pas de processus zombie à attendre
}

} // namespace claude_code
