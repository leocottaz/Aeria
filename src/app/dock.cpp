#include "app/dock.h"

#include "app/autostart.h"
#include "app/tray.h"
#include "common/util.h"
#include "features/media.h"
#include "features/reminders.h"
#include "web/bridge.h"
#include "web/webview_host.h"

#include <dwmapi.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace dock {
namespace {

// ------------------------------------------------------------------ constantes

constexpr UINT_PTR TIMER_POLL = 1;      // surveillance de la souris (30 ms)
constexpr UINT_PTR TIMER_ANIM = 2;      // animation de la fenêtre (~60 i/s)
constexpr UINT_PTR TIMER_REMINDERS = 3; // échéances des rappels (1 s)
constexpr int HOTKEY_TOGGLE = 1;        // Ctrl+Alt+Espace

// Géométrie en DIP. Le haut de la fenêtre dépasse au-dessus de l'écran (kInset)
// pour que seuls les coins du bas soient arrondis : effet « encoche » qui descend.
// Ces valeurs doivent correspondre à --inset et --bar-h dans ui/css/base.css.
constexpr int kInset = 14;
constexpr int kBarH = 58;
constexpr int kTabW = 150, kTabH = 7;     // languette (mode 1)
constexpr int kStripW = 300, kStripH = 6; // barre de progression (mode 0)
constexpr int kMiniH = 40;                // mini-dock : rappels, paroles (= --mini-h dans ui/css/dock.css)
constexpr int kMusicH = 76;               // carte musique (= --mcard-h dans ui/css/music.css)
constexpr int kFlare = 32;                // marge de chaque côté pour les bords évasés, au maximum (= --flare-pad dans ui/css/base.css)
constexpr int kExtra = 40;               // marge pour le rebond de l'animation

// Mini : dock rangé réduit à une pastille (cloche des rappels sonnés, ligne de paroles en cours).
// Music : carte du morceau (pochette, titre, contrôles) qui descend quand la musique change.
enum class State { Hidden, Progress, Peek, Mini, Music, Bar, Panel };

const char* StateName(State s) {
    switch (s) {
        case State::Hidden: return "hidden";
        case State::Progress: return "progress";
        case State::Peek: return "peek";
        case State::Mini: return "mini";
        case State::Music: return "music";
        case State::Bar: return "bar";
        case State::Panel: return "panel";
    }
    return "hidden";
}

struct Anim {
    bool active = false;
    RECT from{}, to{};
    ULONGLONG start = 0;
    int duration = 0;
    bool bounce = false;
};

// ------------------------------------------------------------------ état

HWND g_hwnd = nullptr;
HMONITOR g_monitor = nullptr;
UINT g_taskbarCreated = 0;
Config g_cfg;
State g_state = State::Hidden;
Anim g_anim;
bool g_pinned = false;
bool g_introDone = false;
ULONGLONG g_progressStart = 0;
ULONGLONG g_outsideSince = 0;
ULONGLONG g_holdUntil = 0;
bool g_releaseHoldOnEnter = false; // une ouverture sans la souris tient jusqu'au premier survol
bool g_waitLeave = false;          // après un repli manuel, pas de réapparition avant que la souris parte
int g_miniW = 0;                   // largeur voulue par la page pour le mini-dock (DIP), 0 = rien à montrer
int g_musicW = 380;                // largeur de la carte musique (DIP), selon le titre

// ------------------------------------------------------------------ géométrie

// Écran où vit le dock. Rangé, il suit la souris (FollowMouse) ; ouvert, il reste en place.
HMONITOR Monitor() {
    if (!g_monitor) g_monitor = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    return g_monitor;
}

HMONITOR MouseMonitor() {
    POINT p;
    GetCursorPos(&p);
    return MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST);
}

RECT MonitorRect() {
    MONITORINFO mi{sizeof mi};
    if (!GetMonitorInfoW(Monitor(), &mi)) { // écran débranché : retour à l'écran principal
        g_monitor = nullptr;
        GetMonitorInfoW(Monitor(), &mi);
    }
    return mi.rcMonitor;
}

double Scale() {
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(Monitor(), MDT_EFFECTIVE_DPI, &dx, &dy);
    return dx / 96.0;
}

