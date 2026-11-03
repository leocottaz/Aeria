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
#include "features/voice.h"
#include "web/webview_host.h"

#include <shellapi.h>

#include <cstdlib>
#include <memory>

namespace bridge {
namespace {

HWND g_window = nullptr;

void PostKeyStatus(bool ok) {
    PostToPage("{\"t\":\"key\",\"ok\":" + std::string(ok ? "true" : "false") + "," + api_key::StatusJson() + "}");
}

void PostFreeKeyStatus(const std::string& service, bool ok) {
    PostToPage("{\"t\":\"freekey\",\"service\":" + util::Json(service) + ",\"ok\":" + (ok ? "true" : "false") + "," +
               api_key::FreeStatusJson() + "}");
}

} // namespace

void Init(HWND window) { g_window = window; }

void PostToPage(std::string json) {
    if (g_window) PostMessageW(g_window, WM_APP_WEB, 0, (LPARAM) new std::string(std::move(json)));
}

void Deliver(LPARAM lp) {
    std::unique_ptr<std::string> json((std::string*)lp);
    webview::PostJson(util::Widen(*json));
}

// Une commande par ligne d'en-tête ; le reste du message est l'argument.
void HandleMessage(const std::string& msg) {
    size_t nl = msg.find('\n');
    std::string cmd = msg.substr(0, nl);
    std::string arg = nl == std::string::npos ? "" : msg.substr(nl + 1);

    // --- cycle de vie et fenêtre
    if (cmd == "ready") {
        PostToPage("{\"t\":\"init\"," + api_key::StatusJson() + ",\"autostart\":" +
                   (autostart::Enabled() ? "true" : "false") + ",\"lowMemory\":" +
                   (webview::LowMemory() ? "true" : "false") + "," + api_key::FreeStatusJson() + ",\"version\":" +
                   util::Json(updater::Version()) + ",\"canUpdate\":" + (updater::CanSelfUpdate() ? "true" : "false") + "}");
        dock::OnPageReady();
    } else if (cmd == "config") {
        dock::ApplyConfig(arg);
    } else if (cmd == "lowmem") {
        webview::SetLowMemory(arg == "1");
    } else if (cmd == "open") {
        dock::OpenPanel();
    } else if (cmd == "bar") {
        dock::ShowBar();
    } else if (cmd == "hide") {
        dock::HideByUser();
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

    // --- lecteur de musique
    } else if (cmd == "media") { // toggle | next | prev | seek\n<ms> | sources\n<deezer|all> | refresh
        media::Command(arg);
    } else if (cmd == "quit") {
        DestroyWindow(g_window);

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
    } else if (cmd == "reminders") {
        reminders::Sync(arg);
        reminders::Check();
    } else if (cmd == "timer") { // fin du compte à rebours (ms epoch), 0 = aucun
        reminders::SetTimerEnd(std::atoll(arg.c_str()));
        reminders::Check();
    } else if (cmd == "notify") {
        auto f = util::Split(arg, '\n', 2);
        if (f.size() == 2) tray::Notify(f[0], f[1], dock::Cfg().sound);

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
        pc::PickFile(arg, g_window);
    } else if (cmd == "voice") { // status | download | listen
        voice::Command(arg);
    } else if (cmd == "update") { // tag « v1.2.3 » : télécharge, vérifie et lance l'installateur
        updater::Install(arg);
    } else if (cmd == "openurl") {
        if (arg.rfind("https://", 0) == 0 || arg.rfind("http://", 0) == 0)
            ShellExecuteW(nullptr, L"open", util::Widen(arg).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

} // namespace bridge
