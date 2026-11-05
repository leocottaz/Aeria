// Capture d'écran pour l'assistante (sur demande de la page seulement, jamais en continu).
// Windows : features/screen.cpp. Linux : linux/screen_linux.cpp.
// Réponse asynchrone : {"t":"reply","id":…,"ok":true,"data":<JPEG en base64>,"media":"image/jpeg","w":…,"h":…}
// (+ "file" : chemin relatif au dossier de travail de Claude Code quand toFile est vrai), ou ok:false et error.
#pragma once

#ifdef _WIN32
#include <windows.h>
#endif

#include <string>

namespace screen {

// Écran où se trouve le dock, réduit à 1568 px au plus, en JPEG. Le dock lui-même n'apparaît pas dans l'image.
// toFile : écrit aussi le JPEG dans <dossier de travail de Claude Code>/screens/ (le mode Pro le lit avec l'outil Read).
#ifdef _WIN32
void Capture(const std::string& id, bool toFile, HWND dock);
#else
void Capture(const std::string& id, bool toFile);
#endif

} // namespace screen
