// Parties de util propres à Linux (le reste est dans common/util_common.cpp).
#include "common/util.h"

#include <unistd.h>

#include <cstdlib>
#include <filesystem>

namespace util {
namespace {

std::string AppDir(const char* envVar, const char* fallback) {
    std::filesystem::path base = std::getenv(envVar) && *std::getenv(envVar) ? std::getenv(envVar) : HomeDir() + fallback;
    auto dir = base / "Aeria";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir.string();
}

} // namespace

std::string HomeDir() {
    const char* h = std::getenv("HOME");
    return h && *h ? h : "/tmp";
}

std::string ConfigDir() { return AppDir("XDG_CONFIG_HOME", "/.config"); }
std::string DataDir() { return AppDir("XDG_DATA_HOME", "/.local/share"); }

std::string ExeDir() {
    std::error_code ec;
    auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::current_path().string() : p.parent_path().string();
}

} // namespace util
