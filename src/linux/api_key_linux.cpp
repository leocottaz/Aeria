// Clés API sous Linux : fichiers en lecture seule pour l'utilisateur (0600) dans
// ~/.config/Aeria — même principe que les identifiants de Claude Code. Pas encore
// de trousseau (libsecret) : à faire. Elles ne sont jamais envoyées à la page.
#include "ai/api_key.h"

#include "common/util.h"

#include <sys/stat.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>

namespace api_key {
namespace {

const char* kSlots[] = {"anthropic", "groq", "gemini", "openai"};

std::map<std::string, std::string> g_keys;
std::mutex g_mutex;

std::filesystem::path KeyPath(const std::string& slot) {
    return std::filesystem::path(util::ConfigDir()) / (slot == "anthropic" ? "apikey" : slot + "key");
}

} // namespace

bool IsKnown(const std::string& slot) {
    for (auto s : kSlots)
        if (slot == s) return true;
    return false;
}

void Load() {
    for (auto slot : kSlots) {
        std::ifstream f(KeyPath(slot), std::ios::binary);
        if (!f) continue;
        std::string key((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::lock_guard lock(g_mutex);
        g_keys[slot] = key;
    }
}

bool Save(const std::string& key, const std::string& slot) {
    if (!IsKnown(slot)) return false;
    auto path = KeyPath(slot);
    {
        std::ofstream create(path, std::ios::binary | std::ios::trunc); // créé avant de chmod, puis rempli
    }
    chmod(path.c_str(), S_IRUSR | S_IWUSR);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(key.data(), (std::streamsize)key.size());
    if (!f) return false;
    std::lock_guard lock(g_mutex);
    g_keys[slot] = key;
    return true;
}

void Remove(const std::string& slot) {
    if (!IsKnown(slot)) return;
    std::error_code ec;
    std::filesystem::remove(KeyPath(slot), ec);
    std::lock_guard lock(g_mutex);
    g_keys.erase(slot);
}

std::string Get(const std::string& slot) {
    std::lock_guard lock(g_mutex);
    auto it = g_keys.find(slot);
    return it == g_keys.end() ? std::string() : it->second;
}

std::string StatusJson(const std::string& slot) {
    std::string key = Get(slot);
    std::string hint = key.size() > 4 ? key.substr(key.size() - 4) : "";
    return std::string("\"hasKey\":") + (key.empty() ? "false" : "true") + ",\"keyHint\":" + util::Json(hint);
}

std::string FreeStatusJson() {
    return "\"freeKeys\":{\"groq\":{" + StatusJson("groq") + "},\"gemini\":{" + StatusJson("gemini") + "},\"openai\":{" + StatusJson("openai") + "}}";
}

} // namespace api_key
