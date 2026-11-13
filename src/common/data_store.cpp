#include "common/data_store.h"

#include "common/util.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace data_store {
namespace {

namespace fs = std::filesystem;

struct Key {
    const char* name;
    bool backup; // sauvegarde quotidienne (données précieuses ; les autres se reconstruisent)
};

// À garder identique à FILE_KEYS (ui/js/core/util.js).
constexpr Key kKeys[] = {
    {"settings", true}, {"reminders", true}, {"notes", true}, {"todos", true}, {"convo", true}, {"modes", true},
    {"activeMode", false}, {"timer", false}, {"welcomed", false}, {"lastSeenVersion", false},
    {"mcp", true}, // serveurs MCP de l'utilisateur (commande, arguments, variables d'environnement) et outils toujours autorisés
};

constexpr size_t kMaxSize = 64u << 20; // une conversation avec images réduites reste très en dessous
constexpr int kKeepDays = 7;

std::mutex g_mutex;

const Key* Find(const std::string& key) {
    for (const auto& k : kKeys)
        if (key == k.name) return &k;
    return nullptr;
}

fs::path Dir() {
#ifdef _WIN32
    fs::path d = fs::path(util::RoamingDataDir()) / L"data";
#else
    fs::path d = fs::path(util::DataDir()) / "data";
#endif
    std::error_code ec;
    fs::create_directories(d, ec);
#ifndef _WIN32
    chmod(d.c_str(), 0700);
#endif
    return d;
}

fs::path FilePath(const std::string& key) { return Dir() / (key + ".json"); }
fs::path BackupDir() {
    fs::path d = Dir() / "backups";
    std::error_code ec;
    fs::create_directories(d, ec);
    return d;
}

void Private([[maybe_unused]] const fs::path& p) {
#ifndef _WIN32
    chmod(p.c_str(), 0600);
#endif
}

// --- validateur JSON (descente récursive) : ce qu'on écrit ou relit doit toujours être un document complet.
class Checker {
public:
    explicit Checker(const std::string& s) : s_(s) {}
    bool Run() {
        Ws();
        if (!Value(0)) return false;
        Ws();
        return i_ == s_.size();
    }

private:
    const std::string& s_;
    size_t i_ = 0;

