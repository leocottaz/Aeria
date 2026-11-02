// Pont page <-> natif sous Linux. Même protocole que web/bridge.cpp (Windows) ; les commandes
// dont le module n'est pas encore porté (media, search, routine, volume…) sont ignorées.
#include "web/bridge.h"

#include "ai/anthropic_http.h"
#include "ai/api_key.h"
#include "ai/claude_code.h"
#include "app/autostart.h"
#include "app/dock.h"
#include "app/tray.h"
#include "common/util.h"
#include "features/media.h"
#include "features/pc.h"
#include "features/reminders.h"
#include "features/updater.h"
#include "linux/host.h"

#include <glib.h>

#include <memory>

namespace bridge {
namespace {

void PostKeyStatus(bool ok) {
    PostToPage("{\"t\":\"key\",\"ok\":" + std::string(ok ? "true" : "false") + "," + api_key::StatusJson() + "}");
}

void PostFreeKeyStatus(const std::string& service, bool ok) {
    PostToPage("{\"t\":\"freekey\",\"service\":" + util::Json(service) + ",\"ok\":" + (ok ? "true" : "false") + "," +
               api_key::FreeStatusJson() + "}");
}

} // namespace

// Utilisable depuis n'importe quel thread : la remise à la page se fait sur la boucle principale GTK.
void PostToPage(std::string json) {
    g_idle_add(
        [](gpointer data) -> gboolean {
            std::unique_ptr<std::string> j(static_cast<std::string*>(data));
            host::EvalJson(*j);
            return G_SOURCE_REMOVE;
        },
        new std::string(std::move(json)));
}

void HandleMessage(const std::string& msg) {
    size_t nl = msg.find('\n');
    std::string cmd = msg.substr(0, nl);
    std::string arg = nl == std::string::npos ? "" : msg.substr(nl + 1);

    if (cmd == "ready") {
        PostToPage("{\"t\":\"init\"," + api_key::StatusJson() + ",\"autostart\":" + (autostart::Enabled() ? "true" : "false") +
                   "," + api_key::FreeStatusJson() + ",\"version\":" + util::Json(updater::Version()) +
                   ",\"canUpdate\":" + (updater::CanSelfUpdate() ? "true" : "false") + "}");
        dock::OnPageReady();
    } else if (cmd == "config") {
        dock::ApplyConfig(arg);
    } else if (cmd == "lowmem") {
        // Mode économie de mémoire : propre à WebView2, sans équivalent ici. Le réglage reste dans la page.
    } else if (cmd == "bar") {
        dock::ShowBar();
    } else if (cmd == "pin") {
        dock::SetPinned(arg == "1");
    } else if (cmd == "hold") {
        dock::HoldFor(std::atoi(arg.c_str()));
    } else if (cmd == "reveal") {
        dock::RevealBriefly(std::atoi(arg.c_str()));
    } else if (cmd == "revealmusic") { // ms \n largeur (DIP)
        auto f = util::Split(arg, '\n', 2);
        dock::RevealMusic(f.empty() ? 0 : std::atoi(f[0].c_str()), f.size() > 1 ? std::atoi(f[1].c_str()) : 0);
    } else if (cmd == "mini") {
        dock::SetMini(std::atoi(arg.c_str()));
    } else if (cmd == "media") { // toggle | next | prev | seek\n<ms> | sources\n<deezer|all> | refresh
        media::Command(arg);
    } else if (cmd == "reminders") {
        reminders::Sync(arg);
        reminders::Check();
    } else if (cmd == "timer") { // fin du compte à rebours (ms epoch), 0 = aucun
        reminders::SetTimerEnd(std::atoll(arg.c_str()));
        reminders::Check();
    } else if (cmd == "notify") {
        auto f = util::Split(arg, '\n', 2);
        if (f.size() == 2) tray::Notify(f[0], f[1], dock::Cfg().sound);
    } else if (cmd == "open") {
        dock::OpenPanel();
    } else if (cmd == "hide") {
        dock::HideByUser();
    } else if (cmd == "quit") {
        host::Quit();
    } else if (cmd == "update") {
        updater::Install(arg);
    } else if (cmd == "openurl") {
        if (arg.rfind("https://", 0) == 0 || arg.rfind("http://", 0) == 0) host::OpenUrl(arg);

    // --- mode Clé API
    } else if (cmd == "key") {
        PostKeyStatus(!arg.empty() && api_key::Save(arg));
    } else if (cmd == "delkey") {
        api_key::Remove();
        PostKeyStatus(true);
    } else if (cmd == "http") { // id \n méthode \n chemin \n betas \n corps
        auto f = util::Split(arg, '\n', 5);
        if (f.size() == 5) anthropic_http::Start(f[0], f[1], f[2], f[3], f[4]);

    // --- mode Gratuit (Groq, Gemini)
    } else if (cmd == "freekey") { // service \n clé
        auto f = util::Split(arg, '\n', 2);
        bool ok = f.size() == 2 && !f[1].empty() && api_key::Save(f[1], f[0]);
        PostFreeKeyStatus(f.empty() ? "" : f[0], ok);
    } else if (cmd == "delfreekey") { // service
        api_key::Remove(arg);
        PostFreeKeyStatus(arg, true);
    } else if (cmd == "fhttp") { // id \n service \n méthode \n chemin \n corps
        auto f = util::Split(arg, '\n', 5);
        if (f.size() == 5) anthropic_http::StartFree(f[0], f[1], f[2], f[3], f[4]);

    // --- mode Abonnement Pro
    } else if (cmd == "cc") {
        claude_code::StartChat(arg);
    } else if (cmd == "ccstatus") {
        claude_code::CheckInstalled();
    } else if (cmd == "cclogin") {
        claude_code::OpenLogin();

    // --- commun
    } else if (cmd == "cancel") {
        if (!claude_code::Cancel(arg)) anthropic_http::Cancel(arg);

    // --- actions sur le PC (Claude, routines)
    } else if (cmd == "search") { // id \n mots \n extension \n du \n au
        pc::Search(arg);
    } else if (cmd == "openfound") {
        pc::OpenFound(arg);
    } else if (cmd == "routine") { // une étape par ligne : type \t valeur
        pc::RunRoutine(arg);
    } else if (cmd == "volume") {
        pc::SetVolume(std::atoi(arg.c_str()));
    } else if (cmd == "pickfile") {
        pc::PickFile(arg);
    }
}

} // namespace bridge
