// Parties de la mise à jour indépendantes du système (Windows et Linux).
#include "features/updater.h"

#include <cctype>

#ifndef AERIA_VERSION_STR
#define AERIA_VERSION_STR "0.0.0"
#endif

namespace updater {

const char* Version() { return AERIA_VERSION_STR; }

bool IsValidTag(const std::string& tag) {
    // v<n>.<n>.<n>, chaque nombre de 1 à 5 chiffres.
    if (tag.size() < 6 || tag.size() > 18 || tag[0] != 'v') return false;
    int groups = 0;
    size_t digits = 0;
    for (size_t i = 1; i <= tag.size(); ++i) {
        if (i == tag.size() || tag[i] == '.') {
            if (digits == 0 || digits > 5) return false;
            ++groups;
            digits = 0;
        } else if (std::isdigit((unsigned char)tag[i])) {
            ++digits;
        } else {
            return false;
        }
    }
    return groups == 3;
}

std::string AssetName(const std::string& tag) {
#ifdef _WIN32
    return IsValidTag(tag) ? "Aeria-" + tag + "-windows-x64-setup.exe" : std::string();
#else
    (void)tag;
    return {};
#endif
}

std::string ExpectedHash(const std::string& sums, const std::string& file) {
    size_t pos = 0;
    while (pos < sums.size()) {
        size_t end = sums.find('\n', pos);
        if (end == std::string::npos) end = sums.size();
        std::string line = sums.substr(pos, end - pos);
        pos = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.size() < 66) continue;
        std::string hash = line.substr(0, 64);
        bool hex = true;
        for (char& c : hash) {
            if (!std::isxdigit((unsigned char)c)) hex = false;
            c = (char)std::tolower((unsigned char)c);
        }
        if (!hex || line[64] != ' ') continue;
        size_t nameStart = 64;
        while (nameStart < line.size() && line[nameStart] == ' ') ++nameStart;
        if (nameStart < line.size() && line[nameStart] == '*') ++nameStart; // mode binaire de sha256sum
        if (line.compare(nameStart, std::string::npos, file) == 0) return hash;
    }
    return {};
}

} // namespace updater
