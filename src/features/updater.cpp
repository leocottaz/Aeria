// Mise à jour Windows : télécharge Setup.exe depuis la release GitHub, vérifie son SHA-256, le lance.
// Voir features/updater.h pour le principe et pourquoi la page ne fournit qu'un numéro de version.
#include "features/updater.h"

#include "common/util.h"
#include "web/bridge.h"

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <thread>
#include <vector>

// Dépôt officiel, et seulement lui. Les tests le remplacent par un serveur local (voir tools/dev).
#ifndef AERIA_UPDATE_BASE
#define AERIA_UPDATE_BASE "https://github.com/leocottaz/Aeria/releases/download/"
#endif

namespace updater {
namespace {

constexpr uint64_t kMaxInstaller = 300ull * 1024 * 1024;
constexpr uint64_t kMaxSums = 64 * 1024;

std::atomic<bool> g_busy{false};

void Post(const char* state, int pct = -1, const std::string& msg = "") {
    std::string j = std::string("{\"t\":\"update\",\"state\":\"") + state + "\"";
    if (pct >= 0) j += ",\"pct\":" + std::to_string(pct);
    if (!msg.empty()) j += ",\"msg\":" + util::Json(msg);
    bridge::PostToPage(j + "}");
}

// ------------------------------------------------------------------ SHA-256 (CNG)

class Sha256 {
public:
    Sha256() {
        if (BCryptOpenAlgorithmProvider(&alg_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
            if (BCryptCreateHash(alg_, &hash_, nullptr, 0, nullptr, 0, 0) != 0) hash_ = nullptr;
        }
    }
    ~Sha256() {
        if (hash_) BCryptDestroyHash(hash_);
        if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
    }
    bool Ok() const { return hash_ != nullptr; }
    void Update(const char* data, size_t n) {
        if (hash_) BCryptHashData(hash_, (PUCHAR)data, (ULONG)n, 0);
    }
    std::string Hex() {
        UCHAR out[32]{};
        if (!hash_ || BCryptFinishHash(hash_, out, sizeof out, 0) != 0) return {};
        static const char* digits = "0123456789abcdef";
        std::string s;
        for (UCHAR b : out) s += digits[b >> 4], s += digits[b & 15];
        return s;
    }

private:
    BCRYPT_ALG_HANDLE alg_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
};

// ------------------------------------------------------------------ HTTPS

// GET en flux. sink reçoit les octets (false = abandon). Redirections suivies (GitHub renvoie vers
// objects.githubusercontent.com), mais jamais de HTTPS vers HTTP.
bool HttpGet(const std::string& url, uint64_t maxBytes, const std::function<bool(const char*, size_t)>& sink,
             const std::function<void(uint64_t)>& onLength, std::string& error) {
    std::wstring wurl = util::Widen(url);
    URL_COMPONENTS uc{sizeof uc};
    wchar_t host[256]{}, path[2048]{};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2047;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
        error = "Adresse invalide.";
        return false;
    }
    bool secure = uc.nScheme == INTERNET_SCHEME_HTTPS;
#ifndef AERIA_UPDATE_ALLOW_HTTP
    if (!secure) {
        error = "Seul HTTPS est accepté.";
        return false;
    }
#endif

    HINTERNET session = WinHttpOpen((L"Aeria-updater/" + util::Widen(Version())).c_str(),
                                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connect = session ? WinHttpConnect(session, host, uc.nPort, 0) : nullptr;
    HINTERNET req = connect ? WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                                 WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0)
                            : nullptr;
    bool ok = false;
    if (req) {
        WinHttpSetTimeouts(req, 0, 30000, 30000, 60000);
        if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(req, nullptr)) {
            DWORD status = 0, size = sizeof status;
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                &status, &size, WINHTTP_NO_HEADER_INDEX);
            if (status != 200) {
                error = status == 404 ? "Fichier introuvable dans la release (HTTP 404)."
                                      : "Réponse inattendue du serveur (HTTP " + std::to_string(status) + ").";
            } else {
                wchar_t lenText[32]{};
                DWORD lenSize = sizeof lenText;
                if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, lenText, &lenSize,
                                        WINHTTP_NO_HEADER_INDEX))
                    onLength((uint64_t)_wtoi64(lenText));
                uint64_t total = 0;
                std::vector<char> buf(65536);
                ok = true;
                for (;;) {
                    DWORD avail = 0, read = 0;
                    if (!WinHttpQueryDataAvailable(req, &avail)) {
                        error = "Connexion interrompue.";
                        ok = false;
                        break;
                    }
                    if (avail == 0) break;
                    if (avail > buf.size()) avail = (DWORD)buf.size();
                    if (!WinHttpReadData(req, buf.data(), avail, &read) || read == 0) {
                        error = "Connexion interrompue.";
                        ok = false;
                        break;
                    }
                    total += read;
                    if (total > maxBytes) {
                        error = "Fichier plus gros que prévu : abandon.";
                        ok = false;
                        break;
                    }
                    if (!sink(buf.data(), read)) {
                        error = "Écriture impossible sur le disque.";
                        ok = false;
                        break;
                    }
                }
            }
        } else {
            error = "Impossible de joindre le serveur (code " + std::to_string(GetLastError()) + ").";
        }
    }
    if (req) WinHttpCloseHandle(req);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    if (!ok && error.empty()) error = "Échec du téléchargement.";
    return ok;
}

