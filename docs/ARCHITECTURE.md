# Architecture

Aeria = un petit hôte natif **C++20** (`src/`) + une interface **HTML/CSS/JS** (`ui/`). Deux plateformes, **la même interface et le même protocole** :

| | Windows | Linux |
| --- | --- | --- |
| Hôte | Win32 (`src/app`, `src/web`, `src/ai`, `src/features`) | GTK3 (`src/linux/`) |
| Vue web | WebView2 | WebKitGTK (`webkit2gtk-4.1`) |
| Fenêtre | acrylique (DWM) | transparente, fond dessiné par la page (`html.linux`) |
| Build | `CMakeLists.txt` (MinGW) | `cmake/linux.cmake` (sélectionné quand `NOT WIN32`) |

- Le natif gère tout ce qu'une page web ne peut pas faire : la fenêtre et ses animations, la souris sur le bord de l'écran, le raccourci global, l'icône de notification, l'heure des rappels, la clé API, HTTPS et le lancement de Claude Code.
- La page gère l'affichage et les données de l'utilisateur (rappels, notes, réglages, conversation), stockées dans son `localStorage`.

Ils se parlent par messages (voir [Protocole](#protocole)). Le reste de ce document décrit le côté **Windows** ; la section [Linux](#linux) donne ce qui change.

```
 ┌──────────── ui/ (WebView2) ─────────────┐        ┌──────────────── src/ (C++) ─────────────────┐
 │ main.js ── route les messages natifs    │ "cmd\n │ web/bridge.cpp ── route les commandes       │
 │ dock/ · features/ · core/settings.js    │  arg"  │ app/dock.cpp      fenêtre, états, souris    │
 │ chat/chat.js ─┬─ chat/api.js  (clé)     │ ─────► │ ai/anthropic_http.cpp  HTTPS (Clé API)      │
 │               └─ chat/pro.js  (Pro)     │ ◄───── │ ai/claude_code.cpp     claude -p (Pro)      │
 │ core/bridge.js: send(), ask(), http     │  JSON  │ features/reminders.cpp, media.cpp, pc.cpp   │
 └─────────────────────────────────────────┘        └─────────────────────────────────────────────┘
```

## Organisation

Les deux côtés sont rangés de la même façon : un dossier par domaine.

| Domaine | Natif `src/` | Interface `ui/js/` |
| --- | --- | --- |
| Socle partagé | `common/` | `core/` (pont, état, réglages, utilitaires) |
| Fenêtre du dock | `app/` (fenêtre, icône, démarrage) | `dock/` (états visuels, onglets, mini-dock) |
| Lien page ↔ natif | `web/` (WebView2, routeur) | `core/bridge.js` |
| Claude et modèles | `ai/` | `chat/` |
| Fonctionnalités | `features/` (ce qui a besoin de Windows) | `features/` (un fichier par onglet ou outil) |

Le CSS reste à plat dans `ui/css/` : `base.css` (thème, verre, contrôles), puis un fichier par zone, du même nom que le module JS.

Les `#include` partent de `src/` : `#include "app/dock.h"`.

## Natif (`src/`)

Chaque module expose une petite interface dans son `.h` (un `namespace` : `dock::`, `webview::`, `bridge::`…) ; l'état reste privé dans le `.cpp`.

| Module | Rôle |
| --- | --- |
| `main.cpp` | Instance unique, initialisation, boucle de messages. |
| `app/dock.*` | Fenêtre acrylique, états (`hidden`, `progress`, `peek`, `mini`, `music`, `listen`, `bar`, `panel`), animations, détection de la souris, écran courant (rangé, le dock suit l'écran de la souris), repli auto, raccourci Ctrl+Alt+Espace, menu de l'icône, application des réglages. |
| `app/tray.*` | Icône de notification (générée à l'exécution) et son des notifications (`Chime`) : plus de bulle, les notifications s'affichent dans la pastille du mini-dock. |
| `app/drop_edge.*` | Windows : bande de 2 px au bord haut de l'écran (largeur du dock, `WS_EX_LAYERED` alpha 1, enregistrée comme `IDropTarget`). Quand des **fichiers** (`CF_HDROP` ou fichiers virtuels de navigateur) glissés entrent dedans, elle poste `WM_APP_DRAG` : `dock::OpenForDrag` ouvre le panneau sur le chat, **sans prendre le focus** (le glisser continue dans l'appli d'origine) ; le dépôt se fait ensuite sur la page. Elle ne rend rien (`DROPEFFECT_NONE`) et laisse passer les clics (`WS_EX_TRANSPARENT`), sauf bouton enfoncé (`Poll` la rend alors « solide » : un glisser est un bouton enfoncé). Linux : les bandes de `RebuildEdges` (XWayland seulement) acceptent `text/uri-list` et appellent `OpenForDrag`. |
| `app/autostart.*` | Clé `Run` pour le lancement au démarrage. |
| `web/webview_host.*` | Création de WebView2 (`--in-process-gpu` si l'économie de mémoire est activée, réglage dans `HKCU\Software\Aeria\LowMemory`), fond transparent, liens externes ouverts dans le navigateur, réception des messages, veille quand le dock est caché (`SetVisible(false)` : cible mémoire basse + `TrySuspend`), recréation si le processus meurt. |
| `web/bridge.*` | `PostToPage()` (depuis n'importe quel thread) et le routage des commandes de la page vers les modules. |
| `ai/anthropic_http.*` | HTTPS en flux (lignes complètes) : mode Clé API vers `api.anthropic.com`, mode Gratuit vers Groq ou Gemini (hôte fixe, préfixe de chemin imposé, clé ajoutée en `Bearer`). |
| `ai/claude_code.*` | Mode Abonnement Pro : lance `claude -p` sans outils (sauf `WebSearch` si la page le demande), sans `ANTHROPIC_API_KEY`, dans un dossier dédié ; relaie le stream-json. |
| `ai/api_key.*` | Clés chiffrées par DPAPI : Anthropic, et Groq / Gemini pour le mode Gratuit. Jamais envoyées à la page (seuls les 4 derniers caractères). |
| `features/reminders.*` | Liste synchronisée depuis la page ; vérifie les échéances chaque seconde et déclenche notification + apparition du dock. Tient aussi la fin du minuteur (`SetTimerEnd`). |
| `features/media.*` | Lecteur : thread qui interroge les contrôles multimédias de Windows (GlobalSystemMediaTransportControls) toutes les 400 ms, choisit la session (Deezer, ou toutes les sources), lit titre/pochette/position et exécute lecture/pause/suivant/précédent/position. Interfaces WinRT déclarées à la main (voir l'avertissement en tête de fichier). |
| `features/pc.*` | Actions sur le PC : recherche de fichiers (index Windows Search via OLE DB, dossier de l'utilisateur hors AppData, mots nettoyés), ouverture d'un résultat (seulement un chemin déjà renvoyé ; exécutables et scripts → dossier sélectionné), routines (thread : ouvrir, volume Core Audio, musique, attendre), fenêtre « Ouvrir ». |
| `features/screen.*` | Capture d'écran sur demande de la page (commande `screenshot`, jamais en continu). Windows : `StretchBlt` de l'écran du dock réduit à 1568 px, JPEG par GDI+, dans un thread ; le dock reste visible mais est **absent de l'image** (`SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)` le temps de la capture). Linux (`screen_linux.cpp`) : racine X11, dock rendu transparent 150 ms ; **refusé sous Wayland** (image noire). Avec `file`, écrit aussi le JPEG dans `claude-chat/screens/` (une seule capture gardée) pour que Claude Code le lise avec `Read`. |
| `features/updater.*` | Mise à jour (voir [Mises à jour](#mises-à-jour)) : `updater_common.cpp` (version compilée, validation du tag, lecture de `SHA256SUMS`), `updater.cpp` (Windows : téléchargement WinHTTP, SHA-256 CNG, lancement de `Setup.exe`). |
| `features/appnotifs.*` | Notifications des autres applis (Windows) : `UserNotificationListener`, relevé toutes les 1,5 s dans un thread (l'évènement `NotificationChanged` est réservé aux applis empaquetées), vtable du listener déclarée à la main (namespace nommé), le reste vient des en-têtes MinGW. Les notifications déjà présentes au démarrage de l'écoute sont ignorées. `linux/appnotifs_linux.cpp` : bouchon (`unsupported`). |
| `features/voice.*` | Commande vocale « Aeria » (voir [Voix](#voix)) : `voice.cpp` (partagé : découpage du micro en phrases, whisper.cpp, détection du mot), `voice_win.cpp` (micro waveIn, téléchargement du modèle WinHTTP + SHA-256 CNG), `linux/voice_linux.cpp` (micro `parec`/`arecord`, libcurl). |
| `common/util.h` · `util.cpp` · `util_common.cpp` | JSON, découpage, base64 (`util_common.cpp`, commun) ; UTF-8/UTF-16 et dossiers Windows (`util.cpp`). |

Les `.h` partagés (`dock.h`, `bridge.h`, `tray.h`, `pc.h`, `util.h`) gardent ce qui touche à Windows derrière `#ifdef _WIN32`.

Les threads réseau et processus ne touchent jamais la fenêtre ni WebView2 : ils passent par `bridge::PostToPage`, qui poste un message à la fenêtre (thread UI).

## Interface (`ui/`)

Modules ES (`<script type="module" src="js/boot.js">`, qui charge d'abord les données du natif puis importe `main.js`), sans dépendance externe ni étape de build. Chaque module de fonctionnalité exporte `setupX()` (branche les évènements, appelé une fois par `main.js`) et `renderX()` (redessine).

| Fichier | Rôle |
| --- | --- |
| `js/boot.js` | Demande les données au natif (`loadstore`), attend le message `store` (8 s au plus), puis importe `main.js` : les modules lisent leurs données dès leur import. |
| `js/main.js` | Démarrage et routage des messages du natif (`onNative`). |
| `js/core/bridge.js` | `send(cmd, …args)`, `ask(cmd, args)` (promesse résolue par `{t:"reply"}`), `http` (requêtes en cours par id), messages d'erreur API. |
| `js/core/state.js` | État partagé `S` (état du dock, onglet, connexion). |
| `js/core/settings.js` | Réglages persistés, apparence, envoi au natif. `onSettingChange(fn)` pour réagir sans dépendance circulaire. |
| `js/core/format.js`, `js/core/util.js` | Dates, icônes, stockage (`store`, clés préfixées `aeria.`), toast… |
| `js/dock/dock.js` | États visuels, onglets (`NAV_TABS` = boutons de la barre ; Claude n'en a pas : orbe, champ « Demander », Ctrl+Alt+Espace), horloge, barre, raccourcis clavier. |
| `js/dock/mini.js` | Mini-dock (dock rangé) : cloche + nombre de rappels sonnés (`setMiniAlerts`, appelé par `tick`), temps du minuteur (`setMiniTimer`), ligne de paroles (`setMiniLyric`), notifications (`showMiniNotice`, `notify` : texte à la place des paroles pendant 5 à 10 s, décompté pastille visible) ; calcule la largeur et l'envoie au natif (`mini`). |
| `js/features/reminders.js` | Rappels : saisie en français, liste, alertes. |
| `js/features/notes.js` | Notes : cartes en grille ; un clic ouvre la note en plein panneau (`#noteEditor` : titre, mise en forme, thème de fond). Contenu en HTML limité (`sanitize`), `{ id, title, html, theme, updated }`. |
| `js/features/todo.js` | To-do : tâches à cocher, étoile (en tête de liste), modification par double-clic, section « Faites ». |
| `js/features/timer.js` | Minuteur : temps limité (compte à rebours) ou illimité (chronomètre), calculé par horodatages ; texte de la barre (`timerLabel`), temps dans le mini-dock si l'option `timerPill` est active (`updatePill`, calé sur le changement de seconde). |
| `js/features/music.js` | Mini-lecteur de la barre et onglet Musique : position interpolée, barre de progression, couleur de la pochette, paroles LRCLIB avec lrcmux en secours (synchronisées au format LRC, ou texte simple), ligne en cours pour le mini-dock (`updateMini`). |
| `js/features/files.js` | Recherche de fichiers pour Claude : résultats rangés par recherche (cartes cliquables), ouverture par numéro dans la dernière recherche, contexte du mode Pro. |
| `js/features/screen.js` | Outil `screenshot` : `takeScreenshot` (commande `screenshot`), miniatures gardées en mémoire pour la carte du chat (`shotFor`), jamais enregistrées. Réglage `screenAccess` ; `screenCapable` (settings.js) dit si le mode de connexion lit les images (Pro, Clé API, Gemini, OpenAI ; pas Groq). Mode Clé API / Gratuit : résultat d'outil avec image (Gratuit : image dans un message utilisateur qui suit) ; mode Pro : ligne `[[ecran|raison]]` → capture dans `screens/` → second tour automatique où Claude lit le fichier avec `Read`. |
| `js/features/updater.js` | Mises à jour : interroge l'API GitHub (`releases/latest`) au démarrage puis toutes les 6 h, compare les versions, affiche la bannière (`#updateBanner`, visible panneau ouvert) et la carte des Réglages → Système ; envoie `update` au natif au clic. |
| `js/features/appnotifs.js` | Notifications des autres applis : réglages (Système › Général : `appNotifs`, `appNotifsText`), message `appnotif` → pastille du mini-dock (`showMiniNotice`) ou toast si le dock est ouvert, statut `appnotifs`. |
| `js/features/voice.js` | Voix : page « Voix » des Réglages (état, téléchargement du modèle), champ « Demander » éclairé pendant l'écoute, envoie à Aeria la phrase reçue (`heard`). |
| `js/features/modes.js` | Modes : données (`aeria.modes`, anciennes routines reprises), éditeur des Réglages, démarrage (étapes de démarrage envoyées au natif par `routine`), déclencheur « ouverture d'une appli » (`apps` du mode, `syncWatch` → `watch`, `onAppStart` ← `appstart`), mode en cours (`aeria.activeMode`) avec sa pilule dans la barre, comportements `BEHAVIORS` (réglages imposés via `setOverrides`/`eff` de `settings.js`), recherche par nom pour Claude. |
| `js/chat/connection.js` | Section « Connexion » des Réglages : statut de Claude Code, clé API, clés gratuites. |
| `js/chat/chat.js` | Conversation : état (`convo`, `turn`), envoi, évènements de streaming, rendu. |
| `js/chat/attach.js` | Fichiers joints : trombone, glisser-déposer sur tout le dock, collage ; la page lit elle-même les fichiers (File API, rien ne passe par le natif). Images (réduites à 1568 px), PDF (8 Mo), texte/code (100 Ko), 5 au plus ; blocs `image` / `document` / `<fichier nom="…">` ajoutés au message. `unsupported` refuse ce que le mode ne lit pas (Pro : texte seulement ; Groq : pas d'image ; PDF : mode Clé API). `forStorage` retire le binaire de la conversation enregistrée. |
| `js/chat/api.js` | Transport mode Clé API (API Messages + outils, dont l'outil serveur `web_search` quand le réglage `webSearch` est actif : blocs `server_tool_use` / `web_search_tool_result`, reprise sur `pause_turn`). Les résultats de recherche ne sont pas gardés dans la conversation enregistrée (`slim`, `chat.js`). |
| `js/chat/free.js` | Transport mode Gratuit (Gemini, Groq ; API compatibles OpenAI) : conversion de la conversation (format Claude ↔ OpenAI), outils, choix automatique du modèle dans la liste du service. Sert aussi de secours (`tryFallback` dans `chat.js`) quand Claude échoue. |
| `js/chat/pro.js` | Transport mode Pro (Claude Code) + exécution des lignes `[[rappel|…]]`, `[[note|…]]`, `[[tache|…]]` (`[[tache!|…]]` = importante), `[[fait|…]]`, `[[chercher|mots|ext|du|au]]`, `[[ouvrir|n]]`, `[[site|https://…]]`, `[[mode|Nom]]` (`[[mode|stop]]` = arrêter le mode) (`actionToTool`). |
| `js/chat/prompts.js` | Prompt système (recalculé à chaque message, jamais gardé dans la conversation), bloc `[Contexte]` (date, heure, morceau en cours, modes et mode en cours) ; en mode Pro, listes des rappels, des tâches et de la dernière recherche de fichiers. |
| `js/chat/tools.js` | Outils (`create_reminder`, `list_reminders`, `create_note`, `create_todo`, `list_todos`, `complete_todo`, `search_files`, `open_file`, `open_url` (page web dans le navigateur : http(s) seulement, via la commande `openurl`), `start_mode`, `stop_mode`, `timer`) et leur exécution (`execTool`, asynchrone). |
| `js/chat/markdown.js` | Markdown minimal et sûr. |
| `css/*.css` | `base` (thème, verre, contrôles), `dock`, puis un fichier par onglet. |

## Protocole

**Page → natif** : `send('commande', arg1, arg2…)` envoie la chaîne `"commande\narg1\narg2…"`.

| Commande | Arguments | Effet |
| --- | --- | --- |
| `ready` | — | La page est chargée : le natif répond `init` puis `state`. |
| `config` | lignes `clé=valeur` | `mode`, `revealDelay`, `hideDelay`, `width`, `panelH`, `theme`, `sound`, `autostart`, `fullscreenReveal` (Windows : le bord haut ouvre le dock devant une appli plein écran), `miniLyrics` (garde la page éveillée, dock caché, pendant une lecture : pilule de paroles ou carte musique). |
| `loadstore` | — | Démarrage de la page (avant `ready`) : le natif répond `store` avec tous les fichiers de données. |
| `savestore` | clé, JSON | Écrit `data/<clé>.json` (`common/data_store.*`). Refusé (message `storefail`) si la clé est inconnue, le JSON invalide ou le disque en échec. |
| `open` / `bar` / `hide` | — | Ouvre le panneau / replie sur la barre / masque. |
| `pin` | `0`/`1` | Garder affiché. |
| `hold` | ms | Empêche le repli automatique pendant ms. |
| `reveal` | ms | Fait descendre la barre pendant ms (sauf devant une appli plein écran). |
| `revealmusic` | ms, largeur (DIP) | Nouveau morceau : fait descendre seulement la carte musique (état `music`, 280 à 380 × 76 selon le titre) pendant ms. |
| `pill` | `largeur,hauteur,garder` (DIP, 0/1) | Pilule « Aeria » (état `listen`) : taille voulue par la page (`js/features/pill.js`, mesurée sur son contenu) ; `garder=1` la laisse ouverte même quand la voix a fini d'écouter (la réponse s'y affiche) ; hauteur `0` = la page la referme. Bornée à 520 × 300. |
| `mini` | largeur (DIP), `1` si la pastille montre une notification, hauteur (DIP, carte de notification), séparés par une virgule : `328,1,59` | Mini-dock (rappels sonnés, minuteur, paroles, notification) : quand le dock est rangé, la fenêtre devient une pastille de cette largeur. `0` = rien à montrer. Avec `,1` elle s'affiche aussi devant une appli plein écran. |
| `media` | `toggle`, `next`, `prev`, `seek\n<ms>`, `sources\n<deezer\|all>`, `refresh` | Pilote le lecteur / choisit la source / redemande l'état complet. |
| `lowmem` | `0`/`1` | Économie de mémoire (`--in-process-gpu`) ; si elle change, la vue est recréée. |
| `reminders` | lignes `id\tdueMs\ttexte` | Rappels à venir. |
| `timer` | `finMs` (0 = aucun) | Fin du compte à rebours du minuteur. |
| `chime` | — | Joue le son d'une notification (si le réglage « son » est actif). Le texte, lui, est affiché par la page. |
| `key` / `delkey` | clé / — | Enregistre / supprime la clé API. |
| `http` | id, méthode, chemin, betas, corps | Requête vers l'API (mode Clé API). |
| `fhttp` | id, service (`gemini`/`groq`), méthode, chemin, corps | Requête vers le modèle gratuit (mêmes réponses `chunk` / `done` / `error`). |
| `freekey` / `delfreekey` | service, clé / service | Enregistre / supprime la clé gratuite → `freekey`. |
| `cc` | `id␟modèle␟effort␟session␟outils␟système␟message` (séparateur `\x1f`) | Message via Claude Code (mode Pro). `outils` : vide (aucun outil), `web` (`WebSearch`) et/ou `screen` (`Read` limité à `screens/**`), séparés par une virgule, avec `--allowedTools` ; le natif n'accepte rien d'autre. Le flux contient alors un `tool_use` `WebSearch` avant la réponse. |
| `ccstatus` / `cclogin` | — | Cherche Claude Code / ouvre une console de connexion. |
| `cancel` | id | Annule une requête `http` ou `cc`. |
| `openurl` | URL http(s) | Ouvre le navigateur. |
| `update` | tag `v1.2.3` | Télécharge, vérifie et lance l'installateur de cette version → messages `update`. Le natif refuse tout ce qui n'est pas `vN.N.N`. |
| `voice` | `status` · `download[:id]` · `delete:id` · `listen` · `cancel` | Voix : renvoie l'état, télécharge un modèle (`base` 60 Mo, `small` 190 Mo, `medium` 540 Mo ; SHA-256 vérifié) ou le supprime, écoute la prochaine phrase sans attendre « Aeria » (boutons micro ; avec l'écoute permanente éteinte, le micro ne reste ouvert que le temps de cette phrase) ou arrête cette écoute. L'interrupteur et le modèle choisi passent par `config` (`voiceWake=1`, `voiceModel=small`). |
| `search` | id, mots, extension, du, au | Recherche de fichiers → `reply` avec `results` (`path`, `name`, `modified`, `size`). |
| `openfound` | chemin | Ouvre un fichier déjà renvoyé par `search` (sinon rien). |
| `routine` | lignes `type\tvaleur` (`open`, `volume`, `mute`, `music`, `wait`, `lock`) | Exécute les étapes de démarrage d'un mode dans un thread natif (le mode « en cours » et ses comportements restent côté page). |
| `openapp` | identifiant d'appli (AUMID) | Clic sur la carte d'une notification d'appli : ouvre ou ramène l'appli (`shell:AppsFolder\<AUMID>`). Refusé si cet identifiant n'a jamais été vu dans une notification. L'action exacte de la notification (conversation, lien) n'est pas lisible par l'API. |
| `appnotifs` | `1` ou `0` | Démarre ou arrête l'écoute des notifications des autres applis (réglage `appNotifs`, désactivé par défaut). Renvoie `appnotifs` (statut), puis `appnotif` à chaque nouvelle notification. |
| `watch` | noms d'exécutable, un par ligne (jamais un chemin ; vide = rien) | Applis qui déclenchent un mode : `pc::WatchApps`, relevé des processus chaque seconde par `pc::CheckApps` (même minuterie que les rappels, thread de l'interface). Seule une ouverture **après** la commande déclenche ; ce qui tourne déjà est ignoré. Renvoie `appstart`. |
| `volume` | 0 à 100 | Volume de la sortie audio par défaut. |
| `pickfile` | id | Fenêtre « Ouvrir » → `reply` avec `path` (vide si annulé). |
| `screenshot` | id, `data` ou `file` | Capture l'écran du dock (sans le dock) → `reply` avec `data` (JPEG en base64), `media`, `w`, `h`, et `file` (chemin relatif `screens/screen-N.jpg`) si `file`. |
| `quit` | — | Quitte. |

**Natif → page** : objets JSON avec un champ `t`.

| `t` | Champs | Sens |
| --- | --- | --- |
| `init` | `hasKey`, `keyHint`, `autostart`, `lowMemory` (Windows), `freeKeys`, `version`, `canUpdate` | Réponse à `ready`. Sans `lowMemory`, la page garde son réglage. `autostart` = clé de démarrage présente **ou** demandé par `settings.json` (`adoptNative`, `settings.js`). Avant `init`, `config` n'envoie pas `autostart`. `version` : numéro de l'exécutable (`0.0.0` = build de développement). `canUpdate` : copie installée par `Setup.exe`. |
| `update` | `state` (`downloading`, `verifying`, `installing`, `restarting`, `error`), `pct`, `msg` | Avancement d'une mise à jour. Sur `restarting`, la page envoie `quit` : l'installateur relance le dock. |
| `voice` | `state` (`off`, `nomodel`, `downloading`, `loading`, `idle`, `awake`, `working`, `partial`, `error`, `heard`), `pct`, `msg`, `text`, `models`, `model` | État de l'écoute. `partial` : `text` est ce qu'on dit, transcrit en direct (brouillon). `heard` : `text` est la phrase finale, la page décide quoi en faire (pilule, champ d'un bouton micro, ou conversation). `awake` / `working` : la pilule est ouverte. |
| `freekey` | `service`, `ok`, `freeKeys` | Résultat de `freekey` / `delfreekey`. |
| `state` | `s`, `delay`, `pinned` | Nouvel état du dock. |
| `key` | `ok`, `hasKey`, `keyHint` | Résultat de `key`/`delkey`. |
| `chunk` | `id`, `data` | Lignes de flux (SSE en mode API, stream-json en mode Pro). |
| `done` | `id`, `cancelled`, (`exit`, `stderr` en mode Pro) | Fin d'une requête. |
| `error` | `id`, `status`, `body` | Échec (`-1` pas de clé, `-3` Claude Code introuvable, `0` réseau, sinon HTTP). |
| `ccstatus` | `found`, `path`, `version` | Résultat de `ccstatus`. |
| `fire` | `id` | Un rappel sonne. |
| `timerdone` | — | Le compte à rebours du minuteur est fini. |
| `appstart` | `app` (nom de l'exécutable, minuscules) | Une appli surveillée (commande `watch`) vient de s'ouvrir : la page démarre le mode qui l'attend (`onAppStart`, `modes.js`). Windows : la page est d'abord sortie de veille. |
| `media` | `active`, `app`, `title`, `artist`, `album`, `status`, `pos`, `dur`, `at`, `canNext`, `canPrev`, `canSeek`, (`art` : data URL, seulement au changement de morceau) | Musique suivie. `pos` est la position (ms) à l'instant `at` (ms epoch). |
| `appnotifs` | `status` (`allowed`, `denied`, `unsupported`, `error`) | Résultat du démarrage de l'écoute des notifications des autres applis. Sur tout sauf `allowed`, la page désactive le réglage. |
| `appnotif` | `app`, `title`, `text`, `icon` (data URL PNG du logo, vide si introuvable), `aumid` | Une notification d'une autre appli vient d'arriver : la page l'affiche en carte (logo, titre, texte dessous) (la page est d'abord sortie de veille : `PostToPageAwake`). |
| `hotkey`, `show` | (`tab`) | Ouverture par raccourci ou menu de l'icône. |
| `reply` | `id`, `ok`, (`error`, `results`, `path`) | Réponse à une commande `ask()` (`search`, `pickfile`). |
| `wheel` | `x`, `y`, `dy` | Linux : molette, en crans, à la position (`x`, `y`) de la page. |

## Données

| Quoi | Où |
| --- | --- |
| Réglages, rappels, to-do, notes, modes (et mode en cours), minuteur, conversation | Fichiers JSON `%APPDATA%\Aeria\data\<clé>.json` (Linux : `$XDG_DATA_HOME/Aeria/data`) : `settings`, `reminders`, `todos`, `notes`, `modes`, `activeMode`, `timer`, `convo`, `welcomed`, `lastSeenVersion`. Liste dans `kKeys` (`data_store.cpp`) et `FILE_KEYS` (`util.js`), à garder identiques. Écriture atomique ; sauvegarde quotidienne (7 jours) dans `data\backups\` pour réglages, rappels, notes, to-do, conversation, modes ; un fichier abîmé devient `<clé>.corrupt.json` et la dernière sauvegarde valide est reprise. Pas chiffrés (lisibles et modifiables à la main). |
| Caches et état secondaire (dernières recherches de fichiers, modèles gratuits, paroles introuvables, vérification des mises à jour) | `localStorage` de WebView2 : `%LOCALAPPDATA%\Aeria\WebView2` (clés `aeria.*`). Les anciennes données du `localStorage` sont reprises une fois dans les fichiers au premier démarrage. |
| Clé API (DPAPI) | `%APPDATA%\Aeria\apikey.bin` |
| Clés gratuites (DPAPI) | `%APPDATA%\Aeria\geminikey.bin`, `groqkey.bin` |
| Dossier de travail de Claude Code | `%LOCALAPPDATA%\Aeria\claude-chat` |
| Lancement au démarrage | `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\Aeria` |
| Économie de mémoire | `HKCU\Software\Aeria\LowMemory` (DWORD, 1 par défaut) |

## Linux

Cible séparée : `cmake/linux.cmake` (GTK3, WebKitGTK, libcurl, X11). Les modules ont **la même interface** (`.h`) que sous Windows, avec une implémentation dans `src/linux/`. Sont communs : `common/util_common.cpp` et `features/reminders.cpp`.

| Fichier `src/linux/` | Rôle (équivalent Windows) |
| --- | --- |
| `main_linux.cpp` | Fenêtre GTK, vue WebKitGTK, schéma `aeria://app/` (voir ci-dessous), instance unique par D-Bus (`GtkApplication`), `host::` (`Quit`, `OpenUrl`, `EvalJson`, `ChooseFile`). (`main.cpp`, `webview_host.cpp`) |
| `dock_linux.cpp` | Mêmes états, animations et repli que `app/dock.cpp`. Souris lue au pointeur global X11, Ctrl+Alt+Espace par `XGrabKey`, `reminders::Check()` chaque seconde. |
| `bridge_linux.cpp` | Routeur des commandes de la page ; `PostToPage` remet le JSON sur la boucle GTK (`g_idle_add`). (`web/bridge.cpp`) |
| `tray_linux.cpp` | Icône **StatusNotifierItem** en D-Bus (clic gauche = panneau, clic droit = menu GTK) et notifications `org.freedesktop.Notifications`. (`app/tray.cpp`) |
| `anthropic_http_linux.cpp` | HTTPS en flux avec libcurl, mêmes règles (hôtes fixes pour le mode Gratuit). (`ai/anthropic_http.cpp`) |
| `claude_code_linux.cpp` | `claude -p` par `fork`/`exec`, groupe de processus tué à l'annulation. (`ai/claude_code.cpp`) |
| `api_key_linux.cpp` | Clés dans `~/.config/Aeria`, fichiers `0600` (pas de trousseau pour l'instant). (`ai/api_key.cpp`) |
| `media_linux.cpp` | Lecteur via **MPRIS2** (D-Bus) : mêmes messages `media` ; pochettes converties en data URL. (`features/media.cpp`) |
| `pc_linux.cpp` | Recherche = parcours de `$HOME` (noms seulement, sans dossiers cachés ni `node_modules`) ; `xdg-open` ; volume `wpctl`/`pactl`. (`features/pc.cpp`) |
| `autostart_linux.cpp` | `~/.config/autostart/aeria.desktop`. (`app/autostart.cpp`) |
| `util_linux.cpp` | Dossiers XDG, dossier de l'exécutable. (`common/util.cpp`) |
| `host.h` | Petite interface entre `bridge_linux`/`pc_linux`/`tray_linux` et la fenêtre GTK. |

**Page** : `ui/js/core/bridge.js` détecte `window.webkit.messageHandlers.aeria` ; le natif répond en appelant `window.__fromNative(json)`. Il ajoute la classe `linux` à `<html>` (pas d'évasements, pas de marge cachée : `--inset` = `kInset` = 0, KWin refuse les positions négatives). Le verre arrondi est `.ambient`, comme sous Windows, et la fenêtre est en plus découpée par une forme X11 (`ApplyShape`).

**Molette** : interceptée dans GDK (`ForwardScroll`) et remise à la page par `{t:"wheel"}` (`onWheel`). `AERIA_DEBUG=1` affiche sur la sortie d'erreur l'environnement graphique et chaque cran reçu.

**Souris sous Wayland** : XWayland ne connaît la position que lorsque le curseur survole une de nos fenêtres. `dock_linux.cpp` pose une bande invisible de 1 px en haut de chaque écran (`RebuildEdges`) et suit les entrées/sorties (`PointerKnown`) ; hors de nos fenêtres, la position est ignorée. Sous X11, le pointeur global suffit.

**Interface servie en `aeria://app/…`**, pas en `file://` : WebKit refuse les modules JavaScript chargés depuis `file://`. `ServeUi` ne sert que `ui/` (chemins normalisés, pas de sortie du dossier). Équivalent de `https://aeria.local/` avec WebView2.

**Fenêtre** : un `GtkLayout` (taille minimale nulle) contient la vue, qui garde la taille du panneau complet ; la fenêtre la découpe, comme sous Windows.

Limites connues (voir aussi `Pièges` dans les notes) :
- Backend GDK forcé sur **X11/XWayland** (`gdk_set_allowed_backends("x11,wayland")`) : Wayland ne permet ni de lire la souris au bord de l'écran, ni de placer la fenêtre, ni de capter un raccourci global.
- Pas d'acrylique, pas de détection du plein écran (`ForegroundIsFullscreen` renvoie toujours faux), pas de mode « économie de mémoire » (`lowmem` ignoré).
- GNOME n'affiche l'icône qu'avec l'extension AppIndicator.
- Commandes **non portées** : aucune ; toutes celles du tableau Protocole sont routées par `bridge_linux.cpp`, sauf `lowmem` (ignorée).

### Données sous Linux

| Quoi | Où |
| --- | --- |
| `localStorage` de la page | `~/.local/share/Aeria/localstorage` (cache WebKit : `~/.cache/Aeria`) |
| Clés (Anthropic, Groq, Gemini) | `~/.config/Aeria/apikey`, `groqkey`, `geminikey` (0600) |
| Dossier de travail de Claude Code | `~/.local/share/Aeria/claude-chat` |
| Lancement à l'ouverture de session | `~/.config/autostart/aeria.desktop` |

## Mises à jour

**Répartition** : la page cherche et propose ; le natif ne fait que ce qui est sensible, **au clic** de l'utilisateur.

1. `ui/js/features/updater.js` appelle `https://api.github.com/repos/leocottaz/Aeria/releases/latest` (au démarrage si la dernière vérification date de plus de 6 h, puis toutes les 6 h ; réglage « Vérifier automatiquement »). Ignorés : brouillons, préversions, tags qui ne sont pas `vN.N.N`, version déjà ignorée. Une notification native est envoyée une seule fois par version.
2. Sans clic, rien n'est téléchargé. Au clic sur « Mettre à jour », la page envoie `update\n<tag>`.
3. `updater::Install` (Windows) : refuse si le tag n'est pas `vN.N.N` ou si la copie n'a pas été installée par `Setup.exe` (`unins000.exe` absent) ; télécharge `SHA256SUMS` puis `Aeria-<tag>-windows-x64-setup.exe` **depuis `https://github.com/leocottaz/Aeria/releases/download/<tag>/` uniquement** (la page ne fournit ni URL ni empreinte), vérifie le SHA-256 (CNG), supprime le fichier et s'arrête s'il ne correspond pas, sinon lance `Setup.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /RELAUNCH=1`.
4. `Setup.exe` arrête le dock installé, remplace les fichiers, puis le relance (entrée `[Run]` conditionnée par `/RELAUNCH=1`).

Garde-fous : HTTPS seulement, taille limitée (300 Mo), redirections suivies sauf HTTPS → HTTP. **Limite connue** : l'empreinte vient de la même release que le fichier ; un compte GitHub compromis pourrait publier les deux. Une vraie protection demande une signature par une clé gardée hors de GitHub (à faire), et `Setup.exe` n'est pas signé (SmartScreen).

**Version** : `AERIA_VERSION` (CMake) → `AERIA_VERSION_STR`, sans le `v`. Un build local donne `0.0.0` : la vérification est désactivée (carte « développement »). `release.yml` génère `SHA256SUMS` pour tous les fichiers publiés ; le nom de l'installateur est un contrat entre `aeria.iss` (`OutputBaseFilename`), `AssetName()` et ce fichier.

**Linux** : pas d'installation automatique (le `.deb` demande root ; les autres distributions ont leur gestionnaire de paquets). La page affiche la nouvelle version et le bouton « Télécharger » ouvre la page de la release (`canUpdate` est faux).

**Tester** : `powershell -File tools\test\updater\run.ps1` compile le vrai `updater.cpp` avec un serveur local (macros `AERIA_UPDATE_BASE` et `AERIA_UPDATE_ALLOW_HTTP`, **jamais définies dans les builds livrés**) et un faux `Setup.exe`, puis joue 27 vérifications (tags piégés, fichier altéré, empreinte absente, 404, bonne version). Aperçu de la page : `fetch` remplaçable depuis la console, `window.__preview.updater.onUpdateInit({version:'0.1.0', canUpdate:true})` puis le bouton « Rechercher ».

## Compiler et publier

| Plateforme | Commande |
| --- | --- |
| Windows | `cmake --preset release && cmake --build --preset release` |
| Linux | `cmake --preset linux-release && cmake --build --preset linux-release` (paquets : `libgtk-3-dev libwebkit2gtk-4.1-dev libcurl4-openssl-dev libx11-dev`) |
| Archive portable | `cpack -G ZIP` (Windows) ou `cpack -G TGZ` (Linux), dans le dossier de build ; `-DAERIA_VERSION=v0.1.0` fixe le nom |
| Installateur Windows | `cmake --install <build> --prefix stage`, puis `ISCC /DAppVersion=0.1.0 /DStageDir=stage /DOutDir=dist installer\windows\aeria.iss` (Inno Setup 6) |
| Paquet `.deb` | configurer avec `-DAERIA_DEB_LAYOUT=ON`, puis `cpack -G DEB` |

**Installateur Windows** (`installer/windows/aeria.iss`) : installation par utilisateur (`PrivilegesRequired=lowest`, `{autopf}` = `%LOCALAPPDATA%\Programs`), raccourci du menu Démarrer, cases « Bureau » et « Lancer à l'ouverture de session » (décochées). Cette dernière écrit la même clé `Run\Aeria` que le réglage de l'application (`autostart.cpp`). `AppId` est fixe : c'est lui qui relie une mise à jour à l'installation précédente, ne jamais le changer. Avant d'installer ou de désinstaller, `StopInstalledDock` arrête *seulement* le `Aeria.exe` du dossier d'installation (comparaison du chemin exact), jamais une autre copie. La désinstallation ne retire la clé `Run` que si elle pointe vers le dossier installé, et conserve les données de l'utilisateur.

**Paquet `.deb`** (`cmake/linux.cmake`, option `AERIA_DEB_LAYOUT`) : `/opt/aeria` (exécutable + `ui/`), lien `/usr/bin/aeria`, `aeria.desktop` et l'icône SVG (`installer/linux/`). L'exécutable retrouve `ui/` par `/proc/self/exe`, donc le lien symbolique n'est pas un problème. Dépendances : `dpkg-shlibdeps` quand il existe (Debian, Ubuntu), sinon liste écrite à la main. L'architecture est fixée explicitement (sans `dpkg`, CPack choisirait `i386`). Une version qui ne commence pas par un chiffre devient `0.0.0~<nom>`.

GitHub Actions (`.github/workflows/`) : `ci.yml` compile Windows (MSYS2/MinGW) et Linux (Ubuntu 24.04) à chaque push et pull request. `release.yml` se déclenche sur un tag `v*` : il construit l'installateur et le ZIP Windows, le `.deb` et le `tar.gz` Linux, et les attache à une GitHub Release.

```
git tag v0.1.0 && git push origin v0.1.0
```

## Recettes

- **Ajouter une commande native** : une fonction dans le module concerné (`.h` + `.cpp`), une branche dans `bridge::HandleMessage` (`src/web/bridge.cpp` **et** `src/linux/bridge_linux.cpp`), une ligne dans le tableau ci-dessus ; côté page, `send('ma-commande', …)`.
- **Ajouter un message vers la page** : `bridge::PostToPage("{\"t\":\"…\"}")` (utiliser `util::Json` pour les chaînes) et un `case` dans `onNative` (`ui/js/main.js`).
- **Ajouter un module natif** : `src/<domaine>/nom.h` + `nom.cpp`, un `namespace nom`, et une ligne dans `add_executable` (`CMakeLists.txt`). Pour Linux : `src/linux/nom_linux.cpp` avec la même interface, et une ligne dans `cmake/linux.cmake`.
- **Ajouter une fonctionnalité à l'interface** : `ui/js/features/nom.js` qui exporte `setupNom()` / `renderNom()`, appelés dans `init()` (`ui/js/main.js`) ; son style dans `ui/css/nom.css`, lié dans `index.html`. Pour un nouvel onglet : sa vue dans `index.html` et son entrée dans `NAV_TABS` (`ui/js/dock/dock.js`).
- **Ajouter un réglage** : valeur par défaut dans `DEFAULTS` (`ui/js/core/settings.js`) et un contrôle avec `data-setting="nom"` dans la bonne page (`.set-page`) de `index.html` : `aeria`, `appearance`, `behavior`, `music`, `modes` ou `system` (segmenté, curseur ou interrupteur sont gérés automatiquement). Pour amener l'utilisateur sur une page : `switchTab('settings')` puis `openSettingsPage('…')`. S'il concerne le natif : l'ajouter à `NATIVE_KEYS` et `sendConfig()`, puis le lire dans `dock::ApplyConfig`.
- **Ajouter un outil pour Claude** : définition dans `TOOLS` et exécution dans `execTool` (`ui/js/chat/tools.js`), libellé dans `toolHTML` (`ui/js/chat/chat.js`). Pour le mode Pro, documenter la ligne d'action dans `PRO_ACTIONS` (`prompts.js`) et la lire dans `commitCcAnswer` (`pro.js`).
- **Changer la taille de la barre** : `kBarH`/`kInset` dans `src/app/dock.cpp` **et** `src/linux/dock_linux.cpp`, **et** `--bar-h`/`--inset` dans `ui/css/base.css`.

## Déboguer

- **Interface seule** : servir `ui/` avec n'importe quel serveur local et ouvrir `index.html`. Sans l'hôte natif, la page passe en mode aperçu (panneau ouvert, API désactivée).
- **Dans l'appli** : lancer avec `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=--remote-debugging-port=9222`, puis ouvrir `edge://inspect` ; ou appuyer sur F12 dans le dock.
- **Lecteur dans l'aperçu** : `window.__preview.onNative({t:'media', active:true, title:'…', status:'playing', pos:0, dur:200000, at:Date.now(), …})` simule un morceau, `window.__preview.previewLyrics([{t:0, text:'…'}])` des paroles factices.
- **Linux, interface seule** : comme sous Windows (serveur local, mode aperçu). **Linux, dans l'appli** : lancer `Aeria` depuis le dossier de build ; sous WSL, WSLg fournit l'affichage (X11), mais ignore les positions de fenêtre demandées et donne le focus à l'ouverture. Pour suivre la fenêtre sans outil graphique, un petit programme Xlib qui interroge `XQueryTree` suffit.
- **Plantage en Release seulement** : compiler avec le preset `relwithdebinfo` et lancer sous `gdb` (paquet `mingw-w64-x86_64-gdb` de MSYS2) pour avoir la pile d'appels.
- **Mode Pro sans consommer de quota** : un faux `claude.exe` placé plus tôt dans le `PATH` (ni `%USERPROFILE%\.local\bin` ni winget ne doivent contenir le vrai) reçoit les mêmes arguments.

## Voix
Réglage « Réveiller Aeria à la voix » (`voiceWake`, désactivé par défaut, transmis par `config`). Tout se passe dans le natif, hors ligne :
1. **Micro** (`platform::StartCapture`) : 16 kHz mono, lu en continu par un thread dédié. Rien n'est enregistré sur disque.
2. **Phrases** (`Segmenter`, `voice.cpp`) : détecteur d'énergie à seuil adaptatif (plancher de bruit mesuré). Une phrase commence après 80 ms de voix, finit après 0,7 s de silence (1,1 s pendant une commande), 12 s au plus.
3. **Transcription** : whisper.cpp (`third_party/whisper.cpp`, CPU, AVX2) avec le modèle **choisi par l'utilisateur** (`voiceModel` : `base` ~0,5 s par phrase, `small` ~1,6 s, `medium` ~6 s sur le PC de test). **Aucun modèle n'est livré ni téléchargé d'office** : Réglages → Comportement → Voix propose de choisir, télécharger (`download:<id>`) et supprimer (`delete:<id>`) ; ils vont dans `%APPDATA%\Aeria\models` / `~/.local/share/Aeria/models`, taille et SHA-256 sont épinglés dans `kModels` (`voice.cpp`). Chaque message `voice` porte `models` (ceux déjà présents), `downloading` porte aussi `model`. Changer de modèle recharge l'écoute (`voice::SetModel`). Le CPU ne travaille que pendant les phrases.
4. **Mot d'éveil** (`FindWakeWord`) : « Aeria » à une lettre près (« Aria », « Éria »…) ou coupé en deux petits mots, dans les 3 premiers mots seulement. **Sans attendre la fin de la phrase** : dès 1 s de voix, un brouillon (`Snapshot` de la phrase en cours, passe gloutonne) cherche « Aeria » (`g_earlyWake`) ; la phrase finale le confirme, sinon fausse alerte et rien n'est envoyé. Le reste de la phrase est la commande ; sinon l'écoute reste ouverte 8 s (`awake`). **Pilule** : `dock::SetListeningAsync` (message `WM_APP_LISTEN` sous Windows, `g_idle_add` sous Linux), appelé par `Post` dès que l'état vaut `awake` ou `working`, ouvre au sommet du dock rangé l'état `listen` (190 × 40 DIP au départ), refermé 0,7 s après la fin de l'écoute sauf si la page la garde (`pill`). Pendant l'écoute, un brouillon est refait toutes les ~0,5 s de voix en plus (message `partial`, abandonné si la phrase finale arrive : `g_abortPartial`). Barre ou panneau déjà ouverts : pas de pilule, le champ « Demander » s'éclaire, le dock ne se replie pas et la conversation répond.
5. **Phrase à envoyer** (`Deliver`) : `{"t":"voice","state":"heard"}`. Dock rangé (état `listen`), `pill.js` envoie la phrase (`sendChat`), garde la pilule ouverte (`pill …,1`), affiche la réponse au fur et à mesure (`onChatProgress` de `chat.js`) puis la range après un temps proportionnel à sa longueur (pas tant que la souris est dessus) ; un clic sur la pilule ouvre la conversation. Dock déjà ouvert : `openTab('claude')` + `sendChat`.
**Boutons micro** (champ « Demander » et conversation, `[data-mic]`, `js/features/voice.js`) : envoient `listen` (ou `cancel` s'ils sont allumés). La phrase **s'écrit dans le champ du bouton** (à relire, Entrée pour envoyer) au lieu d'être envoyée à Aeria, et rien ne s'ouvre. Sans `voiceWake`, le natif ouvre le micro en mode « une phrase » (`g_oneShot`) : écoute immédiate, puis `off` après la phrase, l'annulation ou 8 s de silence. Sans modèle, la page renvoie vers Réglages → Comportement. `voice::Shutdown` (fermeture de l'appli) joint le thread d'écoute.

Banc d'essai : `tools/test/voice/run.ps1` (faux micro, phrases dites par la voix de Windows).
