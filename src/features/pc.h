// Actions sur le PC demandées par la page (pour Claude ou les routines) : recherche de
// fichiers (index Windows Search ; parcours du dossier personnel sous Linux), ouverture d'un
// résultat, étape de routine, volume système, fenêtre « Ouvrir ».
// Les réponses asynchrones partent en {"t":"reply","id":…}.
#pragma once

#ifdef _WIN32
#include <windows.h>
#endif

#include <string>

namespace pc {

// "id\nmots\nextension\ndu\nau" (dates AAAA-MM-JJ, incluses ; champs vides = pas de filtre).
// Cherche dans le dossier de l'utilisateur, hors AppData, les 15 fichiers les plus récents.
void Search(const std::string& payload);

// N'ouvre qu'un fichier renvoyé par Search. Un exécutable ou un script n'est jamais lancé :
// on ouvre son dossier en le sélectionnant.
void OpenFound(const std::string& path);

// Routine : une étape par ligne, "type\tvaleur" — open (appli, fichier, dossier ou adresse),
// volume (0 à 100), music (play, pause, next, previous), mute (on, off, toggle), lock (verrouille
// la session), wait (secondes). Exécutée dans un thread : elle continue même quand le dock se
// range et que la page est en veille.
void RunRoutine(const std::string& steps);

// Applis qui déclenchent un mode : noms d'exécutable, un par ligne (« teams.exe » ; jamais un chemin), liste vide = plus
// de surveillance. Quand l'une d'elles apparaît alors qu'elle ne tournait pas au relevé précédent, la page reçoit
// {"t":"appstart","app":"teams.exe"} (le nom en minuscules) et, sous Windows, sort de veille. Ce qui tourne déjà au moment
// de l'appel ne déclenche rien. CheckApps est appelé chaque seconde, sur le thread de l'interface.
void WatchApps(const std::string& names);
void CheckApps();

void SetVolume(int percent); // volume de la sortie audio par défaut (0 à 100)
void SetMute(const std::string& mode); // "on", "off" ou "toggle" sur la sortie audio par défaut
// Fenêtre « Ouvrir » possédée par le dock : elle passe devant lui même s'il est
// « toujours au premier plan ». Répond avec le chemin choisi ("" si annulé).
#ifdef _WIN32
void PickFile(const std::string& id, HWND owner);
#else
void PickFile(const std::string& id); // thread de l'interface uniquement
#endif

} // namespace pc