// ------------------------------------------------------------------ déroulement

void Run(const std::string& tag) {
    namespace fs = std::filesystem;
    const std::string asset = AssetName(tag);
    const std::string base = std::string(AERIA_UPDATE_BASE) + tag + "/";
    std::string error;

    // 1. L'empreinte attendue, dans le SHA256SUMS de la même release.
    Post("downloading", 0);
    std::string sums;
    if (!HttpGet(base + "SHA256SUMS", kMaxSums, [&](const char* d, size_t n) { sums.append(d, n); return true; },
                 [](uint64_t) {}, error)) {
        Post("error", -1, "SHA256SUMS introuvable : " + error);
        return;
    }
    const std::string expected = ExpectedHash(sums, asset);
    if (expected.empty()) {
        Post("error", -1, "L'empreinte de " + asset + " est absente de SHA256SUMS : mise à jour annulée.");
        return;
    }

    // 2. L'installateur, dans un dossier temporaire dédié.
    std::error_code ec;
    fs::path dir = fs::temp_directory_path(ec) / "Aeria-update";
    fs::create_directories(dir, ec);
    fs::path file = dir / asset;
    fs::remove(file, ec);

    Sha256 sha;
    if (!sha.Ok()) {
        Post("error", -1, "Vérification d'empreinte indisponible sur ce système.");
        return;
    }
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
        Post("error", -1, "Impossible d'écrire dans " + dir.string());
        return;
    }
    uint64_t length = 0, received = 0;
    int lastPct = -1;
    bool ok = HttpGet(base + asset, kMaxInstaller,
                      [&](const char* d, size_t n) {
                          out.write(d, (std::streamsize)n);
                          sha.Update(d, n);
                          received += n;
                          int pct = length ? (int)std::min<uint64_t>(99, received * 100 / length) : -1;
                          if (pct >= 0 && pct >= lastPct + 3) {
                              lastPct = pct;
                              Post("downloading", pct);
                          }
                          return (bool)out;
                      },
                      [&](uint64_t n) { length = n; }, error);
    out.close();
    if (!ok) {
        fs::remove(file, ec);
        Post("error", -1, error);
        return;
    }

    // 3. Vérification : un fichier altéré (ou une release incohérente) n'est jamais lancé.
    Post("verifying", 100);
    if (sha.Hex() != expected) {
        fs::remove(file, ec);
        Post("error", -1, "L'empreinte SHA-256 ne correspond pas : fichier non fiable, installation annulée.");
        return;
    }

    // 4. Installation silencieuse par-dessus ; l'installateur arrête ce dock puis le relance (/RELAUNCH=1).
    Post("installing", 100);
    std::wstring cmd = L"\"" + file.wstring() + L"\" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /RELAUNCH=1";
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(file.c_str(), buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        Post("error", -1, "Impossible de lancer l'installateur (code " + std::to_string(GetLastError()) + ").");
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    Post("restarting", 100);
}

} // namespace

bool CanSelfUpdate() {
    namespace fs = std::filesystem;
    std::error_code ec;
    return fs::exists(fs::path(util::ExeDir()) / "unins000.exe", ec);
}

void Install(const std::string& tag) {
    if (!IsValidTag(tag)) {
        Post("error", -1, "Numéro de version invalide.");
        return;
    }
    if (!CanSelfUpdate()) {
        Post("error", -1, "Cette copie n'a pas été installée par le programme d'installation : télécharge la nouvelle version depuis GitHub.");
        return;
    }
    if (g_busy.exchange(true)) return;
    std::thread([tag] {
        Run(tag);
        g_busy = false;
    }).detach();
}

} // namespace updater
