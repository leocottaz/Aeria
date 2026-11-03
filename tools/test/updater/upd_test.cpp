// Banc d'essai de l'updater Windows : vrai code (updater.cpp), faux pont, serveur local.
#include "features/updater.h"
#include "web/bridge.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static std::mutex g_m;
static std::condition_variable g_cv;
static std::vector<std::string> g_msgs;

namespace bridge {
void PostToPage(std::string json) {
    std::lock_guard lock(g_m);
    std::printf("   PAGE  %s\n", json.c_str());
    g_msgs.push_back(json);
    g_cv.notify_all();
}
void HandleMessage(const std::string&) {}
}

static int g_fail = 0;
static void Check(const char* name, bool ok) {
    std::printf("%s  %s\n", ok ? "OK   " : "ECHEC", name);
    if (!ok) ++g_fail;
}

// Lance Install et attend l'état final (error ou restarting) ; retourne le dernier message.
static std::string Run(const std::string& tag) {
    {
        std::lock_guard lock(g_m);
        g_msgs.clear();
    }
    std::printf(">> Install(\"%s\")\n", tag.c_str());
    updater::Install(tag);
    std::unique_lock lock(g_m);
    g_cv.wait_for(lock, std::chrono::seconds(30), [] {
        return !g_msgs.empty() && (g_msgs.back().find("\"error\"") != std::string::npos ||
                                   g_msgs.back().find("\"restarting\"") != std::string::npos);
    });
    std::string last = g_msgs.empty() ? "" : g_msgs.back();
    lock.unlock();
    std::this_thread::sleep_for(std::chrono::milliseconds(400)); // laisse le thread libérer « occupé »
    return last;
}

static bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

int main() {
    // --- logique pure
    Check("tag v1.2.3 valide", updater::IsValidTag("v1.2.3"));
    Check("tag v10.20.30 valide", updater::IsValidTag("v10.20.30"));
    Check("tag v1.2 refusé", !updater::IsValidTag("v1.2"));
    Check("tag 1.2.3 (sans v) refusé", !updater::IsValidTag("1.2.3"));
    Check("tag v1.2.3-rc1 refusé", !updater::IsValidTag("v1.2.3-rc1"));
    Check("tag ../x refusé", !updater::IsValidTag("../x"));
    Check("tag v1.2.3/../../x refusé", !updater::IsValidTag("v1.2.3/../../x"));
    Check("tag vide refusé", !updater::IsValidTag(""));
    Check("tag trop long refusé", !updater::IsValidTag("v123456.1.1"));
    Check("nom de l'installateur", updater::AssetName("v1.2.3") == "Aeria-v1.2.3-windows-x64-setup.exe");
    Check("nom pour un tag invalide = vide", updater::AssetName("v1.2").empty());
    const std::string sums =
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA  fichier-a.zip\r\n"
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb *fichier-b.exe\n"
        "cccc  trop-court.exe\n"
        "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz  pas-hex.exe\n";
    Check("empreinte lue (majuscules → minuscules, CRLF)",
          updater::ExpectedHash(sums, "fichier-a.zip") == std::string(64, 'a'));
    Check("empreinte lue (mode binaire *)", updater::ExpectedHash(sums, "fichier-b.exe") == std::string(64, 'b'));
    Check("ligne trop courte ignorée", updater::ExpectedHash(sums, "trop-court.exe").empty());
    Check("non hexadécimal ignoré", updater::ExpectedHash(sums, "pas-hex.exe").empty());
    Check("fichier absent", updater::ExpectedHash(sums, "inconnu.exe").empty());
    Check("pas de correspondance partielle", updater::ExpectedHash(sums, "fichier-a").empty());
    Check("copie installée (unins000.exe présent)", updater::CanSelfUpdate());
    Check("version compilée", std::string(updater::Version()) == "0.1.0");

    // --- scénarios réseau (serveur local)
    std::string r;
    r = Run("v1.2");
    Check("tag invalide : erreur, rien de téléchargé", Has(r, "\"error\"") && Has(r, "invalide"));
    r = Run("../../etc");
    Check("tag piégé : erreur", Has(r, "\"error\""));

    r = Run("v9.9.8");
    Check("fichier altéré : refusé (SHA-256 différent)", Has(r, "\"error\"") && Has(r, "SHA-256"));
    r = Run("v9.9.7");
    Check("empreinte absente de SHA256SUMS : refusé", Has(r, "\"error\"") && Has(r, "absente"));
    r = Run("v9.9.6");
    Check("installateur introuvable (404) : erreur", Has(r, "\"error\"") && Has(r, "404"));
    r = Run("v9.9.5");
    Check("SHA256SUMS introuvable : erreur", Has(r, "\"error\"") && Has(r, "SHA256SUMS"));

    r = Run("v9.9.9");
    Check("bonne version : installateur lancé (restarting)", Has(r, "\"restarting\""));
    {
        std::lock_guard lock(g_m);
        bool sawDownload = false, sawVerify = false, sawInstall = false;
        for (auto& m : g_msgs) {
            if (Has(m, "\"downloading\"")) sawDownload = true;
            if (Has(m, "\"verifying\"")) sawVerify = true;
            if (Has(m, "\"installing\"")) sawInstall = true;
        }
        Check("étapes annoncées dans l'ordre : download, verify, install", sawDownload && sawVerify && sawInstall);
    }

    std::printf("Échecs : %d\n", g_fail);
    return g_fail ? 1 : 0;
}
