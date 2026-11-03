#include "ai/anthropic_http.h"

#include "ai/api_key.h"
#include "common/util.h"
#include "web/bridge.h"

#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace anthropic_http {
namespace {

constexpr wchar_t kApiVersion[] = L"2026-10-21";

struct Request {
    std::string id;
    std::mutex mutex; // protège handle et closed
    HINTERNET handle = nullptr;
    bool closed = false;
    std::atomic<bool> cancelled{false};
};

std::map<std::string, std::shared_ptr<Request>> g_requests;
std::mutex g_requestsMutex;

void CloseHandleOnce(Request& r) {
    std::lock_guard lock(r.mutex);
    if (!r.closed && r.handle) WinHttpCloseHandle(r.handle);
    r.closed = true;
}

// Services gratuits (API compatibles OpenAI) : hôte fixe et seul préfixe de chemin autorisé.
struct FreeService {
    const char* name;
    const wchar_t* host;
    const char* pathPrefix;
};
const FreeService kFree[] = {
    {"groq", L"api.groq.com", "/openai/v1/"},
    {"gemini", L"generativelanguage.googleapis.com", "/v1beta/openai/"},
    {"openai", L"api.openai.com", "/v1/"},
};

// headers : en-têtes complets, clé comprise (chaque ligne terminée par \r\n).
void Run(std::shared_ptr<Request> req, std::wstring host, std::wstring method, std::wstring path, std::wstring headers,
         std::string body) {
    const std::string id = util::Json(req->id);
    int status = 0;
    std::string errorBody;
    DWORD lastError = 0;

    HINTERNET session = WinHttpOpen(L"Aeria/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connect = session ? WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0) : nullptr;
    HINTERNET handle = connect ? WinHttpOpenRequest(connect, method.c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                    WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                               : nullptr;
    {
        std::lock_guard lock(req->mutex);
        req->handle = handle;
        if (req->cancelled && handle) {
            WinHttpCloseHandle(handle);
            req->closed = true;
        }
    }

    bool ok = handle && !req->closed;
    if (ok) {
        WinHttpSetTimeouts(handle, 0, 30000, 60000, 300000);
        ok = WinHttpSendRequest(handle, headers.c_str(), (DWORD)-1L, body.empty() ? WINHTTP_NO_REQUEST_DATA : body.data(),
                                (DWORD)body.size(), (DWORD)body.size(), 0) &&
             WinHttpReceiveResponse(handle, nullptr);
    }
    if (ok) {
        DWORD code = 0, size = sizeof code;
        WinHttpQueryHeaders(handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &code, &size, WINHTTP_NO_HEADER_INDEX);
        status = (int)code;
        std::string pending;
        std::vector<char> buf(16384);
        while (true) {
            DWORD avail = 0, read = 0;
            if (!WinHttpQueryDataAvailable(handle, &avail)) {
                ok = false;
                break;
            }
            if (avail == 0) break;
            if (avail > buf.size()) buf.resize(avail);
            if (!WinHttpReadData(handle, buf.data(), avail, &read)) {
                ok = false;
                break;
            }
            if (status != 200) {
                errorBody.append(buf.data(), read);
                continue;
            }
            pending.append(buf.data(), read);
            // On ne relaie que des lignes complètes : jamais de caractère UTF-8 coupé en deux.
            size_t cut = pending.rfind('\n');
            if (cut != std::string::npos) {
                bridge::PostToPage("{\"t\":\"chunk\",\"id\":" + id + ",\"data\":" + util::Json(pending.substr(0, cut + 1)) + "}");
                pending.erase(0, cut + 1);
            }
        }
        if (!pending.empty() && status == 200)
            bridge::PostToPage("{\"t\":\"chunk\",\"id\":" + id + ",\"data\":" + util::Json(pending + "\n") + "}");
    }
    if (!ok) lastError = GetLastError();

    CloseHandleOnce(*req);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);

    if (req->cancelled) {
        bridge::PostToPage("{\"t\":\"done\",\"id\":" + id + ",\"cancelled\":true}");
    } else if (!ok) {
        bridge::PostToPage("{\"t\":\"error\",\"id\":" + id + ",\"status\":0,\"body\":" +
                           util::Json("Erreur réseau (code " + std::to_string(lastError) + "). Vérifie ta connexion.") + "}");
    } else if (status != 200) {
        bridge::PostToPage("{\"t\":\"error\",\"id\":" + id + ",\"status\":" + std::to_string(status) + ",\"body\":" +
                           util::Json(errorBody) + "}");
    } else {
        bridge::PostToPage("{\"t\":\"done\",\"id\":" + id + "}");
    }
    std::lock_guard lock(g_requestsMutex);
    g_requests.erase(req->id);
}

void Launch(const std::string& id, std::wstring host, const std::string& method, const std::string& path,
            std::wstring headers, const std::string& body) {
    auto req = std::make_shared<Request>();
    req->id = id;
    {
        std::lock_guard lock(g_requestsMutex);
        g_requests[id] = req;
    }
    std::thread(Run, req, std::move(host), util::Widen(method), util::Widen(path), std::move(headers), body).detach();
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
    std::wstring headers = L"content-type: application/json\r\nanthropic-version: " + std::wstring(kApiVersion) +
                           L"\r\nx-api-key: " + util::Widen(key) + L"\r\n";
    if (!betas.empty()) headers += L"anthropic-beta: " + util::Widen(betas) + L"\r\n";
    Launch(id, L"api.anthropic.com", method, path, std::move(headers), body);
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
    std::wstring headers = L"content-type: application/json\r\nauthorization: Bearer " + util::Widen(key) + L"\r\n";
    Launch(id, s->host, method, path, std::move(headers), body);
}

bool Cancel(const std::string& id) {
    std::shared_ptr<Request> req;
    {
        std::lock_guard lock(g_requestsMutex);
        auto it = g_requests.find(id);
        if (it == g_requests.end()) return false;
        req = it->second;
    }
    req->cancelled = true;
    CloseHandleOnce(*req);
    return true;
}

} // namespace anthropic_http