int Px(int dip) { return (int)std::lround(dip * Scale()); }

RECT TargetRect(State s) {
    RECT m = MonitorRect();
    int cx = (m.left + m.right) / 2;
    int w = g_cfg.width, h = kInset;
    switch (s) {
        case State::Hidden: w = g_cfg.mode == 1 ? kTabW : kStripW; h = kInset; break;
        case State::Progress: w = kStripW; h = kInset + kStripH; break;
        case State::Peek: w = kTabW; h = kInset + kTabH; break;
        case State::Mini: w = g_miniW; h = kInset + kMiniH; break;
        case State::Music: w = g_musicW; h = kInset + kMusicH; break;
        case State::Bar: h = kInset + kBarH; break;
        case State::Panel: h = kInset + kBarH + g_cfg.panelH; break;
    }
    int top = m.top - Px(kInset);
    if (s == State::Hidden) top -= Px(24); // complètement hors de l'écran (et de son ombre)
    int pw = Px(w + 2 * kFlare);
    return {cx - pw / 2, top, cx - pw / 2 + pw, top + Px(h)};
}

// La vue web garde toujours la taille du panneau complet ; la fenêtre la « découpe ».
// Quand la fenêtre est plus étroite (languette, progression), on centre la vue.
void UpdateWebBounds() {
    RECT wr;
    GetWindowRect(g_hwnd, &wr);
    int curW = wr.right - wr.left;
    int fullW = Px(g_cfg.width + 2 * kFlare);
    int fullH = Px(kInset + kBarH + g_cfg.panelH + kExtra);
    webview::SetBounds({(curW - fullW) / 2, 0, (curW - fullW) / 2 + fullW, fullH});
}

void PlaceWindow(const RECT& r) {
    SetWindowPos(g_hwnd, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    UpdateWebBounds();
}

// ------------------------------------------------------------------ animation

double EaseOutBack(double t) {
    const double c1 = 1.15, c3 = c1 + 1;
    return 1 + c3 * std::pow(t - 1, 3) + c1 * std::pow(t - 1, 2);
}

double EaseInOutCubic(double t) { return t < 0.5 ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3) / 2; }

void AnimateTo(const RECT& to, int duration, bool bounce) {
    GetWindowRect(g_hwnd, &g_anim.from);
    g_anim.to = to;
    g_anim.start = GetTickCount64();
    g_anim.duration = duration;
    g_anim.bounce = bounce;
    g_anim.active = true;
    SetTimer(g_hwnd, TIMER_ANIM, 8, nullptr);
}

// Dock caché, la page est normalement suspendue. Mais pendant une lecture avec le mini-dock
// activé, elle doit rester éveillée : c'est elle qui charge les paroles et demande le mini-dock.
bool KeepPageAwake() { return g_cfg.miniLyrics && media::IsPlaying(); }

void AnimStep() {
    if (!g_anim.active) return;
    double t = std::min(1.0, (GetTickCount64() - g_anim.start) / (double)std::max(1, g_anim.duration));
    double e = g_anim.bounce ? EaseOutBack(t) : EaseInOutCubic(t);
    auto lerp = [e](LONG a, LONG b) { return (LONG)std::lround(a + (b - a) * e); };
    // On interpole largeur/hauteur autour du centre horizontal pour garder le dock centré.
    const RECT &f = g_anim.from, &to = g_anim.to;
    LONG cx = lerp((f.left + f.right) / 2, (to.left + to.right) / 2);
    LONG w = std::max<LONG>(1, lerp(f.right - f.left, to.right - to.left));
    LONG top = lerp(f.top, to.top);
    LONG h = std::max<LONG>(1, lerp(f.bottom - f.top, to.bottom - to.top));
    PlaceWindow({cx - w / 2, top, cx - w / 2 + w, top + h});
    if (t >= 1.0) {
        g_anim.active = false;
        KillTimer(g_hwnd, TIMER_ANIM);
        PlaceWindow(g_anim.to);
        if (g_state == State::Hidden) {
            ShowWindow(g_hwnd, SW_HIDE);
            webview::SetVisible(false, !KeepPageAwake());
        }
    }
}

