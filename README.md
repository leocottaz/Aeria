# Shadow Assistant

Dock en verre (acrylique Windows 11) qui descend du haut de l'écran : rappels, to-do, minuteur, notes, musique en cours avec paroles, et chat avec Claude.

Hôte natif **C++20** + interface **HTML/CSS/JS**, sans dépendance JavaScript ni étape de build web. Deux plateformes :

- **Windows** : Win32 + **WebView2**, fond acrylique.
- **Linux** (X11 / XWayland) : **GTK3 + WebKitGTK**, verre dessiné par la page sur une fenêtre transparente.

## Télécharger

Chaque version est publiée dans les **Releases** GitHub :

| Plateforme | Fichier | Comment |
| --- | --- | --- |
| Windows (recommandé) | `ShadowAssistant-vX.Y.Z-windows-x64-setup.exe` | Installateur : double-clic, sans droits administrateur. Menu Démarrer, désinstallation dans « Applications installées », mises à jour en réinstallant par-dessus. |
| Windows (portable) | `ShadowAssistant-vX.Y.Z-windows-x64.zip` | Décompresse et lance `ShadowAssistant.exe` ; `ui/` et les DLL restent à côté. |
| Debian, Ubuntu (recommandé) | `shadow-assistant_X.Y.Z_amd64.deb` | `sudo apt install ./shadow-assistant_X.Y.Z_amd64.deb` ; la commande `shadow-assistant` et l'entrée de menu apparaissent. |
| Autres distributions | `ShadowAssistant-vX.Y.Z-linux-x86_64.tar.gz` | Décompresse et lance `ShadowAssistant` ; `ui/` reste à côté. Paquets requis : GTK3, WebKitGTK 4.1, libcurl, libX11. |

Tes rappels, notes et clés sont conservés quand tu désinstalles ou mets à jour (`%APPDATA%` / `%LOCALAPPDATA%\ShadowAssistant` sous Windows, `~/.config` et `~/.local/share` sous Linux).

## Compiler et lancer

### Windows

