#include "ai/api_key.h"

#include "common/util.h"

#include <wincrypt.h>

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
    std::wstring file = slot == "anthropic" ? L"apikey.bin" : util::Widen(slot) + L"key.bin";
    return util::RoamingDataDir() + L"\\" + file;
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
        std::string blob((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        DATA_BLOB in{(DWORD)blob.size(), (BYTE*)blob.data()}, out{};
        if (CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
            std::lock_guard lock(g_mutex);
            g_keys[slot].assign((char*)out.pbData, out.cbData);
            SecureZeroMemory(out.pbData, out.cbData);
            LocalFree(out.pbData);
        }
    }
}

bool Save(const std::string& key, const std::string& slot) {
    if (!IsKnown(slot)) return false;
    DATA_BLOB in{(DWORD)key.size(), (BYTE*)key.data()}, out{};
    if (!CryptProtectData(&in, L"Aeria API key", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return false;
    std::ofstream f(KeyPath(slot), std::ios::binary | std::ios::trunc);
    f.write((char*)out.pbData, out.cbData);
    LocalFree(out.pbData);
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
