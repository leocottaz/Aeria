// Hébergement de WebView2 : création, fond transparent, liens externes, messages.
#pragma once

#include <windows.h>

#include <functional>
#include <string>

namespace webview {

// Posté à la fenêtre parente quand le processus de WebView2 meurt : appeler Restart().
constexpr UINT WM_APP_WEBVIEW_RESTART = WM_APP + 4;

using MessageHandler = std::function<void(const std::string& utf8)>;

// Charge WebView2Loader.dll, crée la vue (asynchrone) dans parent et affiche ui/index.html.
// Retourne false si le loader est introuvable.
bool Create(HWND parent, MessageHandler onMessage);

void SetBounds(const RECT& bounds);
// Caché avec suspend = false, la page reste éveillée (paroles à suivre pour le mini-dock).
void SetVisible(bool visible, bool suspend = true);
void Wake(); // réveille une page suspendue sans l'afficher
void Focus();
void PostJson(const std::wstring& json);
void Restart(); // recrée la vue (après la mort du processus du navigateur)
void Close();

// Économie de mémoire (--in-process-gpu) : ~200 Mo de moins, animations moins fluides.
// Enregistrée dans HKCU\Software\Aeria ; la changer recrée la vue.
bool LowMemory();
void SetLowMemory(bool on);

} // namespace webview
