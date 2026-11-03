// Commande vocale sous Linux : micro lu par `parec` (PulseAudio / PipeWire) ou `arecord` (ALSA), lancé par
// fork/exec comme les autres outils système (pc_linux.cpp) ; modèle téléchargé par libcurl et vérifié
// en SHA-256. Le reste est dans features/voice.cpp.
#include "features/voice_platform.h"

#include "app/dock.h"
#include "common/util.h"

#include <curl/curl.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

namespace voice::platform {
namespace {

// ------------------------------------------------------------------ micro

pid_t g_pid = 0;
int g_fd = -1;
std::thread g_thread;
std::atomic<bool> g_run{false};

// Lance argv avec la sortie standard dans un tube ; -1 si l'exécutable n'existe pas.
int Spawn(const std::vector<const char*>& argv, pid_t& pid) {
    int fds[2];
    if (pipe(fds) != 0) return -1;
    pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        setpgid(0, 0);
        dup2(fds[1], 1);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) dup2(devnull, 2);
        close(fds[0]);
        close(fds[1]);
        std::vector<char*> args;
        for (const char* a : argv) args.push_back(const_cast<char*>(a));
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127);
    }
    close(fds[1]);
    return fds[0];
}

} // namespace

bool StartCapture(const Sink& sink, std::string& error) {
    if (g_pid) return false;
    const std::vector<std::vector<const char*>> recorders = {
        {"parec", "--raw", "--format=s16le", "--rate=16000", "--channels=1", "--latency-msec=40"},
        {"arecord", "-q", "-t", "raw", "-f", "S16_LE", "-r", "16000", "-c", "1"},
    };
    for (const auto& argv : recorders) {
        pid_t pid = 0;
        int fd = Spawn(argv, pid);
        if (fd < 0) continue;
        // Un exécutable absent sort aussitôt (code 127) ; un micro refusé aussi.
        usleep(150 * 1000);
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid) {
            close(fd);
            continue;
        }
        g_pid = pid;
        g_fd = fd;
        g_run = true;
        g_thread = std::thread([sink] {
            std::array<int16_t, 640> buf{}; // 40 ms
            size_t have = 0;                // octets déjà lus dans buf
            while (g_run) {
                ssize_t n = read(g_fd, reinterpret_cast<char*>(buf.data()) + have, sizeof buf - have);
                if (n <= 0) break; // le processus s'est arrêté ou StopCapture a fermé le tube
                have += (size_t)n;
                if (have == sizeof buf) {
                    sink(buf.data(), buf.size());
                    have = 0;
                }
            }
        });
        return true;
    }
    error = "Aucun enregistreur audio : installe pipewire-pulse / pulseaudio-utils (parec) ou alsa-utils (arecord).";
    return false;
}

void StopCapture() {
    if (!g_pid) return;
    g_run = false;
    kill(-g_pid, SIGTERM); // tout le groupe
    kill(g_pid, SIGTERM);
    waitpid(g_pid, nullptr, 0);
    if (g_thread.joinable()) g_thread.join();
    close(g_fd);
    g_fd = -1;
    g_pid = 0;
}

// ------------------------------------------------------------------ modèle

std::string ModelPath() {
    namespace fs = std::filesystem;
    fs::path dir = fs::path(util::DataDir()) / "models";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return (dir / "ggml-small-q5_1.bin").string();
}

