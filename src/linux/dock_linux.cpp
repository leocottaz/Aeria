// Le dock sous Linux (GTK3, X11 ou XWayland) : mêmes états que app/dock.cpp (masqué, progression,
// languette, mini, musique, barre, panneau), mêmes animations et même protocole vers la page.
// Différences : pas d'évasements, pas de détection du plein écran, et le raccourci global
// (Ctrl+Alt+Espace) ne marche que sous X11 — Wayland ne permet pas de capter une touche globale.
// La souris : voir « souris sous XWayland » plus bas.
#include "app/dock.h"

#include "app/autostart.h"
#include "app/tray.h"
#include "common/util.h"
#include "features/media.h"
#include "features/reminders.h"
#include "features/voice.h"
#include "linux/host.h"
#include "web/bridge.h"

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace dock {
namespace {

// ------------------------------------------------------------------ constantes

// Géométrie en DIP (= pixels logiques GTK). Valeurs liées à ui/css/*.css, comme sous Windows.
// Pas de marge cachée au-dessus de l'écran (--inset dans base.css) : KWin ramène la fenêtre à y = 0.
constexpr int kInset = 0;
constexpr int kBarH = 58;
constexpr int kTabW = 150, kTabH = 7;
constexpr int kStripW = 300, kStripH = 6;
constexpr int kMiniH = 40;
constexpr int kMusicH = 76;
constexpr int kListenW = 190; // encoche « j'écoute » ; hauteur = kMiniH
constexpr int kExtra = 40;

enum class State { Hidden, Progress, Peek, Mini, Music, Listen, Bar, Panel };

const char* StateName(State s) {
    switch (s) {
        case State::Hidden: return "hidden";
        case State::Progress: return "progress";
        case State::Peek: return "peek";
        case State::Mini: return "mini";
        case State::Music: return "music";
        case State::Listen: return "listen";
        case State::Bar: return "bar";
        case State::Panel: return "panel";
    }
    return "hidden";
}

struct Rect {
    int l = 0, t = 0, r = 0, b = 0;
    int w() const { return r - l; }
    int h() const { return b - t; }
    bool Contains(int x, int y) const { return x >= l && x < r && y >= t && y < b; }
    void Inflate(int dx, int dy) { l -= dx, r += dx, t -= dy, b += dy; }
};

struct Anim {
    bool active = false;
    Rect from, to;
    gint64 start = 0;
    int duration = 0;
    bool bounce = false;
};

// ------------------------------------------------------------------ état

GtkWidget* g_window = nullptr;
GtkWidget* g_layout = nullptr;
GtkWidget* g_view = nullptr;
GdkMonitor* g_monitor = nullptr;
Config g_cfg;
State g_state = State::Hidden;
Anim g_anim;
Rect g_rect; // dernière position demandée à la fenêtre
bool g_pinned = false;
bool g_introDone = false;
gint64 g_progressStart = 0;
gint64 g_outsideSince = 0;
gint64 g_holdUntil = 0;
bool g_releaseHoldOnEnter = false;
bool g_waitLeave = false;
int g_miniW = 0;
int g_musicW = 380;
bool g_listening = false; // la commande vocale écoute : l'encoche reste là
guint g_animSource = 0;
KeyCode g_hotkeyCode = 0;
bool g_xwayland = false; // session Wayland : on tourne dans XWayland (voir « souris sous XWayland »)
bool g_overDock = false;
bool g_onEdge = false;
std::vector<GtkWidget*> g_edges;

gint64 NowMs() { return g_get_monotonic_time() / 1000; }

// ------------------------------------------------------------------ géométrie

GdkDisplay* GdkDisp() { return gdk_display_get_default(); }

Rect GeometryOf(GdkMonitor* m) {
    GdkRectangle g;
    gdk_monitor_get_geometry(m, &g);
    return {g.x, g.y, g.x + g.width, g.y + g.height};
}

GdkMonitor* PrimaryMonitor() {
    GdkMonitor* m = gdk_display_get_primary_monitor(GdkDisp());
    return m ? m : gdk_display_get_monitor(GdkDisp(), 0);
}

GdkMonitor* Monitor() {
    if (!g_monitor) g_monitor = PrimaryMonitor();
    return g_monitor;
}

// Pointeur global (X11) : position et bouton gauche enfoncé.
void Pointer(int& x, int& y, bool& leftDown) {
    GdkSeat* seat = gdk_display_get_default_seat(GdkDisp());
    GdkDevice* dev = gdk_seat_get_pointer(seat);
    GdkModifierType mask{};
    gdk_window_get_device_position(gdk_get_default_root_window(), dev, &x, &y, &mask);
    leftDown = mask & GDK_BUTTON1_MASK;
}

bool AnyMonitorContains(int x, int y) {
    for (int i = 0; i < gdk_display_get_n_monitors(GdkDisp()); ++i)
        if (GeometryOf(gdk_display_get_monitor(GdkDisp(), i)).Contains(x, y)) return true;
    return false;
}

GdkMonitor* MonitorAt(int x, int y) { return gdk_display_get_monitor_at_point(GdkDisp(), x, y); }

// ------------------------------------------------------------------ souris sous XWayland

// Sous Wayland (Plasma, GNOME), une appli XWayland ne voit la souris que lorsqu'elle survole une de
// ses fenêtres : ailleurs, la position lue reste figée à la dernière connue. Une bande invisible de
// 1 px en haut de chaque écran signale donc l'arrivée au bord, et les entrées/sorties disent si la
// position est encore valable. Sous X11, le pointeur global suffit.
bool PointerKnown() { return !g_xwayland || g_overDock || g_onEdge; }

gboolean OnCrossing(GtkWidget*, GdkEventCrossing* e, gpointer flag) {
    if (e->detail != GDK_NOTIFY_INFERIOR) *static_cast<bool*>(flag) = e->type == GDK_ENTER_NOTIFY;
    return FALSE;
}

void WatchCrossing(GtkWidget* w, bool* flag) {
    gtk_widget_add_events(w, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
    g_signal_connect(w, "enter-notify-event", G_CALLBACK(OnCrossing), flag);
    g_signal_connect(w, "leave-notify-event", G_CALLBACK(OnCrossing), flag);
}

gboolean DrawNothing(GtkWidget*, cairo_t* cr, gpointer) {
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    return TRUE;
}

// Une bande par écran sans voisin au-dessus, de la largeur du dock.
void RebuildEdges() {
    if (!g_xwayland) return;
    for (GtkWidget* e : g_edges) gtk_widget_destroy(e);
    g_edges.clear();
    g_onEdge = false;
    for (int i = 0; i < gdk_display_get_n_monitors(GdkDisp()); ++i) {
        Rect m = GeometryOf(gdk_display_get_monitor(GdkDisp(), i));
        int cx = (m.l + m.r) / 2;
        if (AnyMonitorContains(cx, m.t - 1)) continue;
        GtkWidget* edge = gtk_window_new(GTK_WINDOW_POPUP); // hors gestionnaire de fenêtres : ni focus, ni placement imposé
        gtk_widget_set_app_paintable(edge, TRUE);
        if (GdkVisual* rgba = gdk_screen_get_rgba_visual(gtk_widget_get_screen(edge))) gtk_widget_set_visual(edge, rgba);
        g_signal_connect(edge, "draw", G_CALLBACK(DrawNothing), nullptr);
        WatchCrossing(edge, &g_onEdge);
        gtk_window_set_default_size(GTK_WINDOW(edge), g_cfg.width, 1);
        gtk_window_move(GTK_WINDOW(edge), cx - g_cfg.width / 2, m.t);
        gtk_widget_show(edge);
        g_edges.push_back(edge);
    }
}

Rect TargetRect(State s) {
    Rect m = GeometryOf(Monitor());
    int cx = (m.l + m.r) / 2;
    int w = g_cfg.width, h = kInset;
    switch (s) {
        case State::Hidden: w = g_cfg.mode == 1 ? kTabW : kStripW; h = kInset; break;
        case State::Progress: w = kStripW; h = kInset + kStripH; break;
        case State::Peek: w = kTabW; h = kInset + kTabH; break;
        case State::Mini: w = g_miniW; h = kInset + kMiniH; break;
        case State::Music: w = g_musicW; h = kInset + kMusicH; break;
        case State::Listen: w = kListenW; h = kInset + kMiniH; break;
        case State::Bar: h = kInset + kBarH; break;
        case State::Panel: h = kInset + kBarH + g_cfg.panelH; break;
    }
    return {cx - w / 2, m.t, cx - w / 2 + w, m.t + h};
}

// La vue web garde la taille du panneau complet ; la fenêtre la découpe. Elle est centrée.
void UpdateWebBounds() {
    int fullW = g_cfg.width, fullH = kInset + kBarH + g_cfg.panelH + kExtra;
    gtk_widget_set_size_request(g_view, fullW, fullH);
    gtk_layout_move(GTK_LAYOUT(g_layout), g_view, (g_rect.w() - fullW) / 2, 0);
}

// Coins bas arrondis découpés dans la fenêtre elle-même : sous KDE, la transparence de la vue ne
// suffit pas, les coins restaient carrés. Même rayon que le verre (--r, .ambient dans dock.css).
void ApplyShape() {
    GdkWindow* win = gtk_widget_get_window(g_window);
    if (!win) return;
    int w = std::max(1, g_rect.w()), h = std::max(1, g_rect.h());
    int r = std::min({g_state == State::Progress || g_state == State::Peek ? 4 : 18, h, w / 2});
    static int lastW = 0, lastH = 0, lastR = 0;
    if (w == lastW && h == lastH && r == lastR) return;
    lastW = w, lastH = h, lastR = r;
    cairo_rectangle_int_t body{0, 0, w, h - r};
    cairo_region_t* region = cairo_region_create_rectangle(&body);
    for (int y = 0; y < r; ++y) {
        double dy = y + 0.5;
        int inset = r - (int)std::lround(std::sqrt(r * r - dy * dy));
        cairo_rectangle_int_t row{inset, h - r + y, w - 2 * inset, 1};
        cairo_region_union_rectangle(region, &row);
    }
    gdk_window_shape_combine_region(win, region, 0, 0);
    cairo_region_destroy(region);
}

void PlaceWindow(const Rect& r) {
    g_rect = r;
    gtk_window_move(GTK_WINDOW(g_window), r.l, r.t);
    gtk_window_resize(GTK_WINDOW(g_window), std::max(1, r.w()), std::max(1, r.h()));
    UpdateWebBounds();
    ApplyShape();
}

// ------------------------------------------------------------------ molette

// La molette est prise dès GDK et remise à la page, qui déplace la zone sous la souris : sous KDE,
// elle n'arrivait jamais au défilement de WebKit. AERIA_DEBUG=1 trace ce qui arrive ici.
bool g_debug = false;
bool g_sawSmooth = false;

bool ForwardScroll(GdkEvent* e) {
    GdkWindow* dock = gtk_widget_get_window(g_window);
    if (!dock || gdk_window_get_toplevel(e->scroll.window) != dock) return false; // boîte de dialogue, menu…
    double dx = 0, dy = 0;
    if (e->scroll.direction == GDK_SCROLL_SMOOTH) {
        gdk_event_get_scroll_deltas(e, &dx, &dy);
        g_sawSmooth = true;
    } else if (e->scroll.direction == GDK_SCROLL_UP || e->scroll.direction == GDK_SCROLL_DOWN) {
        if (g_sawSmooth && gdk_event_get_pointer_emulated(e)) return true; // doublon du défilement continu
        dy = e->scroll.direction == GDK_SCROLL_UP ? -1 : 1;
    } else {
        return false;
    }
    int ox = 0, oy = 0;
    gdk_window_get_origin(dock, &ox, &oy);
    int viewLeft = (g_rect.w() - g_cfg.width) / 2;
    int x = (int)e->scroll.x_root - ox - viewLeft, y = (int)e->scroll.y_root - oy;
    static int tracedX = -1, tracedY = -1;
    if (g_debug && (x != tracedX || y != tracedY)) { // une trace par position, pas une par cran
        tracedX = x, tracedY = y;
        int realW = 0, realH = 0;
        gtk_window_get_size(GTK_WINDOW(g_window), &realW, &realH);
        GdkRectangle screen, work;
        gdk_monitor_get_geometry(Monitor(), &screen);
        gdk_monitor_get_workarea(Monitor(), &work);
        g_printerr("[molette] sens=%d dy=%.3f page=%d,%d fenetre demandee=%dx%d a %d,%d reelle=%dx%d a %d,%d echelle=%d ecran=%dx%d utile=%dx%d\n",
                   (int)e->scroll.direction, dy, x, y, g_rect.w(), g_rect.h(), g_rect.l, g_rect.t, realW, realH, ox, oy,
                   gdk_window_get_scale_factor(dock), screen.width, screen.height, work.width, work.height);
    }
    char delta[G_ASCII_DTOSTR_BUF_SIZE]; // toujours un point décimal, quelle que soit la langue du système
    g_ascii_formatd(delta, sizeof delta, "%.3f", dy);
    bridge::PostToPage("{\"t\":\"wheel\",\"x\":" + std::to_string(x) + ",\"y\":" + std::to_string(y) + ",\"dy\":" + delta +
                       ",\"debug\":" + (g_debug ? "true" : "false") + "}");
    return true;
}

void OnGdkEvent(GdkEvent* e, gpointer) {
    if (e->type == GDK_SCROLL && ForwardScroll(e)) return;
    gtk_main_do_event(e);
}

// ------------------------------------------------------------------ animation

double EaseOutBack(double t) {
    const double c1 = 1.15, c3 = c1 + 1;
    return 1 + c3 * std::pow(t - 1, 3) + c1 * std::pow(t - 1, 2);
}

double EaseInOutCubic(double t) { return t < 0.5 ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3) / 2; }

gboolean AnimStep(gpointer);

void AnimateTo(const Rect& to, int duration, bool bounce) {
    g_anim.from = g_rect;
    g_anim.to = to;
    g_anim.start = NowMs();
    g_anim.duration = duration;
    g_anim.bounce = bounce;
    g_anim.active = true;
    if (!g_animSource) g_animSource = g_timeout_add(8, AnimStep, nullptr);
}

gboolean AnimStep(gpointer) {
    if (!g_anim.active) {
        g_animSource = 0;
        return G_SOURCE_REMOVE;
    }
    double t = std::min(1.0, (NowMs() - g_anim.start) / (double)std::max(1, g_anim.duration));
    double e = g_anim.bounce ? EaseOutBack(t) : EaseInOutCubic(t);
    auto lerp = [e](int a, int b) { return (int)std::lround(a + (b - a) * e); };
    // Largeur/hauteur interpolées autour du centre horizontal : le dock reste centré.
    const Rect &f = g_anim.from, &to = g_anim.to;
    int cx = lerp((f.l + f.r) / 2, (to.l + to.r) / 2);
    int w = std::max(1, lerp(f.w(), to.w()));
    int top = lerp(f.t, to.t);
    int h = std::max(1, lerp(f.h(), to.h()));
    PlaceWindow({cx - w / 2, top, cx - w / 2 + w, top + h});
    if (t >= 1.0) {
        g_anim.active = false;
        PlaceWindow(g_anim.to);
        if (g_state == State::Hidden) {
            gtk_widget_hide(g_window);
            g_overDock = false; // cachée sous la souris : aucune sortie ne sera signalée
        }
        g_animSource = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

// ------------------------------------------------------------------ états

bool ForegroundIsFullscreen() { return false; } // à faire : _NET_ACTIVE_WINDOW + _NET_WM_STATE_FULLSCREEN

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
    if (s == State::Progress) g_progressStart = NowMs();
    if (prev == State::Hidden && s != State::Hidden) {
        int x, y;
        bool down;
        Pointer(x, y, down);
        if (PointerKnown()) g_monitor = MonitorAt(x, y); // raccourci, rappel : on sort là où est la souris
        PlaceWindow(TargetRect(State::Hidden));
        gtk_widget_show_all(g_window);
        media::Command("refresh");
    }
    bool growing = s == State::Panel || (s == State::Bar && prev != State::Panel) ||
                   ((s == State::Mini || s == State::Music || s == State::Listen) && IsResting(prev));
    int duration = s == State::Panel ? 420 : s == State::Bar ? 380 : s == State::Mini || s == State::Music || s == State::Listen ? 360 : 260;
    if (prev == State::Panel && s == State::Bar) duration = 320;
    AnimateTo(TargetRect(s), duration, growing);
    NotifyState();
}

void Activate() {
    gtk_widget_show(g_window);
    gtk_window_present(GTK_WINDOW(g_window));
    gtk_widget_grab_focus(g_view);
}

void HoldUntilHovered(gint64 ms) {
    g_holdUntil = NowMs() + ms;
    g_releaseHoldOnEnter = true;
}

void FollowMouse(int x, int y) {
    if (g_state == State::Bar || g_state == State::Panel || g_state == State::Music || g_state == State::Listen ||
        g_anim.active)
        return;
    GdkMonitor* mon = MonitorAt(x, y);
    if (mon == Monitor()) return;
    g_monitor = mon;
    if (g_state == State::Hidden) return;
    PlaceWindow(TargetRect(State::Hidden));
    SetState(RestState(), true);
}

// Surveille la souris : bord haut de l'écran, survol, repli automatique (toutes les 30 ms).
gboolean Poll(gpointer) {
    int px, py;
    bool mouseDown;
    Pointer(px, py, mouseDown);
    if (PointerKnown()) {
        FollowMouse(px, py);
    } else { // position périmée : la souris est ailleurs, loin du dock
        px = py = -1000000;
        mouseDown = false;
    }
    Rect m = GeometryOf(Monitor());
    int cx = (m.l + m.r) / 2, half = g_cfg.width / 2;
    bool fullscreen = ForegroundIsFullscreen();
    // Un vrai bord : pas d'autre écran juste au-dessus.
    bool atTopEdge = py <= m.t + 1 && py >= m.t - 2 && px >= cx - half && px <= cx + half && !AnyMonitorContains(px, m.t - 1);
    gint64 now = NowMs();
    Rect tab = TargetRect(g_state == State::Mini ? State::Mini : State::Peek);
    tab.Inflate(6, 4);
    bool wantsReveal = (atTopEdge || (g_cfg.mode == 1 && tab.Contains(px, py))) && !mouseDown && !fullscreen;
    if (g_waitLeave) {
        if (!wantsReveal) g_waitLeave = false;
        wantsReveal = false;
    }

    switch (g_state) {
        case State::Hidden:
            if (RestState() != State::Hidden) SetState(RestState());
            else if (wantsReveal) SetState(g_cfg.revealDelay <= 0 ? State::Bar : State::Progress);
            break;
        case State::Peek:
        case State::Mini:
            if (RestState() != g_state) SetState(RestState());
            else if (wantsReveal && g_cfg.mode == 1) SetState(State::Bar);
            else if (wantsReveal) SetState(g_cfg.revealDelay <= 0 ? State::Bar : State::Progress);
            break;
        case State::Progress:
            if (!atTopEdge) SetState(RestState());
            else if (now - g_progressStart >= g_cfg.revealDelay) SetState(State::Bar);
            break;
        case State::Listen:
            if (!g_listening) SetState(RestState()); // l'écoute est finie (la voix envoie sa fin, ou le micro s'arrête)
            break;
        case State::Music:
        case State::Bar:
        case State::Panel: {
            Rect wr = g_rect;
            wr.Inflate(8, 8);
            if (g_state == State::Music && wantsReveal && !wr.Contains(px, py)) {
                SetState(State::Bar);
                break;
            }
            bool inside = wr.Contains(px, py) || atTopEdge;
            if (inside && g_releaseHoldOnEnter) {
                g_releaseHoldOnEnter = false;
                g_holdUntil = 0;
            }
            // Fenêtre active (ex. saisie dans le chat) : on reste ouvert, sauf pour la carte musique.
            bool hold = g_pinned || now < g_holdUntil || (g_state != State::Music && gtk_window_is_active(GTK_WINDOW(g_window)));
            if (inside || hold) {
                g_outsideSince = 0;
            } else {
                if (!g_outsideSince) g_outsideSince = now;
                if (now - g_outsideSince >= g_cfg.hideDelay) SetState(RestState());
            }
            break;
        }
    }
    return G_SOURCE_CONTINUE;
}

// « Aeria » entendu : une encoche s'ouvre tant que la voix écoute. Barre ou panneau ouverts, c'est leur champ
// « Demander » qui s'éclaire (js/features/voice.js) : on les empêche seulement de se replier pendant l'écoute.
void SetListening(bool on) {
    g_listening = on;
    if (on) {
        if (g_state == State::Bar || g_state == State::Panel) HoldUntilHovered(9000);
        else SetState(State::Listen);
    } else if (g_state == State::Listen) {
        SetState(RestState());
    }
}

// ------------------------------------------------------------------ entrées

void ToggleFromHotkey() {
    if (g_state == State::Panel && gtk_window_is_active(GTK_WINDOW(g_window))) {
        HideByUser();
    } else {
        OpenPanel();
        bridge::PostToPage("{\"t\":\"hotkey\"}");
    }
}

// Ctrl+Alt+Espace : prise de touche globale X11 (avec les variantes Verr. Maj / Verr. Num).
GdkFilterReturn HotkeyFilter(GdkXEvent* xev, GdkEvent*, gpointer) {
    auto* e = static_cast<XEvent*>(xev);
    if (e->type != KeyPress || e->xkey.keycode != g_hotkeyCode) return GDK_FILTER_CONTINUE;
    unsigned wanted = ControlMask | Mod1Mask, relevant = ControlMask | Mod1Mask | ShiftMask | Mod4Mask;
    if ((e->xkey.state & relevant) != wanted) return GDK_FILTER_CONTINUE;
    ToggleFromHotkey();
    return GDK_FILTER_REMOVE;
}

void InstallHotkey() {
    GdkDisplay* display = GdkDisp();
    if (!GDK_IS_X11_DISPLAY(display)) return; // Wayland : pas de raccourci global
    Display* xd = GDK_DISPLAY_XDISPLAY(display);
    Window root = DefaultRootWindow(xd);
    g_hotkeyCode = XKeysymToKeycode(xd, XK_space);
    gdk_x11_display_error_trap_push(display);
    for (unsigned lock : {0u, (unsigned)LockMask, (unsigned)Mod2Mask, (unsigned)(LockMask | Mod2Mask)})
        XGrabKey(xd, g_hotkeyCode, ControlMask | Mod1Mask | lock, root, False, GrabModeAsync, GrabModeAsync);
    XSync(xd, False);
    if (gdk_x11_display_error_trap_pop(display)) return; // déjà prise par une autre appli
    gdk_window_add_filter(gdk_get_default_root_window(), HotkeyFilter, nullptr);
}

} // namespace

// ------------------------------------------------------------------ API publique

void Init(void* window, void* layout, void* view) {
    g_window = GTK_WIDGET(window);
    g_layout = GTK_WIDGET(layout);
    g_view = GTK_WIDGET(view);
    g_rect = TargetRect(State::Hidden);
    UpdateWebBounds();
    gtk_widget_realize(g_window); // la découpe demande une fenêtre X existante
    ApplyShape();
    gdk_event_handler_set(OnGdkEvent, nullptr, nullptr);
    g_debug = g_getenv("AERIA_DEBUG");
    if (g_debug) {
        GdkScreen* screen = gdk_display_get_default_screen(GdkDisp());
        g_printerr("[dock] session=%s wayland=%s x11=%d composite=%d rgba=%d webkit=%u.%u.%u\n", g_getenv("XDG_SESSION_TYPE"),
                   g_getenv("WAYLAND_DISPLAY"), GDK_IS_X11_DISPLAY(GdkDisp()), gdk_screen_is_composited(screen),
                   gdk_screen_get_rgba_visual(screen) != nullptr, webkit_get_major_version(), webkit_get_minor_version(), webkit_get_micro_version());
    }
    // Alt+F4 / fermeture par le gestionnaire de fenêtres : on replie, on ne quitte pas.
    g_signal_connect(g_window, "delete-event", G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer) -> gboolean {
                         HideByUser();
                         return TRUE;
                     }),
                     nullptr);
    InstallHotkey();
    tray::Init();
    g_xwayland = GDK_IS_X11_DISPLAY(GdkDisp()) && g_getenv("WAYLAND_DISPLAY");
    if (g_xwayland) {
        WatchCrossing(g_window, &g_overDock);
        RebuildEdges();
        GCallback rebuild = G_CALLBACK(+[]() { RebuildEdges(); });
        g_signal_connect(GdkDisp(), "monitor-added", rebuild, nullptr);
        g_signal_connect(GdkDisp(), "monitor-removed", rebuild, nullptr);
        g_signal_connect(gdk_display_get_default_screen(GdkDisp()), "monitors-changed", rebuild, nullptr);
    }
    g_timeout_add(30, Poll, nullptr);
    g_timeout_add_seconds(1, +[](gpointer) -> gboolean {
        reminders::Check(); // échéances des rappels et du minuteur
        return G_SOURCE_CONTINUE;
    }, nullptr);
}

