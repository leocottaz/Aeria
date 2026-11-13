// Faux serveur MCP « stdio » pour le banc d'essai de src/features/mcp.cpp (et mcp_linux.cpp). Sans dépendance : il reconnaît
// les requêtes par sous-chaînes. Outils : echo (longueur de la ligne reçue), info (arguments, dossier, environnement),
// spawn (lance un enfant qui dort, pour vérifier qu'il est tué avec le serveur), flood (beaucoup de lignes), die (code 3).
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

static std::string Esc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '\\' || c == '"') o += '\\';
        o += c;
    }
    return o;
}

static std::string IdOf(const std::string& line) {
    size_t p = line.find("\"id\":");
    if (p == std::string::npos) return "null";
    p += 5;
    size_t e = line.find_first_of(",}", p);
    return line.substr(p, e - p);
}

static void Out(const std::string& s) {
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

static void Text(const std::string& id, const std::string& text, bool error = false) {
    Out("{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"" + Esc(text) + "\"}],\"isError\":" +
        (error ? "true" : "false") + "}}");
}

int main(int argc, char** argv) {
    std::string selfArgs;
    for (int i = 1; i < argc; ++i) selfArgs += std::string(i > 1 ? "|" : "") + argv[i];
    if (argc > 1 && std::string(argv[1]) == "--sleep") { // l'enfant de « spawn »
#ifdef _WIN32
        Sleep(600000);
#else
        sleep(600);
#endif
        return 0;
    }
    std::fprintf(stderr, "fake: demarre avec %d arguments\n", argc - 1);
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string id = IdOf(line);
        auto has = [&](const char* s) { return line.find(s) != std::string::npos; };
        if (has("\"method\":\"initialize\"")) {
            Out("{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"fake\",\"version\":\"1.0\"}}}");
        } else if (has("\"method\":\"tools/list\"")) {
            Out("{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":{\"tools\":[{\"name\":\"echo\",\"description\":\"Renvoie la longueur\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}}}},"
                "{\"name\":\"info\",\"inputSchema\":{\"type\":\"object\"},\"annotations\":{\"readOnlyHint\":true}}]}}");
        } else if (has("\"method\":\"tools/call\"")) {
            if (has("\"name\":\"echo\"")) {
                Text(id, "len=" + std::to_string(line.size()));
            } else if (has("\"name\":\"info\"")) {
                char cwd[1024] = "";
#ifdef _WIN32
                GetCurrentDirectoryA(sizeof cwd, cwd);
#else
                if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
#endif
                const char* var = std::getenv("AERIA_TEST_VAR");
                Text(id, "args=" + selfArgs + ";var=" + (var ? var : "(absente)") + ";apikey=" + (std::getenv("ANTHROPIC_API_KEY") ? "oui" : "non") + ";cwd=" + cwd);
            } else if (has("\"name\":\"spawn\"")) {
#ifdef _WIN32
                char exe[MAX_PATH];
                GetModuleFileNameA(nullptr, exe, MAX_PATH);
                std::string cmd = std::string("\"") + exe + "\" --sleep";
                STARTUPINFOA si{sizeof si};
                PROCESS_INFORMATION pi{};
                if (CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                    Text(id, "child=" + std::to_string(pi.dwProcessId));
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);
                } else {
                    Text(id, "spawn impossible", true);
                }
#else
                pid_t p = fork();
                if (p == 0) {
                    execl("/proc/self/exe", "fake_server", "--sleep", (char*)nullptr);
                    _exit(127);
                }
                Text(id, "child=" + std::to_string(p));
#endif
            } else if (has("\"name\":\"flood\"")) {
                for (int i = 0; i < 2000; ++i) Out("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/message\",\"params\":{\"n\":" + std::to_string(i) + "}}");
                Text(id, "flood termine");
            } else if (has("\"name\":\"die\"")) {
                std::exit(3);
            } else {
                Text(id, "outil inconnu", true);
            }
        } else if (has("\"method\":\"ping\"")) {
            Out("{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":{}}");
        }
    }
    return 0;
}
