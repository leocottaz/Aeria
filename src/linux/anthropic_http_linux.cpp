// Requêtes HTTPS sous Linux (libcurl, un thread par requête) — mêmes règles que la version
// WinHTTP : flux relayé par lignes complètes ("chunk"), puis "done" ou "error".
#include "ai/anthropic_http.h"

#include "ai/api_key.h"
#include "common/util.h"
#include "web/bridge.h"

#include <curl/curl.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace anthropic_http {
namespace {

constexpr char kApiVersion[] = "2026-10-21";

struct Request {
    std::string id;
    std::atomic<bool> cancelled{false};
};

std::map<std::string, std::shared_ptr<Request>> g_requests;
std::mutex g_requestsMutex;

struct FreeService {
    const char* name;
    const char* host;
    const char* pathPrefix;
};
const FreeService kFree[] = {
    {"groq", "api.groq.com", "/openai/v1/"},
    {"gemini", "generativelanguage.googleapis.com", "/v1beta/openai/"},
    {"openai", "api.openai.com", "/v1/"},
};

struct Transfer {
    Request* req;
    std::string jsonId;
    std::string pending;   // flux 200 : lignes incomplètes
    std::string errorBody; // autres statuts
};

int OnProgress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<Transfer*>(user)->req->cancelled ? 1 : 0; // non nul : interrompt le transfert
}

// headers : une ligne par en-tête, clé comprise.
void Run(std::shared_ptr<Request> req, std::string host, std::string method, std::string path,
         std::vector<std::string> headers, std::string body) {
    const std::string id = util::Json(req->id);
    Transfer t{req.get(), id, {}, {}};
    CURL* curl = curl_easy_init();
    curl_slist* list = nullptr;
    for (const auto& h : headers) list = curl_slist_append(list, h.c_str());

    long status = 0;
    CURLcode rc = CURLE_FAILED_INIT;
    if (curl) {
        // Le statut n'est connu qu'après les en-têtes : on trie le corps dans la fonction d'écriture.
        struct Ctx {
            CURL* curl;
            Transfer* t;
        } ctx{curl, &t};
        auto write = +[](char* data, size_t size, size_t count, void* user) -> size_t {
            auto* c = static_cast<Ctx*>(user);
            size_t n = size * count;
            long code = 0;
            curl_easy_getinfo(c->curl, CURLINFO_RESPONSE_CODE, &code);
            if (code != 200) {
                c->t->errorBody.append(data, n);
                return n;
            }
            c->t->pending.append(data, n);
            // On ne relaie que des lignes complètes : jamais de caractère UTF-8 coupé en deux.
            size_t cut = c->t->pending.rfind('\n');
            if (cut != std::string::npos) {
                bridge::PostToPage("{\"t\":\"chunk\",\"id\":" + c->t->jsonId + ",\"data\":" +
                                   util::Json(c->t->pending.substr(0, cut + 1)) + "}");
                c->t->pending.erase(0, cut + 1);
            }
            return n;
        };
        std::string url = "https://" + host + path;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Aeria/1.0");
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
        if (!body.empty()) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body.size());
        }
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L); // pas un octet pendant 5 min : abandon
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 300L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, OnProgress);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &t);
        rc = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_cleanup(curl);
    }
    curl_slist_free_all(list);

    bool ok = rc == CURLE_OK;
    if (ok && status == 200 && !t.pending.empty())
        bridge::PostToPage("{\"t\":\"chunk\",\"id\":" + id + ",\"data\":" + util::Json(t.pending + "\n") + "}");

    if (req->cancelled) {
        bridge::PostToPage("{\"t\":\"done\",\"id\":" + id + ",\"cancelled\":true}");
    } else if (!ok) {
        bridge::PostToPage("{\"t\":\"error\",\"id\":" + id + ",\"status\":0,\"body\":" +
                           util::Json(std::string("Erreur réseau (") + curl_easy_strerror(rc) + "). Vérifie ta connexion.") + "}");
    } else if (status != 200) {
        bridge::PostToPage("{\"t\":\"error\",\"id\":" + id + ",\"status\":" + std::to_string(status) + ",\"body\":" +
                           util::Json(t.errorBody) + "}");
    } else {
        bridge::PostToPage("{\"t\":\"done\",\"id\":" + id + "}");
    }
    std::lock_guard lock(g_requestsMutex);
    g_requests.erase(req->id);
}

void Launch(const std::string& id, std::string host, const std::string& method, const std::string& path,
            std::vector<std::string> headers, const std::string& body) {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    auto req = std::make_shared<Request>();
    req->id = id;
    {
        std::lock_guard lock(g_requestsMutex);
        g_requests[id] = req;
    }
    std::thread(Run, req, std::move(host), method, path, std::move(headers), body).detach();
}

void PostError(const std::string& id, int status, const std::string& text) {
    bridge::PostToPage("{\"t\":\"error\",\"id\":" + util::Json(id) + ",\"status\":" + std::to_string(status) +
                       ",\"body\":" + util::Json(text) + "}");
}

} // namespace

void Start(const std::string& id, const std::string& method, const std::string& path, const std::string& betas,
           const std::string& body) {
    std::string key = api_key::Get();
    if (key.empty()) {
        PostError(id, -1, "Aucune clé API enregistrée.");
        return;
    }
    std::vector<std::string> headers = {"content-type: application/json", std::string("anthropic-version: ") + kApiVersion,
                                        "x-api-key: " + key};
    if (!betas.empty()) headers.push_back("anthropic-beta: " + betas);
    Launch(id, "api.anthropic.com", method, path, std::move(headers), body);
}

void StartFree(const std::string& id, const std::string& service, const std::string& method, const std::string& path,
               const std::string& body) {
    const FreeService* s = nullptr;
    for (const auto& f : kFree)
        if (service == f.name) s = &f;
    if (!s || path.rfind(s->pathPrefix, 0) != 0 || path.find("..") != std::string::npos) {
        PostError(id, -2, "Service ou adresse non autorisés.");
        return;
    }
    std::string key = api_key::Get(service);
    if (key.empty()) {
        PostError(id, -1, "Aucune clé enregistrée pour ce service gratuit.");
        return;
    }
    Launch(id, s->host, method, path, {"content-type: application/json", "authorization: Bearer " + key}, body);
}

bool Cancel(const std::string& id) {
    std::lock_guard lock(g_requestsMutex);
    auto it = g_requests.find(id);
    if (it == g_requests.end()) return false;
    it->second->cancelled = true;
    return true;
}

} // namespace anthropic_http