void Summon() { ToggleFromHotkey(); }

bool Pinned() { return g_pinned; }

void TogglePinFromMenu() {
    SetPinned(!g_pinned);
    if (g_pinned && g_state != State::Panel) SetState(State::Bar);
}

const Config& Cfg() { return g_cfg; }

void ApplyConfig(const std::string& payload) {
    Config c = g_cfg;
    bool autostartOn = autostart::Enabled(), wantAutostart = autostartOn;
    int wantVoice = -1;
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
        else if (k == "voiceWake") wantVoice = v == "1";
    }
    bool geometryChanged = c.mode != g_cfg.mode || c.width != g_cfg.width || c.panelH != g_cfg.panelH;
    if (wantVoice >= 0) voice::Apply(wantVoice == 1);
    bool widthChanged = c.width != g_cfg.width;
    g_cfg = c;
    if (wantAutostart != autostartOn) autostart::Set(wantAutostart);
    if (widthChanged) RebuildEdges();
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

void OpenPanelAsync() {
    g_idle_add(+[](gpointer) -> gboolean {
        OpenPanel();
        return G_SOURCE_REMOVE;
    }, nullptr);
}

void SetListeningAsync(bool on) {
    g_idle_add(+[](gpointer data) -> gboolean {
        SetListening(GPOINTER_TO_INT(data) != 0);
        return G_SOURCE_REMOVE;
    }, GINT_TO_POINTER(on ? 1 : 0));
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

void HoldFor(int ms) { g_holdUntil = NowMs() + std::max(0, ms); }

void SetMini(int widthDip) {
    int w = widthDip > 0 ? std::clamp(widthDip, 40, 640) : 0;
    if (w == g_miniW) return;
    bool resized = w && g_miniW;
    g_miniW = w;
    if (g_state == State::Mini && resized) AnimateTo(TargetRect(State::Mini), 320, false);
    else if (IsResting(g_state) && RestState() != g_state) SetState(RestState());
}

void RevealBriefly(int ms) {
    if (ForegroundIsFullscreen()) return;
    if (g_state != State::Bar && g_state != State::Panel) SetState(State::Bar);
    HoldUntilHovered(ms);
}

void RevealMusic(int ms, int widthDip) {
    if (ForegroundIsFullscreen()) return;
    int w = widthDip > 0 ? std::clamp(widthDip, 240, 480) : 380;
    bool resized = w != g_musicW;
    g_musicW = w;
    if (g_state == State::Music) {
        if (resized) AnimateTo(TargetRect(State::Music), 300, false);
    } else if (g_state != State::Bar && g_state != State::Panel) {
        SetState(State::Music);
    }
    HoldUntilHovered(ms);
}

} // namespace dock
