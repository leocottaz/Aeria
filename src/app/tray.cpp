#include "app/tray.h"

#include "common/util.h"

#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace tray {
namespace {

HWND g_window = nullptr;
HICON g_icon = nullptr;

HICON MakeOrbIcon(int size) {
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof bi;
    bi.bV5Width = size;
    bi.bV5Height = -size;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    auto* px = static_cast<uint32_t*>(bits);
    const double c = size / 2.0, r = size / 2.0 - 0.5;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            double dx = x + 0.5 - c, dy = y + 0.5 - c;
            double cov = std::clamp(r - std::sqrt(dx * dx + dy * dy) + 0.5, 0.0, 1.0);
            double t = (x + y) / (2.0 * size);
            double R = 150 + (80 - 150) * t, G = 110 + (210 - 110) * t, B = 255 + (250 - 255) * t;
            double hx = x - size * 0.34, hy = y - size * 0.3;
            double h = std::max(0.0, 1.0 - std::sqrt(hx * hx + hy * hy) / (size * 0.38)) * 0.65;
            R += (255 - R) * h, G += (255 - G) * h, B += (255 - B) * h;
            auto a = (uint32_t)(cov * 255);
            px[y * size + x] = (a << 24) | ((uint32_t)(R * cov) << 16) | ((uint32_t)(G * cov) << 8) | (uint32_t)(B * cov);
        }
    }
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

NOTIFYICONDATAW BaseData() {
    NOTIFYICONDATAW nid{sizeof nid};
    nid.hWnd = g_window;
    nid.uID = 1;
    return nid;
}

} // namespace

void Init(HWND window) {
    g_window = window;
    if (!g_icon) g_icon = MakeOrbIcon(64);
    Readd();
}

void Readd() {
    auto nid = BaseData();
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = g_icon;
    wcscpy_s(nid.szTip, L"Aeria — Ctrl+Alt+Espace");
    Shell_NotifyIconW(NIM_ADD, &nid);
}

void Remove() {
    auto nid = BaseData();
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

HICON Icon() {
    if (!g_icon) g_icon = MakeOrbIcon(64);
    return g_icon;
}

void Notify(const std::string& title, const std::string& text, bool sound) {
    auto nid = BaseData();
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON | (sound ? 0 : NIIF_NOSOUND);
    nid.hBalloonIcon = g_icon;
    wcsncpy_s(nid.szInfoTitle, util::Widen(title).c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo, util::Widen(text).c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

} // namespace tray
