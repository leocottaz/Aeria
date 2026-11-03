// Banc d'essai de la commande vocale (src/features/voice.cpp) : le vrai code, un faux micro qui lit des
// fichiers WAV 16 kHz mono (voir make-wavs.ps1), un faux pont et un faux dock. Aucun micro réel.
// Usage : voice_test <modele.bin> <fichier.wav>...   (les fichiers sont dits à la suite, séparés de 2 s)
#include "features/voice.h"
#include "features/voice_platform.h"
#include "web/bridge.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
std::string g_model;
std::vector<std::string> g_wavs;
std::atomic<int> g_opens{0};
std::atomic<bool> g_feeding{false}, g_run{false};
std::thread g_thread;
std::mutex g_log;
std::vector<std::string> g_posts;

std::vector<int16_t> ReadWav(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::vector<char> all((std::istreambuf_iterator<char>(f)), {});
    size_t data = 44;
    for (size_t i = 12; i + 8 < all.size();) { // cherche le bloc « data »
        uint32_t len;
        std::memcpy(&len, &all[i + 4], 4);
        if (!std::memcmp(&all[i], "data", 4)) { data = i + 8; break; }
        i += 8 + len;
    }
    std::vector<int16_t> pcm((all.size() - data) / 2);
    std::memcpy(pcm.data(), &all[data], pcm.size() * 2);
    return pcm;
}
} // namespace

namespace bridge {
void PostToPage(std::string json) {
    std::lock_guard<std::mutex> l(g_log);
    std::printf("  -> %s\n", json.c_str());
    g_posts.push_back(json);
}
} // namespace bridge

namespace voice::platform {
bool StartCapture(const Sink& sink, std::string&) {
    g_run = true;
    g_thread = std::thread([sink] {
        auto feed = [&](const std::vector<int16_t>& pcm) {
            for (size_t i = 0; i < pcm.size() && g_run; i += 320) {
                sink(pcm.data() + i, std::min<size_t>(320, pcm.size() - i));
                std::this_thread::sleep_for(std::chrono::milliseconds(4)); // 5x le temps réel
            }
        };
        std::vector<int16_t> gap(16000 * 2);
        for (size_t i = 0; i < gap.size(); i++) gap[i] = (int16_t)((i * 7919) % 41 - 20); // bruit de fond léger
        feed(gap);
        for (const auto& w : g_wavs) {
            feed(ReadWav(w));
            feed(gap);
        }
        g_feeding = false;
        while (g_run) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    });
    return true;
}
void StopCapture() {
    g_run = false;
    if (g_thread.joinable()) g_thread.join();
}
std::string ModelPath() { return g_model; }
bool Download(const std::string&, const std::string&, uint64_t, const std::string&, const std::function<void(int)>&, std::string&) { return false; }
void OpenDock() { g_opens++; }
void ShowListening(bool on) { std::printf("  [encoche %s]\n", on ? "ouverte" : "fermée"); }
} // namespace voice::platform

int main(int argc, char** argv) {
    int fails = 0;
    auto check = [&](bool ok, const char* what) {
        std::printf("%s %s\n", ok ? "OK    " : "ECHEC ", what);
        if (!ok) fails++;
    };

    // --- reconnaissance du mot, sans modèle
    struct Case { const char* text; bool found; const char* rest; };
    const Case cases[] = {
        {"Aeria, mets un minuteur.", true, "mets un minuteur."},
        {"Éria quelle heure est-il", true, "quelle heure est-il"},
        {"Aria ?", true, ""},
        {"ok A éria rappelle-moi", true, "rappelle-moi"},
        {"Maria vient demain", false, ""},
        {"Bonjour, il fait beau", false, ""},
        {"", false, ""},
        {"Hey Aeria !", true, ""},
        {"Aia, Ria.", true, ""},
        {"Il paraît que Aeria viendra demain soir avec nous", false, ""},
        {"a riz et du poulet", false, ""},
        {"À Uriam !", true, ""},
        {"A U R I A O", true, ""},
        {"Il y a eu a b c d", false, ""},
        {"Ariane vient demain", false, ""},
        {"Aurélie arrive", false, ""},
    };
    for (const auto& c : cases) {
        std::string rest;
        bool found = voice::FindWakeWord(c.text, rest);
        check(found == c.found && (!found || rest == c.rest), c.text);
    }
    if (argc < 3) return fails ? 1 : 0;

    // --- chaîne complète : micro simulé → phrases → whisper → page
    g_model = argv[1];
    for (int i = 2; i < argc; i++) g_wavs.push_back(argv[i]);
    g_feeding = true;
    voice::Apply(true);
    auto t0 = std::chrono::steady_clock::now();
    while (g_feeding && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(120)) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::this_thread::sleep_for(std::chrono::seconds(6)); // le temps de finir les dernières transcriptions
    voice::Apply(false);
    int heard = 0;
    for (const auto& p : g_posts) if (p.find("\"heard\"") != std::string::npos) heard++;
    std::printf("RESULTAT opens=%d heard=%d\n", g_opens.load(), heard);
    return fails ? 1 : 0;
}
