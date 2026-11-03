#include "app/drop_edge.h"

#include <ole2.h>
#include <shlobj.h>

namespace drop_edge {
namespace {

constexpr wchar_t kClassName[] = L"AeriaDropEdge";

HWND g_hwnd = nullptr;
RECT g_rect{};
bool g_solid = false;

// Cible de dépôt qui n'accepte rien : elle ne sert qu'à savoir que des fichiers arrivent.
class Target : public IDropTarget {
public:
    Target(HWND owner, UINT message) : m_owner(owner), m_message(message) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (id == IID_IUnknown || id == IID_IDropTarget) {
            *out = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_refs); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG n = InterlockedDecrement(&m_refs);
        if (!n) delete this;
        return n;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
        *effect = DROPEFFECT_NONE; // on ne reçoit rien ici : le panneau, qui s'ouvre dessous, reçoit le dépôt
        if (HasFiles(data)) PostMessageW(m_owner, m_message, 0, 0);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override {
        *effect = DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject*, DWORD, POINTL, DWORD* effect) override {
        *effect = DROPEFFECT_NONE;
        return S_OK;
    }

private:
    // Fichiers de l'Explorateur (CF_HDROP) ou d'un navigateur (fichiers virtuels) : pas du texte ni un lien.
    static bool HasFiles(IDataObject* data) {
        FORMATETC drop{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        FORMATETC virt{(CLIPFORMAT)RegisterClipboardFormatW(L"FileGroupDescriptorW"), nullptr, DVASPECT_CONTENT, -1,
                       TYMED_HGLOBAL};
        return data && (data->QueryGetData(&drop) == S_OK || data->QueryGetData(&virt) == S_OK);
    }

    HWND m_owner;
    UINT m_message;
    LONG m_refs = 1;
};

} // namespace

void Create(HINSTANCE instance, HWND owner, UINT message) {
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = instance;
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
    // Presque transparente (alpha 1) mais pas totalement : un cadre à alpha 0 ne reçoit pas le glisser.
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
                             kClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
    if (!g_hwnd) return;
    SetLayeredWindowAttributes(g_hwnd, 0, 1, LWA_ALPHA);
    OleInitialize(nullptr); // déjà en STA (main.cpp) : renvoie S_FALSE, sans conséquence
    RegisterDragDrop(g_hwnd, new Target(owner, message));
}

void Place(const RECT& r, bool solid) {
    if (!g_hwnd) return;
    if (solid != g_solid) {
        g_solid = solid;
        LONG_PTR ex = GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE);
        SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, solid ? ex & ~WS_EX_TRANSPARENT : ex | WS_EX_TRANSPARENT);
    }
    if (EqualRect(&r, &g_rect)) return;
    g_rect = r;
    if (IsRectEmpty(&r)) {
        ShowWindow(g_hwnd, SW_HIDE);
        return;
    }
    SetWindowPos(g_hwnd, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

} // namespace drop_edge
