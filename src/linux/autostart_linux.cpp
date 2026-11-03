// Lancement à l'ouverture de session : fichier .desktop dans ~/.config/autostart (XDG).
#include "app/autostart.h"

#include "common/util.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace autostart {
namespace {

std::filesystem::path DesktopFile() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    std::filesystem::path base = xdg && *xdg ? std::filesystem::path(xdg) : std::filesystem::path(util::HomeDir()) / ".config";
    return base / "autostart" / "aeria.desktop";
}

// Valeur de Exec : entre guillemets, avec les caractères spéciaux échappés (spécification Desktop Entry).
std::string QuoteExec(const std::string& path) {
    std::string o = "\"";
    for (char c : path) {
        if (c == '"' || c == '\\' || c == '`' || c == '$') o += "\\\\"; // l'échappement est lui-même échappé
        o += c;
    }
    return o + "\"";
}

} // namespace

bool Enabled() {
    std::error_code ec;
    return std::filesystem::exists(DesktopFile(), ec);
}

void Set(bool enabled) {
    auto file = DesktopFile();
    std::error_code ec;
    if (!enabled) {
        std::filesystem::remove(file, ec);
        return;
    }
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream f(file, std::ios::trunc);
    f << "[Desktop Entry]\nType=Application\nName=Aeria\nComment=Dock en verre\n"
      << "Exec=" << QuoteExec(util::ExeDir() + "/Aeria") << "\nTerminal=false\nX-GNOME-Autostart-enabled=true\n";
}

} // namespace autostart
