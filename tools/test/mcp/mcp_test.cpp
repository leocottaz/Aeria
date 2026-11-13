// Banc d'essai du transport MCP (src/features/mcp.cpp sous Windows, src/linux/mcp_linux.cpp sous Linux) : le vrai code,
// un faux pont qui garde les messages envoyés à la page, et un faux serveur (fake_server.cpp). Aucun vrai serveur MCP,
// aucun réseau, aucun vrai dock.
//   mcp_test <fake_server> <dossier de travail>
#include "features/mcp.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

// ---- faux pont : les messages « vers la page » sont gardés ici
namespace bridge {
std::mutex g_mutex;
std::vector<std::string> g_events;
std::condition_variable g_cv;
void PostToPage(std::string json) {
    {
        std::lock_guard lock(g_mutex);
        g_events.push_back(std::move(json));
    }
    g_cv.notify_all();
}
void PostToPageAwake(std::string json) { PostToPage(std::move(json)); }
} // namespace bridge

namespace {

int g_failed = 0, g_passed = 0;
void Check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "OK    " : "ÉCHEC ", what.c_str());
    (ok ? g_passed : g_failed)++;
}

std::string Unesc(const std::string& s) { // le contenu d'une chaîne JSON, sans les échappements \" \\ \n
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            ++i;
            o += s[i] == 'n' ? '\n' : s[i];
        } else {
            o += s[i];
        }
    }
    return o;
}

// Premier évènement (après `from`) du serveur `id` de type `ev` dont le texte (déséchappé) contient `needle`. -1 si aucun.
int FindEvent(const std::string& id, const std::string& ev, const std::string& needle = "", size_t from = 0) {
    std::lock_guard lock(bridge::g_mutex);
    for (size_t i = from; i < bridge::g_events.size(); ++i) {
        const std::string& e = bridge::g_events[i];
        if (e.find("\"id\":\"" + id + "\"") == std::string::npos || e.find("\"ev\":\"" + ev + "\"") == std::string::npos) continue;
        if (needle.empty() || Unesc(e).find(needle) != std::string::npos) return (int)i;
    }
    return -1;
}

int WaitEvent(const std::string& id, const std::string& ev, const std::string& needle = "", size_t from = 0, int ms = 8000) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    for (;;) {
        int i = FindEvent(id, ev, needle, from);
        if (i >= 0) return i;
        std::unique_lock lock(bridge::g_mutex);
        if (bridge::g_cv.wait_until(lock, end) == std::cv_status::timeout) return FindEvent(id, ev, needle, from);
    }
}

size_t Mark() {
    std::lock_guard lock(bridge::g_mutex);
    return bridge::g_events.size();
}

std::string EventAt(int i) {
    std::lock_guard lock(bridge::g_mutex);
    return i >= 0 && (size_t)i < bridge::g_events.size() ? bridge::g_events[i] : "";
}

long long GenOf(const std::string& e) {
    size_t p = e.find("\"gen\":");
    return p == std::string::npos ? -1 : std::atoll(e.c_str() + p + 6);
}

std::string Join(const std::vector<std::string>& v, char sep) {
    std::string o;
    for (size_t i = 0; i < v.size(); ++i) o += (i ? std::string(1, sep) : "") + v[i];
    return o;
}

void StartServer(const std::string& id, const std::string& cmd, const std::vector<std::string>& args = {}, const std::vector<std::string>& env = {}) {
    mcp::Start(id + "\x1f" + cmd + "\x1f" + Join(args, '\x1e') + "\x1f" + Join(env, '\x1e'));
}

void Line(const std::string& id, const std::string& json) { mcp::Send(id + "\n" + json); }

bool ProcessAlive(long pid) {
#ifdef _WIN32
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h) return false;
    bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
#else
    return kill((pid_t)pid, 0) == 0;
#endif
}

