// Commande vocale sous Windows : micro (waveIn), téléchargement du modèle (WinHTTP + SHA-256 CNG) et
// ouverture du dock. Le reste est dans features/voice.cpp.
#include "features/voice_platform.h"

#include "app/dock.h"
#include "common/util.h"

#include <windows.h>
#include <bcrypt.h>
#include <mmsystem.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

namespace voice::platform {
namespace {

// ------------------------------------------------------------------ micro

constexpr int kBuffers = 6;
constexpr int kBufferSamples = 1600; // 100 ms à 16 kHz

HWAVEIN g_in = nullptr;
HANDLE g_event = nullptr;
std::thread g_thread;
std::atomic<bool> g_run{false};
WAVEHDR g_headers[kBuffers];
std::vector<int16_t> g_data[kBuffers];

void CaptureLoop(Sink sink) {
    while (g_run) {
        WaitForSingleObject(g_event, 200);
        for (WAVEHDR& h : g_headers) {
            if (!(h.dwFlags & WHDR_DONE)) continue;
            if (g_run && h.dwBytesRecorded) sink(reinterpret_cast<const int16_t*>(h.lpData), h.dwBytesRecorded / 2);
            h.dwBytesRecorded = 0;
            if (g_run) waveInAddBuffer(g_in, &h, sizeof h);
        }
    }
}

} // namespace

bool StartCapture(const Sink& sink, std::string& error) {
    if (g_in) return false;
    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 1;
    fmt.nSamplesPerSec = 16000;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 2;
    fmt.nAvgBytesPerSec = 32000;
    g_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    // WAVE_MAPPER convertit lui-même vers 16 kHz mono si le micro travaille à un autre format.
    if (waveInOpen(&g_in, WAVE_MAPPER, &fmt, (DWORD_PTR)g_event, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        g_in = nullptr;
        CloseHandle(g_event);
        g_event = nullptr;
        error = "Aucun micro disponible (ou accès refusé dans les paramètres de confidentialité de Windows).";
        return false;
    }
    for (int i = 0; i < kBuffers; i++) {
        g_data[i].assign(kBufferSamples, 0);
        g_headers[i] = {};
        g_headers[i].lpData = reinterpret_cast<LPSTR>(g_data[i].data());
        g_headers[i].dwBufferLength = kBufferSamples * 2;
        waveInPrepareHeader(g_in, &g_headers[i], sizeof(WAVEHDR));
        waveInAddBuffer(g_in, &g_headers[i], sizeof(WAVEHDR));
    }
    g_run = true;
    waveInStart(g_in);
    g_thread = std::thread(CaptureLoop, sink);
    return true;
}

void StopCapture() {
    if (!g_in) return;
    g_run = false;
    SetEvent(g_event);
    if (g_thread.joinable()) g_thread.join();
    waveInStop(g_in);
    waveInReset(g_in);
    for (WAVEHDR& h : g_headers) waveInUnprepareHeader(g_in, &h, sizeof h);
    waveInClose(g_in);
    g_in = nullptr;
    CloseHandle(g_event);
    g_event = nullptr;
}

// ------------------------------------------------------------------ modèle

std::string ModelPath(const std::string& file) {
    namespace fs = std::filesystem;
    fs::path dir = fs::path(util::RoamingDataDir()) / "models";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return (dir / file).string();
}

namespace {

std::string Sha256Hex(BCRYPT_HASH_HANDLE hash) {
    UCHAR out[32]{};
    if (BCryptFinishHash(hash, out, sizeof out, 0) != 0) return {};
    static const char* digits = "0123456789abcdef";
    std::string s;
    for (UCHAR b : out) s += digits[b >> 4], s += digits[b & 15];
    return s;
}

} // namespace

bool Download(const std::string& url, const std::string& dest, uint64_t maxBytes, const std::string& sha256,
              const std::function<void(int)>& progress, std::string& error) {
    namespace fs = std::filesystem;
    std::wstring wurl = util::Widen(url);
    URL_COMPONENTS uc{sizeof uc};
    wchar_t host[256]{}, path[2048]{};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2047;
    uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) {
        error = "Adresse du modèle invalide.";
        return false;
    }
    std::wstring target = std::wstring(path, uc.dwUrlPathLength) + std::wstring(uc.lpszExtraInfo ? uc.lpszExtraInfo : L"", uc.dwExtraInfoLength);

    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0 ||
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) != 0) {
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
        error = "Vérification d'empreinte indisponible sur ce système.";
        return false;
    }

    const fs::path tmp = fs::path(dest + ".part");
    std::error_code ec;
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    HINTERNET session = WinHttpOpen(L"Aeria-voice", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connect = session ? WinHttpConnect(session, host, uc.nPort, 0) : nullptr;
    HINTERNET req = connect ? WinHttpOpenRequest(connect, L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                 WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                            : nullptr;
    bool ok = false;
    if (!out) {
        error = "Impossible d'écrire dans " + tmp.parent_path().string();
    } else if (req) {
        WinHttpSetTimeouts(req, 0, 30000, 30000, 60000);
        if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(req, nullptr)) {
            DWORD status = 0, size = sizeof status;
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                &status, &size, WINHTTP_NO_HEADER_INDEX);
            if (status != 200) {
                error = "Réponse inattendue du serveur (HTTP " + std::to_string(status) + ").";
            } else {
                wchar_t lenText[32]{};
                DWORD lenSize = sizeof lenText;
                uint64_t length = 0, received = 0;
                if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, lenText, &lenSize,
                                        WINHTTP_NO_HEADER_INDEX))
                    length = (uint64_t)_wtoi64(lenText);
                std::vector<char> buf(65536);
                ok = true;
                for (;;) {
                    DWORD avail = 0, read = 0;
                    if (!WinHttpQueryDataAvailable(req, &avail) || (avail && !WinHttpReadData(req, buf.data(), std::min<DWORD>(avail, (DWORD)buf.size()), &read))) {
                        error = "Connexion interrompue.";
                        ok = false;
                        break;
                    }
                    if (avail == 0) break;
                    received += read;
                    if (received > maxBytes) {
                        error = "Fichier plus gros que prévu : abandon.";
                        ok = false;
                        break;
                    }
                    out.write(buf.data(), read);
                    BCryptHashData(hash, (PUCHAR)buf.data(), read, 0);
                    if (!out) {
                        error = "Écriture impossible sur le disque.";
                        ok = false;
                        break;
                    }
                    if (length) progress((int)std::min<uint64_t>(99, received * 100 / length));
                }
            }
        } else {
            error = "Impossible de joindre le serveur (code " + std::to_string(GetLastError()) + ").";
        }
    }
    out.close();
    if (req) WinHttpCloseHandle(req);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);

    const std::string got = ok ? Sha256Hex(hash) : "";
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (ok && got != sha256) {
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
    if (!ok && error.empty()) error = "Échec du téléchargement.";
    if (ok) progress(100);
    return ok;
}

void ShowListening(bool on) { dock::SetListeningAsync(on); }

} // namespace voice::platform
