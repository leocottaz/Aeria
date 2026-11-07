#include "features/appnotifs.h"

#include "common/util.h"
#include "web/bridge.h"

#include <windows.h>
#include <inspectable.h>
#include <roapi.h>
#include <winstring.h>
#include <shcore.h>
#include <shobjidl.h>
#include <objidl.h>
#include <gdiplus.h>
#include <windows.applicationmodel.h>
#include <windows.storage.streams.h>
#include <windows.ui.notifications.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Windows.UI.Notifications.Management n'est pas dans les en-têtes de MinGW : on déclare à la main la
// vtable du listener (ordre vérifié sur ce PC par une sonde : 6 RequestAccessAsync, 7 GetAccessStatus,
// 8-9 évènement NotificationChanged, 10 GetNotificationsAsync). Les IID ont été lus avec GetIids().
// Les autres interfaces (UserNotification, AppInfo…) viennent des en-têtes.
// Namespace NOMMÉ obligatoire (voir Pièges : namespace anonyme = plantage en Release).
// ---------------------------------------------------------------------------

namespace appnotifs_abi {

template <class R>
struct IAsyncOp : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE put_Completed(void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults(R* results) = 0;
};

struct IAsyncInfoAbi : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Id(UINT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Status(int*) = 0; // 0 en cours, 1 terminé, 2 annulé, 3 erreur
    virtual HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE Cancel() = 0;
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};
const IID IID_IAsyncInfoAbi = {0x00000036, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

template <class T>
struct IVectorViewAbi : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetAt(UINT32 index, T* item) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(UINT32* size) = 0;
};

struct IListener : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE RequestAccessAsync(IInspectable** op) = 0;       // -> UserNotificationListenerAccessStatus
    virtual HRESULT STDMETHODCALLTYPE GetAccessStatus(int* status) = 0;                // 0 non précisé, 1 autorisé, 2 refusé
    virtual HRESULT STDMETHODCALLTYPE add_NotificationChanged(void*, INT64*) = 0;      // inutilisé : réservé aux applis empaquetées
    virtual HRESULT STDMETHODCALLTYPE remove_NotificationChanged(INT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetNotificationsAsync(UINT32 kinds, IInspectable** op) = 0; // 1 = toast
};
const IID IID_IListener = {0x62553E41, 0x8A06, 0x4CEF, {0x82, 0x15, 0x60, 0x33, 0xA5, 0xBE, 0x4B, 0x03}};

struct IStatics : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Current(IInspectable** listener) = 0;
};
const IID IID_IStatics = {0xFF6123CF, 0x4386, 0x4AA3, {0xB7, 0x3D, 0xB8, 0x04, 0xE5, 0xB6, 0x3B, 0x23}};

} // namespace appnotifs_abi

