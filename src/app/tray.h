// Icône de la zone de notification et son des notifications. Plus de bulle : les notifications
// s'affichent dans la pastille du mini-dock (ui/js/dock/mini.js), l'appelant joue seulement le son.
#pragma once

#ifdef _WIN32
#include <windows.h>
#endif

#include <string>

namespace tray {

void Chime(); // son d'une notification (le réglage « son » est vérifié par l'appelant)

#ifdef _WIN32
constexpr UINT WM_APP_TRAY = WM_APP + 2; // callback des clics sur l'icône

// Crée l'icône (orbe dégradé généré à l'exécution) et l'ajoute à la zone de notification.
void Init(HWND window);
void Readd();  // après un redémarrage de l'Explorateur (message TaskbarCreated)
void Remove();
HICON Icon();
#else
void Init(); // icône StatusNotifierItem (D-Bus) et son menu
#endif

} // namespace tray
