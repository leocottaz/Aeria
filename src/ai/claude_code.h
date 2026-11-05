// Mode « Abonnement Pro » : le dock lance le CLI officiel Claude Code (`claude -p`),
// connecté au compte de l'utilisateur, sans aucun outil (sauf la recherche web si la page le demande). La sortie stream-json est
// relayée à la page ligne par ligne ("chunk"), puis "done" avec le code de sortie.
#pragma once

#include <string>

namespace claude_code {

// payload : id \x1f modèle \x1f effort \x1f session à reprendre \x1f outils ("web", "screen", les deux séparés par une virgule, ou vide) \x1f prompt système \x1f message
void StartChat(const std::string& payload);
bool Cancel(const std::string& id); // false si l'id est inconnu

// Cherche claude.exe et poste {"t":"ccstatus","found":…,"version":…}.
void CheckInstalled();

// Ouvre une console avec `claude` pour se connecter (/login).
void OpenLogin();

} // namespace claude_code