namespace appnotifs {
namespace {

using namespace appnotifs_abi;
using namespace ABI::Windows::UI::Notifications;
namespace UN = ABI::Windows::UI::Notifications; // « IUserNotification » existe aussi dans shobjidl.h : on qualifie
using namespace ABI::Windows::ApplicationModel;

constexpr int kPollMs = 1500;
constexpr size_t kMaxChars = 160; // par texte : la pastille tient sur 2 lignes

std::atomic<int> g_gen{0};   // change à chaque démarrage/arrêt : l'ancien thread s'arrête
bool g_running = false;

template <class T>
struct Ptr { // pointeur COM minimal
    T* p = nullptr;
    Ptr() = default;
    Ptr(const Ptr&) = delete;
    Ptr& operator=(const Ptr&) = delete;
    ~Ptr() { reset(); }
    void reset() {
        if (p) p->Release();
        p = nullptr;
    }
    T** put() {
        reset();
        return &p;
    }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

// Attend la fin d'une opération asynchrone WinRT (thread de fond).
template <class R>
bool Await(IInspectable* op, R* result, DWORD timeoutMs = 4000) {
    if (!op) return false;
    Ptr<IAsyncInfoAbi> info;
    if (FAILED(op->QueryInterface(IID_IAsyncInfoAbi, (void**)info.put()))) return false;
    int status = 0;
    ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (SUCCEEDED(info->get_Status(&status)) && status == 0) {
        if (GetTickCount64() > deadline) {
            info->Cancel();
            return false;
        }
        Sleep(5);
    }
    if (status != 1) return false;
    return SUCCEEDED(static_cast<IAsyncOp<R>*>(static_cast<void*>(op))->GetResults(result));
}

// Texte d'une HSTRING (libérée), sans retours à la ligne, tronqué sans couper une paire de substitution.
std::string TakeText(HSTRING h, size_t maxChars = kMaxChars) {
    if (!h) return {};
    UINT32 len = 0;
    const wchar_t* raw = WindowsGetStringRawBuffer(h, &len);
    std::wstring w(raw, len);
    WindowsDeleteString(h);
    for (auto& c : w)
        if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
    if (w.size() > maxChars) {
        size_t cut = maxChars;
        if (cut > 0 && w[cut - 1] >= 0xD800 && w[cut - 1] <= 0xDBFF) cut--;
        w = w.substr(0, cut) + L"…";
    }
    return util::Narrow(w);
}

// Logo de l'appli en data URL (PNG/JPEG), vide s'il est indisponible ou trop gros.
std::string LogoDataUrl(IAppDisplayInfo* display) {
    ABI::Windows::Foundation::Size size{64.0f, 64.0f};
    Ptr<ABI::Windows::Storage::Streams::IRandomAccessStreamReference> ref;
    if (FAILED(display->GetLogo(size, ref.put())) || !ref) return {};
    Ptr<IInspectable> op;
    using OpenOp = ABI::Windows::Foundation::IAsyncOperation<ABI::Windows::Storage::Streams::IRandomAccessStreamWithContentType*>;
    if (FAILED(ref->OpenReadAsync(reinterpret_cast<OpenOp**>(op.put()))) || !op) return {};
    Ptr<IInspectable> winrtStream;
    if (!Await(op.p, winrtStream.put()) || !winrtStream) return {};
    Ptr<IStream> stream;
    if (FAILED(CreateStreamOverRandomAccessStream(winrtStream.p, IID_IStream, (void**)stream.put()))) return {};
    std::string bytes;
    char buf[16384];
    ULONG read = 0;
    while (SUCCEEDED(stream->Read(buf, sizeof buf, &read)) && read > 0) {
        bytes.append(buf, read);
        if (bytes.size() > 200 * 1024) return {}; // garde-fou : un logo, pas une photo
    }
    if (bytes.size() < 8) return {};
    const unsigned char b0 = (unsigned char)bytes[0];
    const char* mime = b0 == 0x89 && bytes[1] == 'P' ? "image/png" : b0 == 0xFF ? "image/jpeg" : nullptr;
    return mime ? std::string("data:") + mime + ";base64," + util::Base64(bytes) : std::string();
}

// Les applis non empaquetées (Win32 : navigateurs, PowerShell…) n'ont pas de logo côté WinRT (GetLogo rend une
// référence nulle) : on prend l'icône que l'Explorateur leur donne dans « shell:AppsFolder\<AUMID> ».
struct GdiPlusSession { // GDI+ n'est utilisé que par le thread de relevé
    ULONG_PTR token = 0;
    GdiPlusSession() {
        Gdiplus::GdiplusStartupInput in;
        Gdiplus::GdiplusStartup(&token, &in, nullptr);
    }
    ~GdiPlusSession() {
        if (token) Gdiplus::GdiplusShutdown(token);
    }
};

bool PngEncoderClsid(CLSID* out) {
    UINT n = 0, bytes = 0;
    Gdiplus::GetImageEncodersSize(&n, &bytes);
    if (!n || !bytes) return false;
    std::vector<char> buf(bytes);
    auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
    Gdiplus::GetImageEncoders(n, bytes, codecs);
    for (UINT i = 0; i < n; i++)
        if (!wcscmp(codecs[i].MimeType, L"image/png")) {
            *out = codecs[i].Clsid;
            return true;
        }
    return false;
}

std::string ShellIconDataUrl(const std::wstring& aumid) {
    if (aumid.empty()) return {};
    Ptr<IShellItemImageFactory> factory;
    std::wstring path = L"shell:AppsFolder\\" + aumid;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_IShellItemImageFactory, (void**)factory.put())) || !factory)
        return {};
    HBITMAP hbm = nullptr;
    if (FAILED(factory->GetImage(SIZE{64, 64}, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hbm)) || !hbm) return {};
    BITMAP bm{};
    GetObjectW(hbm, sizeof bm, &bm);
    std::string result;
    if (bm.bmWidth > 0 && bm.bmHeight > 0 && bm.bmWidth <= 512 && bm.bmHeight <= 512) {
        // Pixels BGRA de haut en bas (hauteur négative), sans dépendre du sens de la bitmap d'origine.
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = bm.bmWidth;
        bi.bmiHeader.biHeight = -bm.bmHeight;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        std::vector<BYTE> pixels((size_t)bm.bmWidth * bm.bmHeight * 4);
        HDC dc = GetDC(nullptr);
        int rows = GetDIBits(dc, hbm, 0, bm.bmHeight, pixels.data(), &bi, DIB_RGB_COLORS);
        ReleaseDC(nullptr, dc);
        if (rows == bm.bmHeight) {
            bool anyAlpha = false;
            for (size_t i = 3; i < pixels.size(); i += 4)
                if (pixels[i]) { anyAlpha = true; break; }
            if (!anyAlpha) // icône sans canal alpha : opaque
                for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;
            CLSID png;
            Gdiplus::Bitmap image(bm.bmWidth, bm.bmHeight, bm.bmWidth * 4, PixelFormat32bppPARGB, pixels.data());
            IStream* stream = nullptr;
            if (PngEncoderClsid(&png) && SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) && stream) {
                if (image.Save(stream, &png, nullptr) == Gdiplus::Ok) {
                    STATSTG st{};
                    HGLOBAL mem = nullptr;
                    if (SUCCEEDED(stream->Stat(&st, STATFLAG_NONAME)) && SUCCEEDED(GetHGlobalFromStream(stream, &mem)) && mem) {
                        void* data = GlobalLock(mem);
                        if (data && st.cbSize.QuadPart > 0 && st.cbSize.QuadPart < 200 * 1024)
                            result = "data:image/png;base64," + util::Base64(std::string((const char*)data, (size_t)st.cbSize.QuadPart));
                        if (data) GlobalUnlock(mem);
                    }
                }
                stream->Release();
            }
        }
    }
    DeleteObject(hbm);
    return result;
}