// ------------------------------------------------------------------ états

bool ForegroundIsFullscreen() {
    HWND fg = GetForegroundWindow();
    if (!fg || fg == g_hwnd) return false;
    wchar_t cls[64]{};
    GetClassNameW(fg, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd")) return false;
    RECT r, m = MonitorRect();
    GetWindowRect(fg, &r);
    return r.left <= m.left && r.top <= m.top && r.right >= m.right && r.bottom >= m.bottom;
}

// État de repos (jamais devant une appli plein écran) : mini-dock si la page a quelque chose
// à montrer (rappels qui ont sonné, paroles), sinon languette en mode 1, sinon masqué.
State RestState() {
    if (ForegroundIsFullscreen()) return State::Hidden;
    if (g_miniW > 0) return State::Mini;
    return g_cfg.mode == 1 ? State::Peek : State::Hidden;
}

bool IsResting(State s) { return s == State::Hidden || s == State::Peek || s == State::Mini; }

void SetState(State s, bool force = false) {
    if (s == g_state && !force) return;
    State prev = g_state;
    g_state = s;
    g_outsideSince = 0;
    if (s == State::Progress) g_progressStart = GetTickCount64();
    if (prev == State::Hidden && s != State::Hidden) {
        g_monitor = MouseMonitor(); // raccourci, icône, rappel : on sort là où est la souris
        PlaceWindow(TargetRect(State::Hidden));
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
        webview::SetVisible(true); // sort la vue de veille
        media::Command("refresh"); // la page était suspendue : on lui renvoie l'état du lecteur
    }
    bool growing = s == State::Panel || (s == State::Bar && prev != State::Panel) ||
                   ((s == State::Mini || s == State::Music) && IsResting(prev));
    int duration = s == State::Panel ? 420 : s == State::Bar ? 380 : s == State::Mini || s == State::Music ? 360 : 260;
    if (prev == State::Panel && s == State::Bar) duration = 320;
    AnimateTo(TargetRect(s), duration, growing);
    NotifyState();
}

void Activate() {
    ShowWindow(g_hwnd, SW_SHOW);
    SetForegroundWindow(g_hwnd);
    webview::Focus();
}

void HoldUntilHovered(ULONGLONG ms) {
    g_holdUntil = GetTickCount64() + ms;
    g_releaseHoldOnEnter = true;
}

// Rangé (masqué, languette, mini-dock, progression), le dock passe sur l'écran de la souris.
void FollowMouse(POINT p) {
    if (g_state == State::Bar || g_state == State::Panel || g_state == State::Music || g_anim.active) return;
    HMONITOR mon = MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST);
    if (mon == Monitor()) return;
    g_monitor = mon;
    if (g_state == State::Hidden) return; // la fenêtre est cachée : rien à déplacer
    PlaceWindow(TargetRect(State::Hidden)); // la languette redescend sur le nouvel écran
    SetState(RestState(), true);
}

bool g_wasFocused = false;
bool g_marginClick = false; // clic dans la marge transparente : le focus de la fenêtre ne garde pas le dock ouvert

