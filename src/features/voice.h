// Commande vocale : dire « Aeria » ouvre le dock, puis la phrase suivante est envoyée à l'assistante.
//  - Hors ligne : le micro est lu en continu par le NATIF, découpé en phrases par un détecteur d'énergie,
//    et seules les phrases sont transcrites par whisper.cpp (modèle ggml-base-q5_1, ~60 Mo). Aucun
//    son n'est enregistré ni envoyé : tout reste en mémoire le temps de la transcription.
//  - Désactivé par défaut (réglage « voiceWake »). Le modèle se télécharge à la demande de l'utilisateur.
// Avancement vers la page : {"t":"voice","state":"off|nomodel|downloading|loading|idle|awake|working|error",
// "pct":0-100,"msg":"…"} et {"t":"voice","state":"heard","text":"…"} quand une phrase est à envoyer.
// Windows : features/voice_win.cpp (micro, téléchargement). Linux : linux/voice_linux.cpp.
#pragma once

#include <string>

namespace voice {

// Réglage « voiceWake » : démarre ou arrête l'écoute. Sans modèle, annonce « nomodel ».
void Apply(bool enabled);

// Commandes de la page : « status » (renvoie l'état), « download » (télécharge le modèle),
// « listen » (écoute la prochaine phrase sans attendre « Aeria »).
void Command(const std::string& arg);

// Cherche « Aeria » (ou une variante de transcription : « Éria », « Aria »…) dans un texte. Vrai s'il est
// trouvé ; rest reçoit alors ce qui suit, sans ponctuation de tête. Exposé pour les tests.
bool FindWakeWord(const std::string& text, std::string& rest);

} // namespace voice