// Nom et logo de l'appli qui a posté la notification. Le logo est gardé par nom d'appli (icons) : on ne le relit pas.
std::string AppDetails(UN::IUserNotification* un, std::map<std::string, std::string>& icons, std::string* icon) {
    Ptr<IAppInfo> info;
    std::string name;
    if (SUCCEEDED(un->get_AppInfo(info.put())) && info) {
        Ptr<IAppDisplayInfo> display;
        HSTRING h = nullptr;
        if (SUCCEEDED(info->get_DisplayInfo(display.put())) && display) {
            if (SUCCEEDED(display->get_DisplayName(&h))) name = TakeText(h, 60);
            auto cached = icons.find(name);
            if (cached == icons.end()) {
                std::string logo = LogoDataUrl(display.p);
                if (logo.empty()) {
                    HSTRING id = nullptr;
                    if (SUCCEEDED(info->get_AppUserModelId(&id)) && id) {
                        UINT32 len = 0;
                        const wchar_t* raw = WindowsGetStringRawBuffer(id, &len);
                        std::wstring aumid(raw, len);
                        WindowsDeleteString(id);
                        logo = ShellIconDataUrl(aumid);
                    }
                }
                cached = icons.emplace(name, std::move(logo)).first;
            }
            *icon = cached->second;
        }
    }
    return name;
}

// Textes de la liaison « ToastGeneric » : le premier est le titre, les suivants le corps.
std::vector<std::string> Texts(UN::IUserNotification* un) {
    std::vector<std::string> out;
    Ptr<INotification> notification;
    if (FAILED(un->get_Notification(notification.put())) || !notification) return out;
    Ptr<INotificationVisual> visual;
    if (FAILED(notification->get_Visual(visual.put())) || !visual) return out;
    HSTRING tpl = nullptr;
    const wchar_t* generic = L"ToastGeneric";
    if (FAILED(WindowsCreateString(generic, (UINT32)wcslen(generic), &tpl))) return out;
    Ptr<INotificationBinding> binding;
    HRESULT hr = visual->GetBinding(tpl, binding.put());
    WindowsDeleteString(tpl);
    if (FAILED(hr) || !binding) return out;
    ABI::Windows::Foundation::Collections::IVectorView<AdaptiveNotificationText*>* raw = nullptr;
    if (FAILED(binding->GetTextElements(&raw)) || !raw) return out;
    auto* texts = reinterpret_cast<IVectorViewAbi<IAdaptiveNotificationText*>*>(raw);
    UINT32 count = 0;
    texts->get_Size(&count);
    for (UINT32 i = 0; i < count && i < 4; i++) {
        Ptr<IAdaptiveNotificationText> text;
        HSTRING h = nullptr;
        if (SUCCEEDED(texts->GetAt(i, text.put())) && text && SUCCEEDED(text->get_Text(&h))) {
            std::string s = TakeText(h);
            if (!s.empty()) out.push_back(std::move(s));
        }
    }
    texts->Release();
    return out;
}

// La page dort quand le dock est caché : PostToPageAwake la réveille (sur le thread UI, pas ici) pour
// qu'elle affiche la pastille. Piège : webview::Wake() appelé depuis ce thread ne fait rien.
void Post(const std::string& json) { bridge::PostToPageAwake(json); }

