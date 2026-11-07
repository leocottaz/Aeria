// Pont entre le natif et la page web.
//  - Page -> natif : chaînes "commande\nargument", routées par HandleMessage.
//  - Natif -> page : objets JSON {"t": "...", ...} postés par PostToPage.
// Le protocole complet est décrit dans docs/ARCHITECTURE.md.
#pragma once

#ifdef _WIN32
#include <windows.h>
#endif

#include <string>

namespace bridge {

// Envoie un JSON à la page. Utilisable depuis n'importe quel thread.
void PostToPage(std::string json);

// Comme PostToPage, mais réveille d'abord la page si elle dort (dock caché, Windows) : pour un message
// qui doit s'afficher tout de suite (notification). Utilisable depuis n'importe quel thread : le réveil
// (WebView2) se fait sur le thread UI. Sous Linux la page ne dort jamais.
void PostToPageAwake(std::string json);

// Commande reçue de la page.
void HandleMessage(const std::string& msg);

#ifdef _WIN32
constexpr UINT WM_APP_WEB = WM_APP + 1; // lParam = std::string* (JSON à remettre à la page), wParam = 1 : réveiller la page d'abord

// Fenêtre qui recevra WM_APP_WEB (le message est traité sur le thread UI).
void Init(HWND window);

// À appeler depuis la WndProc pour WM_APP_WEB.
void Deliver(WPARAM wake, LPARAM lp);
#endif

} // namespace bridge