void SetEnv(const char* k, const char* v) {
#ifdef _WIN32
    _putenv_s(k, v);
#else
    setenv(k, v, 1);
#endif
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage : mcp_test <fake_server> <dossier>\n");
        return 2;
    }
    const std::string fake = argv[1];
    const std::filesystem::path work = argv[2];

    // 1. Lancement, poignée de main, appel d'outil (réponses sur une ligne, fins de ligne \r\n sous Windows)
    SetEnv("ANTHROPIC_API_KEY", "ne-doit-pas-passer");
    SetEnv("AERIA_TEST_VAR", "du-parent");
    StartServer("t1", fake, {"un argument", "deux"}, {"AERIA_TEST_VAR=valeur-ok"});
    int started = WaitEvent("t1", "started");
    Check(started >= 0, "le serveur démarre (évènement started)");
    long long gen1 = GenOf(EventAt(started));
    Check(gen1 > 0, "started porte une génération");
    Line("t1", R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"test","version":"1"}}})");
    Check(WaitEvent("t1", "line", "\"protocolVersion\":\"2025-06-18\"") >= 0, "initialize : réponse reçue");
    Line("t1", R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    Line("t1", R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})");
    Check(WaitEvent("t1", "line", "\"name\":\"echo\"") >= 0, "tools/list : les outils arrivent");
    Line("t1", R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"info","arguments":{}}})");
    int info = WaitEvent("t1", "line", "args=un argument|deux");
    Check(info >= 0, "les arguments (dont un avec espace) arrivent intacts");
    std::string infoText = Unesc(EventAt(info));
    Check(infoText.find("var=valeur-ok") != std::string::npos, "la variable du serveur remplace celle du parent");
    Check(infoText.find("apikey=non") != std::string::npos, "ANTHROPIC_API_KEY n'est pas transmise");
    Check(infoText.find("mcp") != std::string::npos, "dossier de travail dédié");
    int stderrCheck = -1;

    // 2. Lignes inconnues et envois refusés
    size_t m = Mark();
    Line("t1", "{\"jsonrpc\":\"2.0\",\"id\":98,\"method\":\"ping\"}\n{\"jsonrpc\":\"2.0\",\"id\":99,\"method\":\"ping\"}"); // deux lignes d'un coup : refusé
    mcp::Send("inconnu\n{\"jsonrpc\":\"2.0\",\"id\":97,\"method\":\"ping\"}");                                                   // id inconnu : ignoré, sans plantage
    mcp::Send("sans-retour-a-la-ligne");                                                                                          // mal formé : ignoré
    Line("t1", R"({"jsonrpc":"2.0","id":4,"method":"ping"})");
    Check(WaitEvent("t1", "line", "\"id\":4", m) >= 0, "le serveur répond toujours après des envois invalides");
    Check(FindEvent("t1", "line", "\"id\":98", m) < 0 && FindEvent("t1", "line", "\"id\":99", m) < 0, "une ligne contenant un retour à la ligne n'est pas envoyée");

    // 3. Gros message dans les deux sens (file d'écriture + lecture par morceaux)
    std::string big(1500000, 'x');
    Line("t1", "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":{\"name\":\"echo\",\"arguments\":{\"text\":\"" + big + "\"}}}");
    int echo = WaitEvent("t1", "line", "len=15", 0, 15000);
    Check(echo >= 0, "message de 1,5 Mo transmis intact (longueur reçue par le serveur : " + (echo >= 0 ? Unesc(EventAt(echo)).substr(Unesc(EventAt(echo)).find("len="), 12) : std::string("?")) + ")");

    // 4. Beaucoup de lignes rapides, dans l'ordre
    m = Mark();
    Line("t1", R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"flood","arguments":{}}})");
    Check(WaitEvent("t1", "line", "flood termine", m, 15000) >= 0, "2000 notifications puis la réponse arrivent");
    int ordered = 1;
    {
        std::lock_guard lock(bridge::g_mutex);
        int expect = 0;
        for (size_t i = m; i < bridge::g_events.size(); ++i) {
            const std::string e = Unesc(bridge::g_events[i]);
            size_t p = e.find("\"n\":");
            if (p == std::string::npos) continue;
            if (std::atoi(e.c_str() + p + 4) != expect++) ordered = 0;
        }
        ordered = ordered && expect == 2000;
    }
    Check(ordered, "les lignes arrivent dans l'ordre, sans perte");

    // 5. Un enfant du serveur est tué avec lui (job Windows / groupe de processus Linux)
    m = Mark();
    Line("t1", R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"spawn","arguments":{}}})");
    int sp = WaitEvent("t1", "line", "child=", m);
    long child = sp >= 0 ? std::atol(Unesc(EventAt(sp)).substr(Unesc(EventAt(sp)).find("child=") + 6).c_str()) : 0;
    Check(child > 0 && ProcessAlive(child), "le serveur a lancé un enfant qui tourne");
    m = Mark();
    mcp::Stop("t1");
    int exited = WaitEvent("t1", "exit", "", m);
    Check(exited >= 0, "Stop : évènement exit reçu");
    Check(GenOf(EventAt(exited)) == gen1, "exit porte la génération du serveur arrêté");
    bool dead = false;
    for (int i = 0; i < 40 && !dead; ++i) {
        dead = !ProcessAlive(child);
        if (!dead) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    Check(dead, "l'enfant du serveur est mort avec lui");
    stderrCheck = WaitEvent("t1", "exit", "fake: demarre avec 2 arguments", m, 1000);
    Check(stderrCheck >= 0, "la fin de stderr est jointe à l'évènement exit");

    // 6. Relancer un id déjà lancé : l'ancien est arrêté, la génération augmente
    m = Mark();
    StartServer("t2", fake);
    int s1 = WaitEvent("t2", "started", "", m);
    long long g1 = GenOf(EventAt(s1));
    StartServer("t2", fake);
    int s2 = WaitEvent("t2", "started", "", s1 + 1);
    long long g2 = GenOf(EventAt(s2));
    Check(s1 >= 0 && s2 > s1 && g2 > g1, "relancer un id donne une nouvelle génération");
    int oldExit = WaitEvent("t2", "exit", "", m);
    Check(oldExit >= 0 && GenOf(EventAt(oldExit)) == g1, "l'ancien processus signale sa fin avec son ancienne génération");
    Line("t2", R"({"jsonrpc":"2.0","id":1,"method":"ping"})");
    Check(WaitEvent("t2", "line", "\"id\":1", s2) >= 0, "le nouveau processus répond");

    // 7. Le serveur meurt tout seul : code de sortie transmis
    m = Mark();
    Line("t2", R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"die","arguments":{}}})");
    int died = WaitEvent("t2", "exit", "\"code\":3", m);
    Check(died >= 0, "code de sortie du serveur (3) transmis");

    // 8. Commande introuvable, trop de serveurs
    m = Mark();
    StartServer("t3", "commande-qui-n-existe-pas-xyz");
    int fail = WaitEvent("t3", "fail", "introuvable", m);
    Check(fail >= 0, "commande introuvable : évènement fail");
    m = Mark();
    mcp::Start("mauvais id\x1f" + fake + "\x1f\x1f"); // id invalide : ignoré
    mcp::Start("t4\x1f" + fake + "\x1f\x1f" + "SANS_EGAL");  // variable mal formée : ignoré
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    Check(Mark() == m, "id invalide ou variable mal formée : aucun lancement");
    for (int i = 0; i < 8; ++i) StartServer("m" + std::to_string(i), fake);
    for (int i = 0; i < 8; ++i) WaitEvent("m" + std::to_string(i), "started");
    m = Mark();
    StartServer("m8", fake);
    Check(WaitEvent("m8", "fail", "Trop de serveurs", m) >= 0, "plus de 8 serveurs : refusé");
    mcp::Stop("");
    for (int i = 0; i < 8; ++i) WaitEvent("m" + std::to_string(i), "exit");
    Check(true, "Stop(\"\") arrête tous les serveurs");

