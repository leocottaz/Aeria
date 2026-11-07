// Données de l'utilisateur (réglages, rappels, notes, tâches, conversation, modes…) dans des fichiers JSON
// simples, hors du stockage du navigateur : `<dossier de données>/Aeria/data/<clé>.json`
// (Windows : %APPDATA%\Aeria\data, Linux : $XDG_DATA_HOME/Aeria/data). Ils survivent donc aux mises à jour
// et à une réinitialisation du profil WebView2 / WebKit. Le contenu est un JSON opaque pour le natif :
// la page l'écrit (commande `savestore`) et le reçoit en bloc avant de démarrer (`loadstore` → message `store`).
//
// Protection contre la perte :
//  - écriture atomique (fichier temporaire puis renommage) : une coupure ne laisse jamais un fichier tronqué ;
//  - un texte qui n'est pas du JSON valide est refusé (jamais écrit par-dessus de bonnes données) ;
//  - une sauvegarde par jour (7 jours gardés) dans data/backups/ pour les données importantes ;
//  - un fichier abîmé est mis de côté (`<clé>.corrupt.json`, jamais supprimé) et la dernière sauvegarde valide est reprise.
// Pas de chiffrement : les fichiers restent lisibles et modifiables à la main (comme demandé), protégés par
// les droits du compte (Linux : 0700 / 0600).
#pragma once

#include <string>

namespace data_store {

// Seules ces clés sont acceptées (la page ne choisit pas des chemins) : voir `kKeys` dans data_store.cpp
// et `FILE_KEYS` dans ui/js/core/util.js (à garder identiques).
bool IsKnown(const std::string& key);

// Texte JSON de la clé, ou "" si elle n'existe pas encore (ou si aucune version valide n'a pu être lue).
std::string Load(const std::string& key);

// Objet JSON `{"clé":"<texte>", …}` des clés présentes (chaque texte est une chaîne JSON échappée), pour le message `store`.
std::string LoadAllJson();

// Écrit la clé. Faux si la clé est inconnue, si le texte n'est pas du JSON valide ou si le disque refuse.
bool Save(const std::string& key, const std::string& json);

// Vrai si `text` est un document JSON complet et valide (exposé pour les tests).
bool IsValidJson(const std::string& text);

} // namespace data_store
