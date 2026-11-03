// Petites fonctions partagées : conversions UTF-8/UTF-16 (Windows), JSON, chemins.
#pragma once

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

#include <string>
#include <vector>

namespace util {

// Chaîne JSON entre guillemets, échappée.
std::string Json(const std::string& s);

// Découpe s sur sep ; avec maxParts, le dernier morceau garde le reste tel quel.
std::vector<std::string> Split(const std::string& s, char sep, size_t maxParts = 0);

// Les n derniers octets de s.
std::string Tail(const std::string& s, size_t n);

std::string Base64(const std::string& bytes);

long long NowEpochMs();

#ifdef _WIN32
std::wstring Widen(const std::string& utf8);
std::string Narrow(const std::wstring& utf16);

std::wstring KnownFolder(REFKNOWNFOLDERID id);
std::wstring RoamingDataDir(); // %APPDATA%\Aeria (créé au besoin)
std::wstring LocalDataDir();   // %LOCALAPPDATA%\Aeria (créé au besoin)
std::wstring ExeDir();
bool FileExists(const std::wstring& path);
#else
std::string HomeDir();   // $HOME
std::string ConfigDir(); // $XDG_CONFIG_HOME/Aeria (créé au besoin)
std::string DataDir();   // $XDG_DATA_HOME/Aeria (créé au besoin)
std::string ExeDir();
#endif

} // namespace util
