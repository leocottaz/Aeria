// Mise à jour depuis les Releases GitHub.
//  - La PAGE cherche s'il existe une version plus récente (API GitHub) et affiche la bannière.
//  - Le NATIF ne fait que le travail sensible, à la demande de l'utilisateur (un clic) : télécharger
//    l'installateur de CETTE version dans le dépôt officiel, vérifier son SHA-256 d'après le fichier
//    SHA256SUMS de la même release, puis le lancer en silencieux. La page ne fournit qu'un numéro de
//    version (« v1.2.3 ») : jamais d'adresse ni d'empreinte.
// Avancement vers la page : {"t":"update","state":"downloading|verifying|installing|restarting|error",
// "pct":0-100,"msg":"…"}.
// Windows : features/updater.cpp. Linux : linux/updater_linux.cpp (pas d'installation automatique :
// le .deb demande root ; la page renvoie vers la page de la release).
#pragma once

#include <string>

namespace updater {

// Numéro compilé dans l'exécutable : « 0.1.0 », ou « 0.0.0 » pour un build de développement
// (l'updater est alors inactif).
const char* Version();

// Vrai si cette copie a été installée par le programme d'installation (Windows : unins000.exe à côté
// de l'exécutable). Une copie portable ou de développement n'est jamais mise à jour automatiquement.
bool CanSelfUpdate();

// « v1.2.3 » uniquement (trois nombres) : rejette tout le reste, notamment les chemins et URL.
bool IsValidTag(const std::string& tag);

// Fichier attendu dans la release : « Aeria-v1.2.3-windows-x64-setup.exe » ; vide hors Windows.
std::string AssetName(const std::string& tag);

// Contenu d'un SHA256SUMS (« <sha256>  <nom> » par ligne) → empreinte du fichier en minuscules,
// ou chaîne vide s'il est absent ou mal formé.
std::string ExpectedHash(const std::string& sums, const std::string& file);

// Télécharge, vérifie et lance l'installateur de la version tag. Asynchrone ; ne fait rien si la copie
// n'est pas installée (CanSelfUpdate) ou si une mise à jour est déjà en cours.
void Install(const std::string& tag);

} // namespace updater