// Surveille la souris : bord haut de l'écran, survol, repli automatique.
void Poll() {
    POINT p;
    GetCursorPos(&p);
    FollowMouse(p);
    RECT m = MonitorRect();
    int cx = (m.left + m.right) / 2, half = Px(g_cfg.width) / 2;
    bool fullscreen = ForegroundIsFullscreen();
    bool mouseDown = GetAsyncKeyState(VK_LBUTTON) & 0x8000;
    // Un vrai bord : pas d'autre écran juste au-dessus (sinon la souris ne fait que passer).
    bool atTopEdge = p.y <= m.top + 1 && p.y >= m.top - 2 && p.x >= cx - half && p.x <= cx + half &&
                     !MonitorFromPoint({p.x, m.top - 1}, MONITOR_DEFAULTTONULL);
    ULONGLONG now = GetTickCount64();
    // En mode languette, survoler ce qui dépasse (languette ou mini-dock) suffit. Devant une appli
    // plein écran rien ne dépasse : seul le bord haut compte, et seulement si le réglage l'autorise.
    RECT tab = TargetRect(g_state == State::Mini ? State::Mini : State::Peek);
    InflateRect(&tab, Px(6), Px(4));
    bool overTab = g_cfg.mode == 1 && !fullscreen && PtInRect(&tab, p);
    bool wantsReveal = (atTopEdge || overTab) && !mouseDown && (!fullscreen || g_cfg.fullscreenReveal);
    if (g_waitLeave) {
        if (!wantsReveal) g_waitLeave = false;
        wantsReveal = false;
    }
    if (g_state == State::Hidden && KeepPageAwake()) webview::Wake(); // une lecture vient de démarrer

    switch (g_state) {
        case State::Hidden:
            if (RestState() != State::Hidden) SetState(RestState());
            else if (wantsReveal) SetState(g_cfg.mode == 1 || g_cfg.revealDelay <= 0 ? State::Bar : State::Progress);
            break;
        case State::Peek:
        case State::Mini:
            if (RestState() != g_state) SetState(RestState()); // plein écran, paroles qui arrivent ou s'arrêtent
            else if (wantsReveal && g_cfg.mode == 1) SetState(State::Bar);
            else if (wantsReveal) SetState(g_cfg.revealDelay <= 0 ? State::Bar : State::Progress);
            break;
        case State::Progress:
            if (!atTopEdge) SetState(RestState());
            else if (now - g_progressStart >= (ULONGLONG)g_cfg.revealDelay) SetState(State::Bar);
            break;
        case State::Music:
        case State::Bar:
        case State::Panel: {
            RECT wr;
            GetWindowRect(g_hwnd, &wr);
            // Les marges transparentes de chaque côté (évasements) ne comptent pas comme le dock : un clic
            // dedans donne le focus à la fenêtre, mais il doit se ranger comme un clic à côté.
            RECT full = wr;
            wr.left += Px(kFlare);
            wr.right -= Px(kFlare);
            bool inInner = PtInRect(&wr, p);
            bool focused = GetForegroundWindow() == g_hwnd;
            bool inMargin = PtInRect(&full, p) && !inInner;
            if (inMargin && (mouseDown || (focused && !g_wasFocused))) g_marginClick = true; // clic, ou focus pris par un clic
            g_wasFocused = focused;
            if (inInner) g_marginClick = false;
            InflateRect(&wr, Px(8), Px(8));
            // Carte musique : la souris en haut de l'écran, à côté de la carte, ouvre la barre complète.
            if (g_state == State::Music && wantsReveal && !PtInRect(&wr, p)) {
                SetState(State::Bar);
                break;
            }
            bool inside = PtInRect(&wr, p) || atTopEdge;
            if (inside && g_releaseHoldOnEnter) {
                g_releaseHoldOnEnter = false;
                g_holdUntil = 0;
            }
            // Une fenêtre du dock au premier plan (lui, ou la fenêtre « Ouvrir » qu'il possède) le garde ouvert :
            // en se rangeant, il cacherait aussi les fenêtres qu'il possède.
            // (Pas pour la carte musique : un clic sur lecture/pause l'active, elle ne doit pas rester pour autant.)
            HWND fg = GetForegroundWindow();
            bool hold = g_pinned || now < g_holdUntil ||
                        (g_state != State::Music && !g_marginClick && (fg == g_hwnd || (fg && GetWindow(fg, GW_OWNER) == g_hwnd)));
            if (inside || hold) {
                g_outsideSince = 0;
            } else {
                if (!g_outsideSince) g_outsideSince = now;
                if (now - g_outsideSince >= (ULONGLONG)g_cfg.hideDelay) SetState(RestState());
            }
            break;
        }
    }
}

// ------------------------------------------------------------------ apparence

