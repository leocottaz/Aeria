#include "features/screen.h"

#include "common/util.h"
#include "web/bridge.h"

#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011 // Windows 10 2004+ : la fenêtre reste visible mais absente des captures
#endif

namespace screen {
namespace {

constexpr int kMaxSide = 1568; // l'API réduit de toute façon au-delà
constexpr int kKeepFiles = 1; // la dernière seulement : elle montre l'écran de l'utilisateur

void Reply(const std::string& id, const std::string& fields) {
    bridge::PostToPage("{\"t\":\"reply\",\"id\":" + util::Json(id) + "," + fields + "}");
}

void Fail(const std::string& id, const std::string& message) {
    Reply(id, "\"ok\":false,\"error\":" + util::Json(message));
}

bool JpegEncoder(CLSID* out) {
    UINT n = 0, bytes = 0;
    Gdiplus::GetImageEncodersSize(&n, &bytes);
    if (!bytes) return false;
    std::vector<char> buf(bytes);
    auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
    Gdiplus::GetImageEncoders(n, bytes, codecs);
    for (UINT i = 0; i < n; ++i)
        if (!wcscmp(codecs[i].MimeType, L"image/jpeg")) {
            *out = codecs[i].Clsid;
            return true;
        }
    return false;
}

// JPEG (qualité 80) d'un HBITMAP, en octets.
bool EncodeJpeg(HBITMAP bmp, std::string& bytes) {
    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartupInput in;
    if (Gdiplus::GdiplusStartup(&token, &in, nullptr) != Gdiplus::Ok) return false;
    bool ok = false;
    {
        Gdiplus::Bitmap image(bmp, nullptr);
        CLSID jpeg;
        IStream* stream = nullptr;
        if (JpegEncoder(&jpeg) && SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) {
            ULONG quality = 80;
            Gdiplus::EncoderParameters params{};
            params.Count = 1;
            params.Parameter[0] = {Gdiplus::EncoderQuality, 1, Gdiplus::EncoderParameterValueTypeLong, &quality};
            if (image.Save(stream, &jpeg, &params) == Gdiplus::Ok) {
                HGLOBAL mem = nullptr;
                if (SUCCEEDED(GetHGlobalFromStream(stream, &mem))) {
                    STATSTG st{};
                    stream->Stat(&st, STATFLAG_NONAME);
                    if (void* p = GlobalLock(mem)) {
                        bytes.assign(static_cast<const char*>(p), (size_t)st.cbSize.QuadPart);
                        GlobalUnlock(mem);
                        ok = !bytes.empty();
                    }
                }
            }
            stream->Release();
        }
    }
    Gdiplus::GdiplusShutdown(token);
    return ok;
}

// Garde les dernières captures seulement (elles contiennent l'écran de l'utilisateur).
std::string SaveFile(const std::string& bytes) {
    namespace fs = std::filesystem;
    fs::path dir = fs::path(util::LocalDataDir()) / L"claude-chat" / L"screens"; // = dossier de travail de claude_code.cpp
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::vector<fs::path> old;
    for (auto& e : fs::directory_iterator(dir, ec))
        if (e.path().extension() == L".jpg") old.push_back(e.path());
    std::sort(old.begin(), old.end());
    while ((int)old.size() >= kKeepFiles) fs::remove(old.front(), ec), old.erase(old.begin());
    std::string name = "screen-" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                       std::chrono::system_clock::now().time_since_epoch()).count()) + ".jpg";
    std::ofstream(dir / name, std::ios::binary).write(bytes.data(), (std::streamsize)bytes.size());
    return "screens/" + name;
}

void Run(std::string id, bool toFile, HWND dock) {
    RECT r{};
    MONITORINFO mi{sizeof mi};
    if (GetMonitorInfoW(MonitorFromWindow(dock, MONITOR_DEFAULTTONEAREST), &mi)) r = mi.rcMonitor;
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return Fail(id, "Écran introuvable.");
    double k = std::min(1.0, kMaxSide / (double)std::max(w, h));
    int tw = std::max(1, (int)(w * k)), th = std::max(1, (int)(h * k));

    // Le dock reste affiché pour l'utilisateur mais n'entre pas dans la capture.
    bool excluded = SetWindowDisplayAffinity(dock, WDA_EXCLUDEFROMCAPTURE);
    if (excluded) Sleep(90); // le temps que le compositeur applique l'exclusion
    HDC screenDc = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screenDc);
    HBITMAP bmp = CreateCompatibleBitmap(screenDc, tw, th);
    HGDIOBJ prev = SelectObject(mem, bmp);
    SetStretchBltMode(mem, HALFTONE);
    SetBrushOrgEx(mem, 0, 0, nullptr);
    BOOL copied = StretchBlt(mem, 0, 0, tw, th, screenDc, r.left, r.top, w, h, SRCCOPY | CAPTUREBLT);
    SelectObject(mem, prev);
    DeleteDC(mem);
    ReleaseDC(nullptr, screenDc);
    if (excluded) SetWindowDisplayAffinity(dock, WDA_NONE);

    std::string jpeg;
    bool ok = copied && EncodeJpeg(bmp, jpeg);
    DeleteObject(bmp);
    if (!ok) return Fail(id, "La capture d'écran a échoué.");
    std::string fields = "\"ok\":true,\"media\":\"image/jpeg\",\"w\":" + std::to_string(tw) + ",\"h\":" + std::to_string(th) +
                         ",\"data\":" + util::Json(util::Base64(jpeg));
    if (toFile) fields += ",\"file\":" + util::Json(SaveFile(jpeg));
    Reply(id, fields);
}

} // namespace

void Capture(const std::string& id, bool toFile, HWND dock) {
    // Dans un thread : la page attend la réponse, et la capture ne doit pas bloquer l'interface.
    std::thread(Run, id, toFile, dock).detach();
}

} // namespace screen
