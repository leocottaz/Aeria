// Client MCP (Model Context Protocol), côté natif : lance les serveurs « stdio » de l'utilisateur et relaie leurs lignes.
// Le natif ne comprend pas le protocole : il fait seulement le transport. Chaque message JSON-RPC tient sur une ligne ;
// la page (ui/js/features/mcp.js) écrit les requêtes, lit les réponses et décide quels outils sont proposés à l'assistante.
//
// Messages vers la page : {"t":"mcp","id":…,"gen":N,"ev":"started"}, {"ev":"line","data":"<une ligne JSON>"},
// {"ev":"fail","msg":…} (impossible à lancer) et {"ev":"exit","code":…,"stderr":…}. `gen` change à chaque lancement d'un même
// id : la page ignore les évènements d'une génération périmée.
#pragma once

#include <string>

namespace mcp {

// payload : id \x1f commande \x1f arguments (séparés par \x1e) \x1f variables d'environnement « NOM=valeur » (séparées par \x1e).
// Relancer un id déjà lancé arrête l'ancien processus. Aucun shell : la commande est cherchée dans le PATH.
void Start(const std::string& payload);

// payload : id \n ligne JSON (sans retour à la ligne).
void Send(const std::string& payload);

// id d'un serveur, ou vide pour tous.
void Stop(const std::string& id);

} // namespace mcp
