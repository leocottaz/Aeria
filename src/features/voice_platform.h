// Ce que features/voice.cpp demande à chaque plateforme (micro, téléchargement, ouverture du dock).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace voice::platform {

using Sink = std::function<void(const int16_t* samples, size_t count)>;

// Lit le micro par défaut en 16 kHz, mono, entiers 16 bits ; sink est appelé depuis un thread dédié.
bool StartCapture(const Sink& sink, std::string& error);
void StopCapture(); // bloque jusqu'à l'arrêt du thread

// Dossier des données + models/ggml-small-q5_1.bin (le dossier est créé au besoin).
std::string ModelPath();

// Télécharge url (HTTPS) dans dest, vérifie le SHA-256 et abandonne au-delà de maxBytes. progress reçoit
// 0-100. Le fichier n'existe à la fin que si tout est bon.
bool Download(const std::string& url, const std::string& dest, uint64_t maxBytes, const std::string& sha256,
              const std::function<void(int)>& progress, std::string& error);

// Ouvre le panneau du dock. Utilisable depuis n'importe quel thread.
void OpenDock();

// Fait apparaître (true) ou rentrer (false) l'encoche « j'écoute » du dock. Utilisable depuis n'importe quel thread.
void ShowListening(bool on);

} // namespace voice::platform