void PostStatus(const char* status) {
    bridge::PostToPage(std::string("{\"t\":\"appnotifs\",\"status\":\"") + status + "\"}");
}

struct Item {
    UINT32 id = 0;
    std::string app, title, text, icon;
};

// Relève les notifications « toast » en cours. Renvoie false si le relevé a échoué.
bool Poll(IListener* listener, std::vector<UINT32>& ids, std::vector<Item>* fresh, const std::set<UINT32>& seen,
          std::map<std::string, std::string>& icons) {
    Ptr<IInspectable> op;
    if (FAILED(listener->GetNotificationsAsync(1, op.put())) || !op) return false;
    IVectorViewAbi<UN::IUserNotification*>* list = nullptr;
    if (!Await(op.p, &list) || !list) return false;
    UINT32 count = 0;
    list->get_Size(&count);
    for (UINT32 i = 0; i < count; i++) {
        Ptr<UN::IUserNotification> un;
        if (FAILED(list->GetAt(i, un.put())) || !un) continue;
        UINT32 id = 0;
        un->get_Id(&id);
        ids.push_back(id);
        if (!fresh || seen.count(id)) continue;
        Item it;
        it.id = id;
        it.app = AppDetails(un.p, icons, &it.icon);
        auto texts = Texts(un.p);
        if (!texts.empty()) it.title = texts[0];
        for (size_t k = 1; k < texts.size(); k++) it.text += (it.text.empty() ? "" : " ") + texts[k];
        fresh->push_back(std::move(it));
    }
    list->Release();
    return true;
}

void Run(int gen) {
    RoInitialize(RO_INIT_MULTITHREADED);
    Ptr<IInspectable> listenerObj;
    {
        const wchar_t* cls = L"Windows.UI.Notifications.Management.UserNotificationListener";
        HSTRING h = nullptr;
        Ptr<IStatics> statics;
        if (FAILED(WindowsCreateString(cls, (UINT32)wcslen(cls), &h))) return PostStatus("error");
        HRESULT hr = RoGetActivationFactory(h, IID_IStatics, (void**)statics.put());
        WindowsDeleteString(h);
        if (FAILED(hr) || FAILED(statics->get_Current(listenerObj.put())) || !listenerObj) return PostStatus("unsupported");
    }
    Ptr<IListener> listener;
    if (FAILED(listenerObj->QueryInterface(IID_IListener, (void**)listener.put()))) return PostStatus("unsupported");

    int status = 0;
    listener->GetAccessStatus(&status);
    if (status != 1) { // pas encore autorisé : on le demande (réglage Windows « Accès aux notifications »)
        Ptr<IInspectable> op;
        int answer = 0;
        if (SUCCEEDED(listener->RequestAccessAsync(op.put())) && Await(op.p, &answer, 30000)) status = answer;
    }
    if (status != 1) return PostStatus("denied");
    if (gen != g_gen) return;
    PostStatus("allowed");

    GdiPlusSession gdi; // pour encoder les icônes en PNG
    std::set<UINT32> seen;
    std::map<std::string, std::string> icons; // logo (data URL) par nom d'appli
    {
        std::vector<UINT32> ids;
        if (Poll(listener.p, ids, nullptr, seen, icons)) seen.insert(ids.begin(), ids.end()); // l'existant n'est pas rejoué
    }
    while (gen == g_gen) {
        for (int waited = 0; waited < kPollMs && gen == g_gen; waited += 100) Sleep(100);
        if (gen != g_gen) break;
        std::vector<UINT32> ids;
        std::vector<Item> fresh;
        if (!Poll(listener.p, ids, &fresh, seen, icons)) continue;
        seen = std::set<UINT32>(ids.begin(), ids.end()); // oublie celles qui ont disparu
        std::sort(fresh.begin(), fresh.end(), [](const Item& a, const Item& b) { return a.id < b.id; });
        if (fresh.size() > 3) fresh.erase(fresh.begin(), fresh.end() - 3); // rafale : les 3 dernières
        for (const auto& it : fresh) {
            if (it.title.empty() && it.text.empty()) continue;
            Post("{\"t\":\"appnotif\",\"app\":" + util::Json(it.app) + ",\"title\":" + util::Json(it.title) +
                 ",\"text\":" + util::Json(it.text) + ",\"icon\":" + util::Json(it.icon) + "}");
        }
        for (const auto& it : fresh) seen.insert(it.id);
    }
}

} // namespace

void SetEnabled(bool on) {
    if (on == g_running) return;
    g_running = on;
    int gen = ++g_gen; // arrête l'éventuel thread précédent
    if (on) std::thread(Run, gen).detach();
}

} // namespace appnotifs
