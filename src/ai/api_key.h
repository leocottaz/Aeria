// Clés API, chiffrées avec DPAPI (liées à la session Windows) dans %APPDATA%\Aeria :
// Anthropic (apikey.bin) et celles des services compatibles OpenAI : Groq, Gemini, OpenAI (<service>key.bin).
// Elles ne sont jamais envoyées à la page.
#pragma once

#include <string>

namespace api_key {

// Service : "anthropic" (par défaut), "groq", "gemini" ou "openai". Tout autre nom est refusé.
bool IsKnown(const std::string& slot);

void Load(); // toutes les clés
bool Save(const std::string& key, const std::string& slot = "anthropic");
void Remove(const std::string& slot = "anthropic");
std::string Get(const std::string& slot = "anthropic"); // copie, sûre entre threads

// Fragment JSON "hasKey":…,"keyHint":"…" (4 derniers caractères).
std::string StatusJson(const std::string& slot = "anthropic");

// Fragment JSON "freeKeys":{"groq":{"hasKey":…,"keyHint":…},"gemini":{…}}.
std::string FreeStatusJson();

} // namespace api_key