namespace {

// SHA-256 (FIPS 180-4), pour ne pas dépendre d'OpenSSL.
class Sha256 {
public:
    Sha256() { Reset(); }
    void Update(const unsigned char* data, size_t n) {
        total_ += n;
        while (n) {
            size_t take = std::min(n, sizeof block_ - fill_);
            std::memcpy(block_ + fill_, data, take);
            fill_ += take;
            data += take;
            n -= take;
            if (fill_ == sizeof block_) {
                Compress();
                fill_ = 0;
            }
        }
    }
    std::string Hex() {
        uint64_t bits = total_ * 8;
        unsigned char pad = 0x80;
        Update(&pad, 1);
        unsigned char zero = 0;
        while (fill_ != 56) Update(&zero, 1);
        unsigned char len[8];
        for (int i = 0; i < 8; i++) len[i] = (unsigned char)(bits >> (56 - 8 * i));
        Update(len, 8);
        char out[65];
        for (int i = 0; i < 8; i++) std::snprintf(out + i * 8, 9, "%08x", h_[i]);
        return out;
    }

private:
    void Reset() {
        static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                         0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        std::memcpy(h_, init, sizeof h_);
    }
    static uint32_t Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void Compress() {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)block_[i * 4] << 24 | (uint32_t)block_[i * 4 + 1] << 16 | (uint32_t)block_[i * 4 + 2] << 8 | block_[i * 4 + 3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = h + (Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            uint32_t t2 = (Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
        }
        h_[0] += a, h_[1] += b, h_[2] += c, h_[3] += d, h_[4] += e, h_[5] += f, h_[6] += g, h_[7] += h;
    }

    uint32_t h_[8];
    unsigned char block_[64];
    size_t fill_ = 0;
    uint64_t total_ = 0;
};

struct DownloadState {
    std::ofstream out;
    Sha256 sha;
    uint64_t received = 0, maxBytes = 0, length = 0;
    const std::function<void(int)>* progress = nullptr;
    bool tooBig = false;
};

size_t OnData(char* data, size_t size, size_t count, void* user) {
    auto* st = static_cast<DownloadState*>(user);
    size_t n = size * count;
    st->received += n;
    if (st->received > st->maxBytes) {
        st->tooBig = true;
        return 0;
    }
    st->out.write(data, (std::streamsize)n);
    st->sha.Update(reinterpret_cast<unsigned char*>(data), n);
    if (st->length) (*st->progress)((int)std::min<uint64_t>(99, st->received * 100 / st->length));
    return st->out ? n : 0;
}

} // namespace

bool Download(const std::string& url, const std::string& dest, uint64_t maxBytes, const std::string& sha256,
              const std::function<void(int)>& progress, std::string& error) {
    namespace fs = std::filesystem;
    if (url.rfind("https://", 0) != 0) {
        error = "Adresse du modèle invalide.";
        return false;
    }
    const fs::path tmp = dest + ".part";
    std::error_code ec;
    DownloadState st;
    st.out.open(tmp, std::ios::binary | std::ios::trunc);
    st.maxBytes = maxBytes;
    st.progress = &progress;
    if (!st.out) {
        error = "Impossible d'écrire dans " + tmp.parent_path().string();
        return false;
    }
    CURL* curl = curl_easy_init();
    if (!curl) {
        error = "libcurl indisponible.";
        return false;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https"); // jamais de redirection vers du HTTP clair
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Aeria-voice");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, OnData);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &st);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L); // moins de 1 Ko/s pendant 60 s : abandon
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
    // La taille annoncée n'est connue qu'après les redirections : on la relit à chaque morceau reçu.
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &st);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, +[](void* user, curl_off_t total, curl_off_t, curl_off_t, curl_off_t) -> int {
        static_cast<DownloadState*>(user)->length = total > 0 ? (uint64_t)total : 0;
        return 0;
    });
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    st.out.close();

    bool ok = rc == CURLE_OK && status == 200;
    if (st.tooBig) error = "Fichier plus gros que prévu : abandon.";
    else if (rc != CURLE_OK) error = std::string("Téléchargement impossible : ") + curl_easy_strerror(rc);
    else if (status != 200) error = "Réponse inattendue du serveur (HTTP " + std::to_string(status) + ").";
    if (ok && st.sha.Hex() != sha256) {
        error = "L'empreinte SHA-256 du modèle ne correspond pas : fichier supprimé.";
        ok = false;
    }
    if (ok) {
        fs::rename(tmp, dest, ec);
        if (ec) {
            error = "Impossible d'enregistrer le modèle : " + ec.message();
            ok = false;
        }
    }
    if (!ok) fs::remove(tmp, ec);
    if (ok) progress(100);
    return ok;
}

void OpenDock() { dock::OpenPanelAsync(); }

} // namespace voice::platform