    bool End() const { return i_ >= s_.size(); }
    void Ws() {
        while (!End() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) ++i_;
    }
    bool Lit(const char* w) {
        size_t n = std::strlen(w);
        if (s_.compare(i_, n, w) != 0) return false;
        i_ += n;
        return true;
    }
    static bool Digit(char c) { return c >= '0' && c <= '9'; }
    bool Digits() {
        size_t st = i_;
        while (!End() && Digit(s_[i_])) ++i_;
        return i_ > st;
    }
    bool Str() {
        if (End() || s_[i_] != '"') return false;
        ++i_;
        while (!End()) {
            unsigned char c = (unsigned char)s_[i_];
            if (c == '"') { ++i_; return true; }
            if (c < 0x20) return false;
            if (c == '\\') {
                if (++i_ >= s_.size()) return false;
                char e = s_[i_];
                if (e == 'u') {
                    for (int k = 1; k <= 4; ++k)
                        if (i_ + k >= s_.size() || !std::isxdigit((unsigned char)s_[i_ + k])) return false;
                    i_ += 4;
                } else if (e == 0 || !std::strchr("\"\\/bfnrt", e)) {
                    return false;
                }
            }
            ++i_;
        }
        return false;
    }
    bool Num() {
        if (!End() && s_[i_] == '-') ++i_;
        if (End()) return false;
        if (s_[i_] == '0') ++i_;
        else if (!Digits()) return false;
        if (!End() && s_[i_] == '.') {
            ++i_;
            if (!Digits()) return false;
        }
        if (!End() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            ++i_;
            if (!End() && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
            if (!Digits()) return false;
        }
        return true;
    }
    bool Value(int depth) {
        if (depth > 200 || End()) return false;
        char c = s_[i_];
        if (c == '{') {
            ++i_;
            Ws();
            if (!End() && s_[i_] == '}') { ++i_; return true; }
            for (;;) {
                Ws();
                if (!Str()) return false;
                Ws();
                if (End() || s_[i_] != ':') return false;
                ++i_;
                Ws();
                if (!Value(depth + 1)) return false;
                Ws();
                if (End()) return false;
                if (s_[i_] == ',') { ++i_; continue; }
                if (s_[i_] == '}') { ++i_; return true; }
                return false;
            }
        }
        if (c == '[') {
            ++i_;
            Ws();
            if (!End() && s_[i_] == ']') { ++i_; return true; }
            for (;;) {
                Ws();
                if (!Value(depth + 1)) return false;
                Ws();
                if (End()) return false;
                if (s_[i_] == ',') { ++i_; continue; }
                if (s_[i_] == ']') { ++i_; return true; }
                return false;
            }
        }
        if (c == '"') return Str();
        if (c == 't') return Lit("true");
        if (c == 'f') return Lit("false");
        if (c == 'n') return Lit("null");
        return Num();
    }
};

bool ReadFile(const fs::path& p, std::string& out) {
    std::error_code ec;
    auto size = fs::file_size(p, ec);
    if (ec || size > kMaxSize) return false;
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (out.size() >= 3 && out.compare(0, 3, "\xEF\xBB\xBF") == 0) out.erase(0, 3); // BOM d'un éditeur
    return true;
}

std::string Today() {
    std::time_t t = std::time(nullptr);
    std::tm lt{};
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    char b[16];
    std::strftime(b, sizeof b, "%Y%m%d", &lt);
    return b;
}

// Sauvegardes d'une clé, de la plus récente à la plus ancienne (le nom porte la date : tri alphabétique).
std::vector<fs::path> Backups(const std::string& key) {
    std::vector<fs::path> list;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(BackupDir(), ec)) {
        std::string name = e.path().filename().string();
        if (name.size() == key.size() + 14 && name.compare(0, key.size() + 1, key + "-") == 0 &&
            name.compare(name.size() - 5, 5, ".json") == 0)
            list.push_back(e.path());
    }
    std::sort(list.begin(), list.end(), std::greater<>());
    return list;
}

// Avant d'écraser un fichier valide, en garde une copie du jour (la première écriture de la journée fige
// l'état de la veille) et ne conserve que kKeepDays sauvegardes.
void SnapshotBeforeWrite(const std::string& key, const fs::path& current) {
    std::error_code ec;
    if (!fs::exists(current, ec)) return;
    fs::path snap = BackupDir() / (key + "-" + Today() + ".json");
    if (!fs::exists(snap, ec)) {
        std::string text;
        if (!ReadFile(current, text) || !IsValidJson(text)) return; // ne jamais sauvegarder un fichier abîmé
        fs::copy_file(current, snap, fs::copy_options::overwrite_existing, ec);
        Private(snap);
    }
    auto all = Backups(key);
    for (size_t i = kKeepDays; i < all.size(); ++i) fs::remove(all[i], ec);
}

// Lecture d'une clé, avec repli sur la dernière sauvegarde valide si le fichier est abîmé.
std::string LoadLocked(const std::string& key) {
    fs::path p = FilePath(key);
    std::error_code ec;
    if (!fs::exists(p, ec)) return "";
    std::string text;
    if (ReadFile(p, text) && IsValidJson(text)) return text;

    // Fichier illisible ou abîmé : on le met de côté (jamais supprimé) puis on reprend la sauvegarde la plus récente.
    fs::path bad = p;
    bad.replace_extension(".corrupt.json");
    fs::rename(p, bad, ec);
    for (const auto& b : Backups(key)) {
        std::string t;
        if (ReadFile(b, t) && IsValidJson(t)) {
            fs::copy_file(b, p, fs::copy_options::overwrite_existing, ec);
            Private(p);
            return t;
        }
    }
    return "";
}

} // namespace

bool IsKnown(const std::string& key) { return Find(key) != nullptr; }

bool IsValidJson(const std::string& text) { return Checker(text).Run(); }

std::string Load(const std::string& key) {
    if (!IsKnown(key)) return "";
    std::lock_guard lock(g_mutex);
    return LoadLocked(key);
}

std::string LoadAllJson() {
    std::lock_guard lock(g_mutex);
    std::string out = "{";
    bool first = true;
    for (const auto& k : kKeys) {
        std::string text = LoadLocked(k.name);
        if (text.empty()) continue;
        if (!first) out += ',';
        first = false;
        out += util::Json(k.name) + ":" + util::Json(text);
    }
    return out + "}";
}

bool Save(const std::string& key, const std::string& json) {
    const Key* k = Find(key);
    if (!k || json.empty() || json.size() > kMaxSize || !IsValidJson(json)) return false;
    std::lock_guard lock(g_mutex);
    fs::path p = FilePath(key);
    if (k->backup) SnapshotBeforeWrite(key, p);
    fs::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(json.data(), (std::streamsize)json.size());
        f.flush();
        if (!f) {
            std::error_code ec;
            fs::remove(tmp, ec);
            return false;
        }
    }
    Private(tmp);
    std::error_code ec;
    fs::rename(tmp, p, ec); // remplace l'ancien fichier (écrase sous Windows et POSIX)
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

} // namespace data_store
