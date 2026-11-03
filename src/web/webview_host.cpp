#include "web/webview_host.h"

#include "common/util.h"

#include <WebView2.h>
#include <shellapi.h>

#include <atomic>

namespace webview {
namespace {

constexpr wchar_t kHost[] = L"aeria.local";
constexpr wchar_t kRegKey[] = L"Software\\Aeria";
constexpr wchar_t kLowMemoryValue[] = L"LowMemory";
constexpr UINT_PTR kRetryTimer = 0x5752; // n'entre pas en conflit avec les minuteurs du dock

ICoreWebView2Controller* g_controller = nullptr;
ICoreWebView2* g_view = nullptr;
HWND g_parent = nullptr;
MessageHandler g_onMessage;
RECT g_bounds{};
bool g_visible = true;
bool g_suspended = false;
std::wstring g_baseArgs; // options Chromium déjà présentes au lancement (ex. port de débogage)
bool g_baseArgsRead = false;
int g_attempts = 0;

// Implémentation COM minimale d'un handler WebView2 à partir d'une lambda.
template <class I, class... A>
class Handler final : public I {
public:
    Handler(REFIID iid, std::function<HRESULT(A...)> fn) : iid_(iid), fn_(std::move(fn)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, iid_)) {
            *ppv = static_cast<I*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --ref_;
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE Invoke(A... args) override { return fn_(args...); }

private:
    IID iid_;
    std::atomic<ULONG> ref_{1};
    std::function<HRESULT(A...)> fn_;
};

void Fail(const wchar_t* text) {
    MessageBoxW(nullptr, text, L"Aeria", MB_ICONERROR);
    PostQuitMessage(1);
}

// Après un changement d'options, l'ancien processus du navigateur met un moment à se fermer :
// d'ici là, créer l'environnement échoue. On réessaie toutes les 200 ms pendant 5 s.
bool RetryLater() {
    if (++g_attempts > 25) return false;
    SetTimer(g_parent, kRetryTimer, 200, [](HWND hwnd, UINT, UINT_PTR id, DWORD) {
        KillTimer(hwnd, id);
        Create(g_parent, g_onMessage);
    });
    return true;
}

void OpenExternal(const std::wstring& uri) {
    if (uri.rfind(L"http", 0) == 0) ShellExecuteW(nullptr, L"open", uri.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

bool IsAppUri(const std::wstring& uri) { return uri.rfind(L"https://" + std::wstring(kHost) + L"/", 0) == 0; }

void RegisterEvents() {
    EventRegistrationToken token;

    auto* onMessage = new Handler<ICoreWebView2WebMessageReceivedEventHandler, ICoreWebView2*,
                                  ICoreWebView2WebMessageReceivedEventArgs*>(
        IID_ICoreWebView2WebMessageReceivedEventHandler,
        [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) {
            LPWSTR raw = nullptr;
            if (SUCCEEDED(args->TryGetWebMessageAsString(&raw)) && raw) {
                if (g_onMessage) g_onMessage(util::Narrow(raw));
                CoTaskMemFree(raw);
            }
            return S_OK;
        });
    g_view->add_WebMessageReceived(onMessage, &token);
    onMessage->Release();

    // Les liens externes s'ouvrent dans le navigateur par défaut, jamais dans le dock.
    auto* onNewWindow = new Handler<ICoreWebView2NewWindowRequestedEventHandler, ICoreWebView2*,
                                    ICoreWebView2NewWindowRequestedEventArgs*>(
        IID_ICoreWebView2NewWindowRequestedEventHandler,
        [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) {
            LPWSTR uri = nullptr;
            args->get_Uri(&uri);
            if (uri) {
                OpenExternal(uri);
                CoTaskMemFree(uri);
            }
            args->put_Handled(TRUE);
            return S_OK;
        });
    g_view->add_NewWindowRequested(onNewWindow, &token);
    onNewWindow->Release();

    auto* onNavigate = new Handler<ICoreWebView2NavigationStartingEventHandler, ICoreWebView2*,
                                   ICoreWebView2NavigationStartingEventArgs*>(
        IID_ICoreWebView2NavigationStartingEventHandler,
        [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) {
            LPWSTR uri = nullptr;
            args->get_Uri(&uri);
            if (uri) {
                if (!IsAppUri(uri)) {
                    args->put_Cancel(TRUE);
                    OpenExternal(uri);
                }
                CoTaskMemFree(uri);
            }
            return S_OK;
        });
    g_view->add_NavigationStarting(onNavigate, &token);
    onNavigate->Release();

    // Si le processus du navigateur meurt (il porte aussi le GPU en mode économie de mémoire),
    // on recrée toute la vue ; si seule la page tombe, on la recharge.
    auto* onFailed = new Handler<ICoreWebView2ProcessFailedEventHandler, ICoreWebView2*,
                                 ICoreWebView2ProcessFailedEventArgs*>(
        IID_ICoreWebView2ProcessFailedEventHandler,
        [](ICoreWebView2* sender, ICoreWebView2ProcessFailedEventArgs* args) {
            COREWEBVIEW2_PROCESS_FAILED_KIND kind = COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED;
            args->get_ProcessFailedKind(&kind);
            if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED)
                PostMessageW(g_parent, WM_APP_WEBVIEW_RESTART, 0, 0); // hors de l'évènement
            else if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED ||
                     kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE)
                sender->Reload();
            return S_OK;
        });
    g_view->add_ProcessFailed(onFailed, &token);
    onFailed->Release();
}

HRESULT OnControllerCreated(HRESULT hr, ICoreWebView2Controller* controller) {
    if (FAILED(hr) || !controller) {
        if (!RetryLater()) Fail(L"Impossible de créer la vue WebView2.");
        return hr;
    }
    g_attempts = 0;
    g_controller = controller;
    g_controller->AddRef();
    g_controller->get_CoreWebView2(&g_view);

    ICoreWebView2Controller2* c2 = nullptr;
    if (SUCCEEDED(g_controller->QueryInterface(IID_ICoreWebView2Controller2, (void**)&c2))) {
        c2->put_DefaultBackgroundColor({0, 0, 0, 0}); // transparent : on voit l'acrylique
        c2->Release();
    }

    ICoreWebView2Settings* settings = nullptr;
    if (SUCCEEDED(g_view->get_Settings(&settings))) {
        settings->put_IsStatusBarEnabled(FALSE);
        settings->put_IsZoomControlEnabled(FALSE);
        settings->Release();
    }

    RegisterEvents();

    ICoreWebView2_3* wv3 = nullptr;
    if (SUCCEEDED(g_view->QueryInterface(IID_ICoreWebView2_3, (void**)&wv3))) {
        std::wstring ui = util::ExeDir() + L"\\ui";
        wv3->SetVirtualHostNameToFolderMapping(kHost, ui.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
        wv3->Release();
    }
    g_controller->put_Bounds(g_bounds);
    g_controller->put_IsVisible(g_visible);
    g_view->Navigate((L"https://" + std::wstring(kHost) + L"/index.html").c_str());
    return S_OK;
}

} // namespace

bool Create(HWND parent, MessageHandler onMessage) {
    g_parent = parent;
    g_onMessage = std::move(onMessage);
    using CreateEnvFn = HRESULT(STDAPICALLTYPE*)(PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions*,
                                                ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);
    HMODULE loader = LoadLibraryW((util::ExeDir() + L"\\WebView2Loader.dll").c_str());
    auto create = loader ? (CreateEnvFn)(void*)GetProcAddress(loader, "CreateCoreWebView2EnvironmentWithOptions") : nullptr;
    if (!create) {
        Fail(L"WebView2Loader.dll est introuvable à côté de l'exécutable.");
        return false;
    }
    SetEnvironmentVariableW(L"WEBVIEW2_DEFAULT_BACKGROUND_COLOR", L"00000000");
    // Économie de mémoire : le GPU dans le processus du navigateur évite un processus GPU
    // séparé (~200 Mo de moins), mais Chromium passe alors en compositing logiciel
    // (animations à ~40 images/s au lieu de 120). Réglable dans les paramètres.
    if (!g_baseArgsRead) {
        g_baseArgsRead = true;
        wchar_t existing[2048] = L"";
        GetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", existing, 2048);
        g_baseArgs = existing;
    }
    std::wstring args = g_baseArgs;
    if (LowMemory() && args.find(L"--in-process-gpu") == std::wstring::npos)
        args += (args.empty() ? L"" : L" ") + std::wstring(L"--in-process-gpu");
    SetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", args.c_str());
    std::wstring userData = util::LocalDataDir() + L"\\WebView2";

    auto* onEnv = new Handler<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, HRESULT,
                              ICoreWebView2Environment*>(
        IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
        [parent](HRESULT hr, ICoreWebView2Environment* env) {
            if (FAILED(hr) || !env) {
                if (!RetryLater())
                    Fail(L"Le runtime WebView2 est introuvable. Installe « Microsoft Edge WebView2 Runtime ».");
                return hr;
            }
            auto* onController = new Handler<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, HRESULT,
                                             ICoreWebView2Controller*>(
                IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, OnControllerCreated);
            env->CreateCoreWebView2Controller(parent, onController);
            onController->Release();
            return S_OK;
        });
    HRESULT hr = create(nullptr, userData.c_str(), nullptr, onEnv);
    onEnv->Release();
    if (FAILED(hr) && !RetryLater()) Fail(L"Impossible de créer la vue WebView2.");
    return true;
}

bool LowMemory() {
    DWORD value = 1, size = sizeof(value); // activé par défaut
    RegGetValueW(HKEY_CURRENT_USER, kRegKey, kLowMemoryValue, RRF_RT_REG_DWORD, nullptr, &value, &size);
    return value != 0;
}

void SetLowMemory(bool on) {
    if (on == LowMemory()) return;
    DWORD value = on ? 1 : 0;
    RegSetKeyValueW(HKEY_CURRENT_USER, kRegKey, kLowMemoryValue, REG_DWORD, &value, sizeof(value));
    // Les options du navigateur ne s'appliquent qu'à sa création. On recrée la vue hors
    // de l'évènement en cours (cet appel vient d'un message de la page).
    PostMessageW(g_parent, WM_APP_WEBVIEW_RESTART, 0, 0);
}

void SetBounds(const RECT& bounds) {
    if (EqualRect(&bounds, &g_bounds)) return;
    g_bounds = bounds;
    if (g_controller) g_controller->put_Bounds(bounds);
}

// WebView2 ne suit pas seul la visibilité de la fenêtre parente : sans ça,
// la page reste « hidden » et ne se redessine plus après un ShowWindow.
// Caché, on met aussi la vue en veille : cible mémoire basse (Chromium libère
// les textures du GPU et ses caches) puis suspension de la page.
void SetVisible(bool visible, bool suspend) {
    bool changed = visible != g_visible;
    g_visible = visible;
    if (!g_controller || !g_view) return;

    ICoreWebView2_3* wv3 = nullptr;
    ICoreWebView2_19* wv19 = nullptr;
    g_view->QueryInterface(IID_ICoreWebView2_3, (void**)&wv3);
    g_view->QueryInterface(IID_ICoreWebView2_19, (void**)&wv19);
    if (visible) {
        if (wv3) wv3->Resume();
        if (wv19) wv19->put_MemoryUsageTargetLevel(COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_NORMAL);
        g_controller->put_IsVisible(TRUE);
        g_suspended = false;
    } else if (changed) {
        g_controller->put_IsVisible(FALSE); // préalable à TrySuspend
        if (wv19) wv19->put_MemoryUsageTargetLevel(COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW);
        if (wv3 && suspend) {
            g_suspended = true;
            auto* done = new Handler<ICoreWebView2TrySuspendCompletedHandler, HRESULT, BOOL>(
                IID_ICoreWebView2TrySuspendCompletedHandler, [](HRESULT, BOOL) { return S_OK; });
            wv3->TrySuspend(done);
            done->Release();
        }
    }
    if (wv3) wv3->Release();
    if (wv19) wv19->Release();
}

void Wake() {
    if (!g_suspended || !g_view) return;
    g_suspended = false;
    ICoreWebView2_3* wv3 = nullptr;
    if (SUCCEEDED(g_view->QueryInterface(IID_ICoreWebView2_3, (void**)&wv3))) {
        wv3->Resume();
        wv3->Release();
    }
}

void Focus() {
    if (g_controller) g_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
}

void PostJson(const std::wstring& json) {
    if (g_view) g_view->PostWebMessageAsJson(json.c_str());
}

void Restart() {
    Close();
    Create(g_parent, g_onMessage);
}

void Close() {
    g_suspended = false;
    if (g_controller) {
        g_controller->Close();
        g_controller->Release();
        g_controller = nullptr;
    }
    if (g_view) {
        g_view->Release();
        g_view = nullptr;
    }
}

} // namespace webview
