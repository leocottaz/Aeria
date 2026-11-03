# Contribuer à Aeria

Merci de ton aide ! Ce guide dit comment compiler, où ranger ton code et comment tester sans mauvaise surprise.
Le projet est en français : commentaires, interface, messages de commit.

## 1. Installer

- **Windows** : Windows 10/11 x64 avec le runtime WebView2 (déjà présent sur Windows 11). CMake ≥ 3.25, Ninja et MinGW-w64 (`g++` C++20). Le plus simple : [MSYS2](https://www.msys2.org), la chaîne utilisée par la CI. Dans son terminal **MINGW64** : `pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja mingw-w64-x86_64-gdb`, puis ajoute `C:\msys64\mingw64\bin` à ton `PATH`. N'importe quel éditeur convient. MSVC n'est pas testé.
- **Linux** : CMake ≥ 3.25, Ninja, `g++` (C++20), `pkg-config` et les paquets `libgtk-3-dev libwebkit2gtk-4.1-dev libcurl4-openssl-dev libx11-dev` (Arch : `gtk3 webkit2gtk-4.1 curl libx11`). Sous Windows, WSL avec WSLg convient pour compiler et lancer.

### Compiler

```powershell
cmake --preset release            # configure dans build/release
cmake --build --preset release    # compile et copie ui/ + les DLL à côté de l'exe
```

Autres presets : `debug`, `relwithdebinfo` (voir `CMakePresets.json`). Sous Linux :

```bash
cmake --preset linux-release
cmake --build --preset linux-release     # ./build/linux-release/Aeria
```

Les installateurs (`installer/`) se construisent comme dans le README (section « Publier une version ») ; teste-les avec un dossier d'installation temporaire (`/DIR=…`) et **sans ta vraie valeur de démarrage automatique** : l'installateur écrit la clé `Run\Aeria`, la même que celle de l'application. L'`AppId` du `.iss` ne doit jamais changer.

Une modification de `src/` doit **compiler sur les deux plateformes** (la CI le vérifie). Un module partagé ne contient pas de code Windows hors de `#ifdef _WIN32` ; sinon, il a une version dans `src/linux/`.

Si `g++` et `ninja` ne sont pas dans ton `PATH`, crée un `CMakeUserPresets.json` (ignoré par git) qui pointe vers ta toolchain, par exemple avec MSYS2 :

```json
{
  "version": 6,
  "configurePresets": [{
    "name": "local-release", "inherits": "release",
    "cacheVariables": {
      "CMAKE_CXX_COMPILER": "C:/msys64/mingw64/bin/g++.exe",
      "CMAKE_MAKE_PROGRAM": "C:/msys64/mingw64/bin/ninja.exe"
    }
  }],
  "buildPresets": [{ "name": "local-release", "configurePreset": "local-release" }]
}
```

**Ferme le dock avant de recompiler** : l'exécutable en cours est verrouillé.
Si tu ne modifies que `ui/`, un `cmake --build` suffit (il recopie l'interface), puis recharge la page (F5 dans le dock).

## 2. Où mettre mon code ?

Les deux côtés sont rangés par domaine. Commence toujours par l'interface : le natif ne sert qu'à ce qu'une page web ne peut pas faire (fenêtre, Windows, fichiers, réseau sans CORS, secrets).

| Je veux… | Fichiers |
| --- | --- |
| Changer l'apparence d'un onglet | `ui/css/<onglet>.css`, `ui/index.html` |
| Ajouter une fonctionnalité à l'interface | `ui/js/features/<nom>.js` (+ `ui/css/<nom>.css`), branchée dans `ui/js/main.js` |
| Ajouter un réglage | `DEFAULTS` dans `ui/js/core/settings.js` + `data-setting` dans la bonne page (`.set-page`) de `index.html` |
| Apprendre un nouvel outil à Claude | `ui/js/chat/tools.js`, `prompts.js`, `pro.js` |
| Demander quelque chose à Windows | un module dans `src/features/`, une commande dans `src/web/bridge.cpp` |
| La même chose sous Linux | `src/linux/<module>_linux.cpp` (même interface), la commande dans `src/linux/bridge_linux.cpp`, la ligne dans `cmake/linux.cmake` |
| Toucher à la fenêtre, aux animations, au raccourci | `src/app/dock.cpp` **et** `src/linux/dock_linux.cpp` |

Les recettes pas à pas (nouvelle commande native, nouveau message, nouveau réglage, nouvel outil) et le protocole complet page ↔ natif sont dans [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). **Si tu ajoutes une commande ou un message, mets à jour ses tableaux.**

## 3. Conventions

### C++ (`src/`)
- Un module = `dossier/nom.h` + `dossier/nom.cpp` et un `namespace`. Le `.h` est court : c'est l'API du module. L'état reste privé dans le `.cpp` (namespace anonyme).
- `#include` relatifs à `src/` (`#include "app/dock.h"`) : l'en-tête du module d'abord, puis ceux du projet triés, puis le système.
- Seul le thread UI touche la fenêtre et WebView2. Depuis un autre thread : `bridge::PostToPage(json)`.
- Chaînes : UTF-8 (`std::string`) dans le code, UTF-16 seulement à l'appel des API Windows (`util::Widen` / `util::Narrow`). JSON : `util::Json` pour échapper.
- Interfaces COM/WinRT déclarées à la main : dans un namespace **nommé**, jamais anonyme (plantage en Release, voir plus bas).
- 4 espaces, accolades sur la même ligne.

### Interface (`ui/`)
- Modules ES natifs, **aucune dépendance externe ni étape de build**. Ne pas ajouter de npm, de bundler ou de CDN.
- Une fonctionnalité exporte `setupX()` (évènements, appelée une fois) et `renderX()` (redessine).
- Réagir à un réglage : `onSettingChange(fn)` plutôt qu'un import croisé (évite les dépendances circulaires).
- Stockage : `store.get/set` (`core/util.js`), qui préfixe les clés par `aeria.`.
- CSS : variables de thème dans `base.css`, un fichier par zone. Pas de `filter: blur()` sur de grandes surfaces (coût GPU).
- 2 espaces, guillemets simples.

Le fichier `.editorconfig` règle l'indentation et les fins de ligne (LF) dans la plupart des éditeurs.

## 4. Tester

### L'interface seule (rapide, sans compiler)
```powershell
tools/dev/serve.ps1 -Root "$PWD\ui"    # http://localhost:8765/index.html
```
Sans hôte natif, la page passe en **aperçu** (panneau ouvert). Dans la console, `window.__preview.onNative({...})` simule un message du natif (voir `docs/ARCHITECTURE.md`, section Déboguer).

### Dans l'appli
Lance l'exe avec `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=--remote-debugging-port=9333`, puis `tools/dev/cdp.ps1 -Expr "…"` exécute du JS dans le dock. **Relance-le sans ce port à la fin** : DevTools empêche la mise en veille de la page et fausse les mesures de mémoire.

Autres scripts dans `tools/dev/` : captures (`shot.ps1`), mémoire (`mem.ps1`, `mem-flags.ps1`), fluidité (`frames.ps1`, `gpu.ps1`).

### Mises à jour
`powershell -File tools\test\updater\run.ps1` teste `src/features/updater.cpp` contre un serveur local et un faux `Setup.exe` (jamais GitHub, jamais ton installation). Ne définis **jamais** `AERIA_UPDATE_BASE` ni `AERIA_UPDATE_ALLOW_HTTP` dans un build livré : ils n'existent que pour ce banc d'essai. Le nom de l'installateur (`OutputBaseFilename` du `.iss`, `AssetName()`, `SHA256SUMS`) forme un contrat : change les trois ensemble.

### Règles de test
- **Musique** : n'envoie aucune commande (pause, suivant, position) à ta vraie lecture. Utilise `tools/dev/fake-session.ps1`, qui crée une session Windows muette, et vérifie que c'est bien elle qui est suivie avant chaque commande.
- **Paroles** : teste avec des lignes fictives (`window.__preview.previewLyrics([...])`). Ne mets jamais de vraies paroles dans le dépôt, une capture ou une issue.
- **Mode Pro** : chaque message consomme le quota de l'abonnement. Compile `tools/fake-claude/fake.cpp` en `claude.exe` et place-le en tête du `PATH` ; attention, `%USERPROFILE%\.local\bin\claude.exe` (le vrai) passe avant.
- **Clés** : ne commite jamais de clé API, même de test.
- **Linux** : même règle pour la musique (aucune commande `media` à ton vrai lecteur MPRIS) et pour `~/.config/Aeria`. Teste la recherche de fichiers avec un faux `HOME` (`HOME=$(mktemp -d)`), et l'icône avec un bus privé (`dbus-run-session`) et un faux `StatusNotifierWatcher`, pas avec ta session.

## 5. Pièges connus

- **Plantage en Release seulement (0xC0000005)** : interfaces COM/WinRT dans un namespace anonyme. GCC dévirtualise l'appel vers une adresse invalide. Diagnostic : preset `relwithdebinfo` + gdb.
- **`-static` ne lie pas** avec MinGW (`__intrinsic_setjmpex`) : `libwinpthread-1.dll` est livrée à côté de l'exe.
- **`std::ifstream(std::wstring)`** ne compile pas sous MinGW : passer par `std::filesystem::path`.
- **Dock caché = page en veille** : un `cdp.ps1` lancé à ce moment attend le réveil. Ouvre le dock avant.
- **Taille de la barre** : `kBarH`/`kInset` (`src/app/dock.cpp` et `src/linux/dock_linux.cpp`) et `--bar-h`/`--inset` (`ui/css/base.css`) doivent rester égaux.
- **Linux : jamais `file://` pour l'interface** : WebKit bloque les modules JS ; la page est servie en `aeria://app/…`. Ne nomme pas une fonction `Display()` dans un fichier qui inclut Xlib (masque le type `Display`).
- **Nouvel état du dock** : l'ajouter aussi à la liste `classList.remove('st-…')` de `setState` (`ui/js/dock/dock.js`).
- **`[hidden]`** est écrasé par `display:flex` : la règle globale `[hidden]{display:none!important}` de `base.css` s'en charge, ne la retire pas.

## 6. Proposer une modification

1. Une modification = un sujet. Petites contributions bienvenues.
2. Vérifie : `cmake --build --preset release` (et `linux-release` si tu touches à `src/` ou `ui/`) passe sans nouvel avertissement, l'aperçu s'ouvre sans erreur dans la console, la fonctionnalité marche dans le vrai dock.
3. Mets à jour `docs/ARCHITECTURE.md` si tu ajoutes un module, une commande, un message ou un réglage natif, et le `README.md` si l'utilisateur voit la différence.
4. Décris ce que tu as changé et comment tu l'as testé ; joins une capture pour un changement visuel.