#ifdef _WIN32
    // 9. Commandes .cmd (npx, etc.) : lancées par cmd.exe, sans que cmd interprète les arguments
    std::filesystem::create_directories(work);
    {
        std::ofstream f(work / "fakecmd.cmd");
        f << "@echo off\r\necho {\"args\":\"%~1|%~2\"}\r\n";
    }
    std::string path = std::getenv("PATH") ? std::getenv("PATH") : "";
    SetEnv("PATH", (work.string() + ";" + path).c_str());
    m = Mark();
    StartServer("c1", "fakecmd", {"hello world", "x"});
    Check(WaitEvent("c1", "line", "\"args\":\"hello world|x\"", m) >= 0, "un .cmd est trouvé dans le PATH et reçoit ses arguments (dont un avec espace)");
    Check(WaitEvent("c1", "exit", "\"code\":0", m) >= 0, "le .cmd se termine avec le code 0");
    for (const char* bad : {"a&b", "a|b", "a>b", "a%b", "a^b", "a\"b", "a!b"}) {
        m = Mark();
        StartServer("c2", "fakecmd", {bad});
        Check(WaitEvent("c2", "fail", ".cmd", m, 2000) >= 0, std::string("argument refusé pour un .cmd : ") + bad);
    }
    m = Mark();
    StartServer("c3", "fakecmd.cmd", {"ok"});
    Check(WaitEvent("c3", "line", "\"args\":\"ok|\"", m) >= 0, "commande donnée avec son extension");
#endif

    std::printf("\nRéussis : %d, échecs : %d\n", g_passed, g_failed);
    mcp::Stop("");
    return g_failed ? 1 : 0;
}
