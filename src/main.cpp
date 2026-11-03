// Aeria — dock en verre qui se cale en haut de l'écran.
// Point d'entrée : instance unique, initialisation des modules, boucle de messages.
// Vue d'ensemble des modules : docs/ARCHITECTURE.md.

#include "ai/api_key.h"
#include "app/autostart.h"
#include "app/dock.h"
#include "features/media.h"
#include "web/bridge.h"
#include "web/webview_host.h"

#include <windows.h>
#include <objbase.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    // Une seule instance : la seconde demande à la première d'ouvrir le panneau.
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\Aeria.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(dock::kClassName, nullptr)) {
            AllowSetForegroundWindow(ASFW_ANY);
            PostMessageW(other, dock::WM_APP_SUMMON, 0, 0);
        }
        return 0;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    api_key::Load();

    HWND window = dock::Create(instance);
    bridge::Init(window);
    webview::Create(window, bridge::HandleMessage);
    media::Start();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    media::Stop();
    CoUninitialize();
    if (single) CloseHandle(single);
    return 0;
}