void ApplyBackdrop() {
    MARGINS margins{-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(g_hwnd, &margins);
    BOOL dark = g_cfg.dark;
    DwmSetWindowAttribute(g_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
    // Plus d'acrylique : il remplit le rectangle de la fenêtre et ignore toute découpe. La page dessine
    // le verre et ses bords évasés (.shell dans ui/css/base.css) sur une fenêtre transparente.
    int corner = 1; // DWMWCP_DONOTROUND
    DwmSetWindowAttribute(g_hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof corner);
    COLORREF border = 0xFFFFFFFE; // DWMWA_COLOR_NONE
    DwmSetWindowAttribute(g_hwnd, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof border);
    int backdrop = 1; // DWMSBT_NONE
    DwmSetWindowAttribute(g_hwnd, 38 /* DWMWA_SYSTEMBACKDROP_TYPE */, &backdrop, sizeof backdrop);
}

// ------------------------------------------------------------------ entrées

void ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"Ouvrir le dock\tCtrl+Alt+Espace");
    AppendMenuW(menu, MF_STRING | (g_pinned ? MF_CHECKED : 0), 2, L"Garder affiché");
    AppendMenuW(menu, MF_STRING, 3, L"Réglages");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 4, L"Quitter");
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(g_hwnd);
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, p.x, p.y, 0, g_hwnd, nullptr);
    DestroyMenu(menu);
    switch (cmd) {
        case 1: OpenPanel(); bridge::PostToPage("{\"t\":\"show\",\"tab\":\"claude\"}"); break;
        case 2: SetPinned(!g_pinned); if (g_pinned && g_state != State::Panel) SetState(State::Bar); break;
        case 3: OpenPanel(); bridge::PostToPage("{\"t\":\"show\",\"tab\":\"settings\"}"); break;
        case 4: DestroyWindow(g_hwnd); break;
        default: break;
    }
}