Prérequis : Windows 10/11 x64, CMake ≥ 3.25, Ninja et MinGW-w64 (`g++`) dans le `PATH`. [MSYS2](https://www.msys2.org) fournit les trois (voir [CONTRIBUTING.md](CONTRIBUTING.md)).

```powershell
cmake --preset release
cmake --build --preset release
.\build\release\ShadowAssistant.exe
```

Le dossier de sortie contient `ShadowAssistant.exe`, `WebView2Loader.dll`, `libwinpthread-1.dll` et `ui/` : ils doivent rester ensemble.

### Linux

Paquets : `cmake ninja-build pkg-config libgtk-3-dev libwebkit2gtk-4.1-dev libcurl4-openssl-dev libx11-dev` (Arch : `gtk3 webkit2gtk-4.1 curl libx11`).

```bash
cmake --preset linux-release
cmake --build --preset linux-release
./build/linux-release/ShadowAssistant
```

Sous Wayland pur, le dock force le backend X11 (XWayland) ; sans lui, ni le bord haut de l'écran ni le raccourci global ne fonctionneraient. Sous GNOME, l'icône de la zone de notification demande l'extension « AppIndicator ».

### Mises à jour

L'application vérifie sur GitHub, au démarrage puis toutes les 6 h, s'il existe une version plus récente (Réglages → Système → Mises à jour pour la désactiver ou lancer une recherche). Si oui, une bannière propose **Mettre à jour**, **Plus tard** ou **Ignorer** : rien n'est téléchargé sans ton clic.

- **Windows (installée avec `Setup.exe`)** : un clic télécharge l'installateur de la release, vérifie son empreinte SHA-256 (fichier `SHA256SUMS` de la release), l'installe en silencieux et relance le dock. Un fichier dont l'empreinte ne correspond pas n'est jamais lancé.
- **Windows portable (ZIP) ou compilée par toi** : la bannière renvoie vers la page de la release. Un build local (`0.0.0`) ne vérifie rien.
- **Linux** : la bannière renvoie vers la page de la release ; installe le nouveau `.deb` (`sudo apt install ./…`) ou ton gestionnaire de paquets.

### Publier une version

```bash
git tag v0.1.0 && git push origin v0.1.0
```

GitHub Actions (`.github/workflows/release.yml`) compile les deux plateformes et attache à la Release l'installateur Windows, le ZIP, le `.deb` et le `tar.gz`. `ci.yml` vérifie que tout compile à chaque push.

Construire les installateurs à la main :

```powershell
# Windows : Inno Setup 6 (winget install JRSoftware.InnoSetup)
cmake --install build/release --prefix stage
& "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe" /DAppVersion=0.1.0 /DStageDir=stage /DOutDir=dist installer\windows\shadow-assistant.iss
```

```bash
# Linux : paquet .deb (disposition /opt + /usr/bin)
cmake -S . -B build-deb -G Ninja -DCMAKE_BUILD_TYPE=Release -DSHADOW_DEB_LAYOUT=ON -DSHADOW_VERSION=v0.1.0
cmake --build build-deb && (cd build-deb && cpack -G DEB)
```

## Utilisation

- **Ctrl+Alt+Espace** : ouvrir / fermer le panneau. **Échap** : replier. **Ctrl+1…6** : changer d'onglet.
- Icône dans la zone de notification : clic = ouvrir, clic droit = menu (garder affiché, réglages, quitter).
- Linux : mêmes fonctions, avec trois différences — pas de flou du bureau, pas de détection du plein écran, et la recherche de fichiers parcourt ton dossier personnel (noms de fichiers seulement). Les clés API sont dans `~/.config/ShadowAssistant` (fichiers lisibles par toi seul).
- Réglages → Comportement : mode **Masqué** (barre de progression au bord haut, durée réglable) ou **Languette**.
- Rappels : texte libre en français (« appeler Paul demain à 14h », « dans 20 min »), répétitions, notification Windows.
- Claude, trois modes de connexion (Réglages → Maya → Connexion) :
  - **Abonnement Pro** (par défaut) : le dock lance le CLI officiel `claude -p` (Claude Code, inclus dans Pro), sans outils, dans `%LOCALAPPDATA%\ShadowAssistant\claude-chat`. Les messages comptent dans les limites de l'abonnement, sans facturation API. Installation : `irm https://claude.ai/install.ps1 | iex` dans PowerShell, puis « Se connecter » dans les réglages.
  - **Clé API** : facturée à l'usage, chiffrée avec DPAPI dans `%APPDATA%\ShadowAssistant\apikey.bin`.
  - **Gratuit** : Gemini ou Groq avec votre propre clé ; sert aussi de secours si Claude échoue.
- Musique : quand Deezer joue, un mini-lecteur apparaît dans la barre et l'onglet **Musique** montre la pochette, la progression, les commandes et les **paroles** synchronisées (sources : [LRCLIB](https://lrclib.net), puis [lrcmux](https://lrcmux.dev) en secours, récupérées à la demande). Réglages → Musique : Deezer seulement ou toutes les sources.
- Claude peut créer des rappels, des notes et des tâches, chercher et ouvrir des fichiers, lancer des routines et le minuteur.

## Structure

```
src/                 hôte natif C++ (un dossier par domaine)
  main.cpp           point d'entrée
  app/               fenêtre du dock, icône de notification, démarrage auto
  web/               WebView2 + routeur des commandes de la page (bridge)
  ai/                Claude : Clé API (HTTPS), Abonnement Pro (claude -p), clés chiffrées
  features/          rappels, lecteur multimédia, actions sur le PC
  common/            utilitaires (UTF-8, JSON…)
  linux/             port Linux : fenêtre GTK, dock, pont, et un fichier *_linux.cpp par module
cmake/linux.cmake    cible Linux (GTK3 + WebKitGTK + libcurl), archive et paquet .deb
installer/           windows/shadow-assistant.iss (Inno Setup), linux/ (entrée de menu .desktop, icône SVG)
.github/workflows/   CI (Windows + Linux) et release sur tag
ui/                  interface (copiée à côté de l'exe à chaque build)
  index.html
  css/               base.css, puis un fichier par zone
  js/main.js         démarrage + routeur des messages du natif
  js/core/           pont avec le natif, état, réglages, utilitaires
  js/dock/           états visuels, onglets, mini-dock
  js/features/       rappels, to-do, notes, minuteur, musique, fichiers, routines
  js/chat/           conversation, transports (API, Pro, Gratuit), outils, prompts
tools/               scripts de développement (aperçu, DevTools, captures, mesures)
third_party/         SDK WebView2 (Microsoft.Web.WebView2 1.0.4314.35)
docs/ARCHITECTURE.md rôle de chaque module, protocole page ↔ natif, recettes
```

## Contribuer

Lis [CONTRIBUTING.md](CONTRIBUTING.md) (installation, conventions, tests) puis [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) avant de modifier quelque chose.
