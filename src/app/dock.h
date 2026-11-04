// La fenêtre du dock : verre acrylique, états (masqué, languette, barre, panneau),
// animations, détection de la souris sur le bord haut, raccourci et menu de l'icône.
// Windows : app/dock.cpp. Linux : linux/dock_linux.cpp (mêmes états, même protocole).
#pragma once

#ifdef _WIN32
#include <windows.h>
#endif

#include <string>

namespace dock {

#ifdef _WIN32
constexpr wchar_t kClassName[] = L"AeriaDock";
constexpr UINT WM_APP_SUMMON = WM_APP + 3; // envoyé par une 2e instance : ouvrir le panneau
constexpr UINT WM_APP_LISTEN = WM_APP + 6; // SetListeningAsync : WPARAM = 1 (écoute) ou 0
constexpr UINT WM_APP_DRAG = WM_APP + 7;   // drop_edge : des fichiers glissés arrivent au bord haut de l'écran
#endif

struct Config {
    int mode = 0;          // 0 = masqué + barre de progression, 1 = languette visible
    int revealDelay = 550; // ms (mode 0)
    int hideDelay = 700;   // ms
    int width = 680;       // DIP
    int panelH = 540;      // DIP
    bool dark = true;
    bool sound = true;
    bool miniLyrics = true; // mini-dock des paroles quand le dock est rangé
    bool fullscreenReveal = false; // la souris en haut de l'écran ouvre le dock devant une appli plein écran
};

#ifdef _WIN32
// Crée la fenêtre, l'icône de notification, le raccourci et les minuteries.
HWND Create(HINSTANCE instance);
#else
// window : fenêtre GTK du dock ; layout : GtkLayout qui la remplit ; view : la vue web, qui garde
// la taille du panneau complet (la fenêtre la « découpe »). Crée aussi le raccourci et les minuteries.
void Init(void* window, void* layout, void* view);
void Summon(); // 2e instance lancée : ouvrir le panneau
bool Pinned();
void TogglePinFromMenu(); // « Garder affiché » du menu de l'icône : épingle et montre la barre
#endif

const Config& Cfg();

// Lignes "clé=valeur" envoyées par la page (voir sendConfig dans ui/js/core/settings.js).
void ApplyConfig(const std::string& payload);

void OnPageReady();          // envoie l'état courant et joue l'intro au premier lancement
void NotifyState();          // {"t":"state",…} vers la page
void OpenPanel();            // ouvre et active le panneau
void SetListeningAsync(bool on); // commande vocale : encoche « j'écoute » (ignorée si la barre est déjà ouverte), depuis n'importe quel thread
void ShowBar();              // replie le panneau sur la barre
void HideByUser();           // Échap, raccourci… : pas de réapparition tant que la souris reste en haut
void SetPinned(bool pinned); // garder affiché
void HoldFor(int ms);        // empêche le repli automatique pendant ms
void RevealBriefly(int ms);  // fait descendre la barre (rappel qui sonne, minuteur…)
void RevealMusic(int ms, int widthDip); // nouveau morceau : seulement la carte musique (largeur selon le titre)
void SetMini(int widthDip);  // largeur du mini-dock (rappels, paroles) voulue par la page, 0 = aucun
void SetPill(int widthDip, int heightDip, bool keep); // pilule de la commande vocale : taille voulue par la page, 0 = fermée

} // namespace dock
