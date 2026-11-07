// Notifications des autres applications (Discord, mails…) : affichées dans la pastille du mini-dock.
// Windows : UserNotificationListener, relevé toutes les 1,5 s dans un thread (voir appnotifs.cpp).
// Linux : pas encore pris en charge (le bouchon répond « unsupported »).
#pragma once

#include <string>

namespace appnotifs {

// Démarre ou arrête l'écoute (réglage « Notifications des autres applis », désactivé par défaut).
// Messages vers la page :
//   {"t":"appnotifs","status":"allowed"|"denied"|"unsupported"|"error"}   à chaque démarrage
//   {"t":"appnotif","app":…,"title":…,"text":…}                            une nouvelle notification
// Les notifications déjà présentes au démarrage de l'écoute ne sont pas rejouées.
void SetEnabled(bool on);

// Clic sur la carte : ouvre ou ramène au premier plan l'appli qui a posté la notification (son identifiant
// `aumid` vient du message `appnotif`). Un identifiant jamais vu dans une notification est refusé.
void OpenApp(const std::string& aumid);

} // namespace appnotifs