void ToggleFromHotkey() {
    if (g_state == State::Panel && GetForegroundWindow() == g_hwnd) {
        HideByUser();
    } else {
        OpenPanel();
        bridge::PostToPage("{\"t\":\"hotkey\"}");
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreated && g_taskbarCreated) {
        tray::Readd();
        return 0;
    }
    switch (msg) {
        case bridge::WM_APP_WEB:
            bridge::Deliver(lp);
            return 0;
        case webview::WM_APP_WEBVIEW_RESTART:
            webview::Restart();
            return 0;
        case tray::WM_APP_TRAY:
            if (LOWORD(lp) == WM_LBUTTONUP) OpenPanel();
            else if (LOWORD(lp) == WM_RBUTTONUP) ShowTrayMenu();
            return 0;
        case WM_APP_SUMMON:
            ToggleFromHotkey();
            return 0;
        case WM_HOTKEY:
            if (wp == HOTKEY_TOGGLE) ToggleFromHotkey();
            return 0;
        case WM_TIMER:
            if (wp == TIMER_ANIM) AnimStep();
            else if (wp == TIMER_POLL) Poll();
            else if (wp == TIMER_REMINDERS) reminders::Check();
            return 0;
        case WM_SIZE:
            UpdateWebBounds();
            return 0;
        case WM_DPICHANGED:
        case WM_DISPLAYCHANGE:
            PlaceWindow(TargetRect(g_state));
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: { // noir = transparent sur l'acrylique
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            FillRect(dc, &ps.rcPaint, (HBRUSH)GetStockObject(BLACK_BRUSH));
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MOUSEACTIVATE:
            return MA_ACTIVATE;
        case WM_CLOSE: // Alt+F4 replie le dock ; on quitte par l'icône ou les réglages
            HideByUser();
            return 0;
        case WM_DESTROY:
            UnregisterHotKey(hwnd, HOTKEY_TOGGLE);
            tray::Remove();
            webview::Close();
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

} // namespace

// ------------------------------------------------------------------ API publique

HWND Create(HINSTANCE instance) {
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = kClassName;
    wc.hIcon = tray::Icon();
    wc.hIconSm = tray::Icon();
    RegisterClassExW(&wc);

    RECT r = TargetRect(State::Hidden);
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kClassName, L"Aeria", WS_POPUP, r.left, r.top,
                             r.right - r.left, r.bottom - r.top, nullptr, nullptr, instance, nullptr);
    ApplyBackdrop();

    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    tray::Init(g_hwnd);
    RegisterHotKey(g_hwnd, HOTKEY_TOGGLE, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_SPACE);
    SetTimer(g_hwnd, TIMER_POLL, 30, nullptr);
    SetTimer(g_hwnd, TIMER_REMINDERS, 1000, nullptr);
    return g_hwnd;
}

const Config& Cfg() { return g_cfg; }

void ApplyConfig(const std::string& payload) {
    Config c = g_cfg;
    bool autostartOn = autostart::Enabled(), wantAutostart = autostartOn;
    for (const auto& line : util::Split(payload, '\n')) {
        auto kv = util::Split(line, '=', 2);
        if (kv.size() != 2) continue;
        const std::string &k = kv[0], &v = kv[1];
        int n = std::atoi(v.c_str());
        if (k == "mode") c.mode = n == 1 ? 1 : 0;
        else if (k == "revealDelay") c.revealDelay = std::clamp(n, 0, 5000);
        else if (k == "hideDelay") c.hideDelay = std::clamp(n, 150, 10000);
        else if (k == "width") c.width = std::clamp(n, 480, 1200);
        else if (k == "panelH") c.panelH = std::clamp(n, 320, 900);
        else if (k == "theme") c.dark = v != "light";
        else if (k == "sound") c.sound = v == "1";
        else if (k == "autostart") wantAutostart = v == "1";
        else if (k == "miniLyrics") c.miniLyrics = v == "1";
        else if (k == "fullscreenReveal") c.fullscreenReveal = v == "1";
    }
    bool themeChanged = c.dark != g_cfg.dark;
    bool geometryChanged = c.mode != g_cfg.mode || c.width != g_cfg.width || c.panelH != g_cfg.panelH;
    g_cfg = c;
    if (wantAutostart != autostartOn) autostart::Set(wantAutostart);
    if (themeChanged) ApplyBackdrop();
    if (geometryChanged) {
        if (IsResting(g_state)) SetState(RestState(), true);
        else AnimateTo(TargetRect(g_state), 300, false);
    }
}

void OnPageReady() {
    NotifyState();
    if (!g_introDone) { // petite démo à l'ouverture : le dock descend puis remonte
        g_introDone = true;
        SetState(State::Bar);
        HoldUntilHovered(2600);
    }
}

void NotifyState() {
    bridge::PostToPage(std::string("{\"t\":\"state\",\"s\":\"") + StateName(g_state) + "\",\"delay\":" +
                       std::to_string(g_cfg.revealDelay) + ",\"pinned\":" + (g_pinned ? "true" : "false") + "}");
}

void OpenPanel() {
    SetState(State::Panel);
    HoldUntilHovered(8000);
    Activate();
}

void ShowBar() { SetState(State::Bar); }

void HideByUser() {
    g_holdUntil = 0;
    g_releaseHoldOnEnter = false;
    g_waitLeave = true;
    SetState(RestState());
}

void SetPinned(bool pinned) {
    g_pinned = pinned;
    NotifyState();
}

void HoldFor(int ms) { g_holdUntil = GetTickCount64() + std::max(0, ms); }

void SetMini(int widthDip) {
    int w = widthDip > 0 ? std::clamp(widthDip, 40, 640) : 0; // 40 : la cloche seule
    if (w == g_miniW) return;
    bool resized = w && g_miniW;
    g_miniW = w;
    if (g_state == State::Mini && resized) AnimateTo(TargetRect(State::Mini), 320, false); // nouvelle ligne
    else if (IsResting(g_state) && RestState() != g_state) SetState(RestState());
}

void RevealBriefly(int ms) {
    if (ForegroundIsFullscreen()) return; // pas par-dessus un jeu ou une vidéo plein écran
    if (g_state != State::Bar && g_state != State::Panel) SetState(State::Bar);
    HoldUntilHovered(ms);
}

void RevealMusic(int ms, int widthDip) {
    if (ForegroundIsFullscreen()) return;
    int w = widthDip > 0 ? std::clamp(widthDip, 240, 480) : 380;
    bool resized = w != g_musicW;
    g_musicW = w;
    if (g_state == State::Music) {
        if (resized) AnimateTo(TargetRect(State::Music), 300, false); // morceau suivant pendant que la carte est là
    } else if (g_state != State::Bar && g_state != State::Panel) {
        SetState(State::Music); // ouvert, la barre montre déjà le morceau
    }
    HoldUntilHovered(ms);
}

} // namespace dock
