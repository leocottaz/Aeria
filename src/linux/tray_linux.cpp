// Zone de notification sous Linux.
//  - Icône : StatusNotifierItem (D-Bus, org.kde.StatusNotifierItem), sans libappindicator. KDE,
//    XFCE, Cinnamon, etc. l'affichent ; GNOME demande l'extension « AppIndicator ».
//    Clic gauche : ouvre le panneau. Clic droit : le menu est dessiné par nous (méthode ContextMenu).
//  - Notifications : service org.freedesktop.Notifications (pas de libnotify).
#include "app/tray.h"

#include "app/dock.h"
#include "linux/host.h"
#include "web/bridge.h"

#include <gio/gio.h>
#include <gtk/gtk.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace tray {
namespace {

constexpr char kObjectPath[] = "/StatusNotifierItem";
constexpr char kTooltip[] = "Aeria — Ctrl+Alt+Espace";

const char kXml[] = R"XML(<node>
  <interface name="org.kde.StatusNotifierItem">
    <property name="Category" type="s" access="read"/>
    <property name="Id" type="s" access="read"/>
    <property name="Title" type="s" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="WindowId" type="i" access="read"/>
    <property name="IconName" type="s" access="read"/>
    <property name="IconPixmap" type="a(iiay)" access="read"/>
    <property name="OverlayIconName" type="s" access="read"/>
    <property name="OverlayIconPixmap" type="a(iiay)" access="read"/>
    <property name="AttentionIconName" type="s" access="read"/>
    <property name="AttentionIconPixmap" type="a(iiay)" access="read"/>
    <property name="ToolTip" type="(sa(iiay)ss)" access="read"/>
    <property name="ItemIsMenu" type="b" access="read"/>
    <method name="ContextMenu"><arg type="i" name="x" direction="in"/><arg type="i" name="y" direction="in"/></method>
    <method name="Activate"><arg type="i" name="x" direction="in"/><arg type="i" name="y" direction="in"/></method>
    <method name="SecondaryActivate"><arg type="i" name="x" direction="in"/><arg type="i" name="y" direction="in"/></method>
    <method name="Scroll"><arg type="i" name="delta" direction="in"/><arg type="s" name="orientation" direction="in"/></method>
    <signal name="NewIcon"/>
    <signal name="NewToolTip"/>
    <signal name="NewStatus"><arg type="s" name="status"/></signal>
  </interface>
</node>)XML";

GDBusConnection* g_bus = nullptr;
GVariant* g_pixmaps = nullptr; // a(iiay), créé une fois
std::string g_busName;
bool g_registered = false;
bool g_nameOwned = false; // notre nom org.kde.StatusNotifierItem-… est obtenu
bool g_watcherUp = false; // la zone de notification (StatusNotifierWatcher) est présente

// Orbe dégradé (le même que l'icône Windows), en ARGB32 non prémultiplié, octets A R V B.
GVariant* MakeOrbPixmaps() {
    GVariantBuilder list;
    g_variant_builder_init(&list, G_VARIANT_TYPE("a(iiay)"));
    for (int size : {22, 32, 48, 64}) {
        std::vector<uint8_t> px((size_t)size * size * 4);
        const double c = size / 2.0, r = size / 2.0 - 0.5;
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                double dx = x + 0.5 - c, dy = y + 0.5 - c;
                double cov = std::clamp(r - std::sqrt(dx * dx + dy * dy) + 0.5, 0.0, 1.0);
                double t = (x + y) / (2.0 * size);
                double R = 150 + (80 - 150) * t, G = 110 + (210 - 110) * t, B = 255 + (250 - 255) * t;
                double hx = x - size * 0.34, hy = y - size * 0.3;
                double h = std::max(0.0, 1.0 - std::sqrt(hx * hx + hy * hy) / (size * 0.38)) * 0.65;
                R += (255 - R) * h, G += (255 - G) * h, B += (255 - B) * h;
                uint8_t* p = &px[((size_t)y * size + x) * 4];
                p[0] = (uint8_t)(cov * 255), p[1] = (uint8_t)R, p[2] = (uint8_t)G, p[3] = (uint8_t)B;
            }
        }
        g_variant_builder_add(&list, "(ii@ay)", size, size,
                              g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, px.data(), px.size(), 1));
    }
    return g_variant_ref_sink(g_variant_builder_end(&list));
}

GVariant* EmptyPixmaps() { return g_variant_new_array(G_VARIANT_TYPE("(iiay)"), nullptr, 0); }

// ------------------------------------------------------------------ menu du clic droit

void ShowMenu(int x, int y) {
    GtkWidget* menu = gtk_menu_new();
    auto add = [&](const char* label, void (*fn)(), bool check = false, bool checked = false) {
        GtkWidget* item = check ? gtk_check_menu_item_new_with_label(label) : gtk_menu_item_new_with_label(label);
        if (check) gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), checked);
        g_signal_connect(item, "activate", G_CALLBACK(+[](GtkMenuItem*, gpointer f) { reinterpret_cast<void (*)()>(f)(); }),
                         reinterpret_cast<gpointer>(fn));
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    };
    add("Ouvrir le dock (Ctrl+Alt+Espace)", +[] {
        dock::OpenPanel();
        bridge::PostToPage("{\"t\":\"show\",\"tab\":\"claude\"}");
    });
    add("Garder affiché", +[] { dock::TogglePinFromMenu(); }, true, dock::Pinned());
    add("Réglages", +[] {
        dock::OpenPanel();
        bridge::PostToPage("{\"t\":\"show\",\"tab\":\"settings\"}");
    });
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    add("Quitter", +[] { host::Quit(); });
    gtk_widget_show_all(menu);
    g_object_ref_sink(menu);
    g_signal_connect(menu, "selection-done", G_CALLBACK(+[](GtkMenuShell* m, gpointer) {
                         // détruit après l'activation de l'entrée choisie
                         g_idle_add(+[](gpointer w) -> gboolean {
                             gtk_widget_destroy(GTK_WIDGET(w));
                             g_object_unref(w);
                             return G_SOURCE_REMOVE;
                         }, m);
                     }),
                     nullptr);
    // Le menu s'ouvre là où l'hôte de la zone de notification a été cliqué (x, y en pixels d'écran) ;
    // sans position, sous le pointeur. Il n'y a pas de vrai évènement GTK : on en fabrique un.
    GdkWindow* root = gdk_get_default_root_window();
    GdkDevice* pointer = gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_display_get_default()));
    if (x == 0 && y == 0) gdk_device_get_position(pointer, nullptr, &x, &y);
    GdkEvent* trigger = gdk_event_new(GDK_BUTTON_PRESS);
    trigger->button.window = static_cast<GdkWindow*>(g_object_ref(root));
    trigger->button.time = GDK_CURRENT_TIME;
    gdk_event_set_device(trigger, pointer);
    GdkRectangle rect{x, y, 1, 1};
    gtk_menu_popup_at_rect(GTK_MENU(menu), root, &rect, GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST, trigger);
    gdk_event_free(trigger);
}

// ------------------------------------------------------------------ StatusNotifierItem

void OnMethod(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* method, GVariant* params,
              GDBusMethodInvocation* invocation, gpointer) {
    std::string m = method;
    if (m == "Activate" || m == "SecondaryActivate") {
        dock::OpenPanel();
    } else if (m == "ContextMenu") {
        gint32 x = 0, y = 0;
        g_variant_get(params, "(ii)", &x, &y);
        ShowMenu(x, y);
    }
    g_dbus_method_invocation_return_value(invocation, nullptr);
}

GVariant* OnGetProperty(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* name, GError**, gpointer) {
    std::string n = name;
    if (n == "Category") return g_variant_new_string("ApplicationStatus");
    if (n == "Id") return g_variant_new_string("aeria");
    if (n == "Title") return g_variant_new_string("Aeria");
    if (n == "Status") return g_variant_new_string("Active");
    if (n == "WindowId") return g_variant_new_int32(0);
    if (n == "IconName" || n == "OverlayIconName" || n == "AttentionIconName") return g_variant_new_string("");
    if (n == "IconPixmap") return g_variant_ref(g_pixmaps);
    if (n == "OverlayIconPixmap" || n == "AttentionIconPixmap") return EmptyPixmaps();
    if (n == "ToolTip") return g_variant_new("(s@a(iiay)ss)", "", EmptyPixmaps(), kTooltip, "");
    if (n == "ItemIsMenu") return g_variant_new_boolean(FALSE);
    return nullptr;
}

// Une seule inscription par vie du watcher, et seulement une fois notre nom obtenu.
void RegisterWithWatcher() {
    if (!g_bus || !g_nameOwned || !g_watcherUp || g_registered) return;
    g_registered = true;
    g_dbus_connection_call(g_bus, "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher", "org.kde.StatusNotifierWatcher",
                           "RegisterStatusNotifierItem", g_variant_new("(s)", g_busName.c_str()), nullptr,
                           G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr, nullptr);
}

} // namespace

void Init() {
    GError* err = nullptr;
    g_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &err);
    if (err) g_error_free(err);
    if (!g_bus) return; // pas de bus de session : pas d'icône
    g_pixmaps = MakeOrbPixmaps();

    GDBusNodeInfo* info = g_dbus_node_info_new_for_xml(kXml, nullptr);
    static const GDBusInterfaceVTable vtable = {OnMethod, OnGetProperty, nullptr, {nullptr}};
    g_dbus_connection_register_object(g_bus, kObjectPath, info->interfaces[0], &vtable, nullptr, nullptr, nullptr);
    g_dbus_node_info_unref(info);

    g_busName = "org.kde.StatusNotifierItem-" + std::to_string(getpid()) + "-1";
    g_bus_own_name_on_connection(g_bus, g_busName.c_str(), G_BUS_NAME_OWNER_FLAGS_NONE,
                                 +[](GDBusConnection*, const gchar*, gpointer) {
                                     g_nameOwned = true;
                                     RegisterWithWatcher();
                                 },
                                 nullptr, nullptr, nullptr);
    // La zone de notification peut démarrer après nous (ou redémarrer) : on se réinscrit à chaque apparition.
    g_bus_watch_name_on_connection(g_bus, "org.kde.StatusNotifierWatcher", G_BUS_NAME_WATCHER_FLAGS_NONE,
                                   +[](GDBusConnection*, const gchar*, const gchar*, gpointer) {
                                       g_watcherUp = true;
                                       RegisterWithWatcher();
                                   },
                                   +[](GDBusConnection*, const gchar*, gpointer) {
                                       g_watcherUp = false;
                                       g_registered = false;
                                   },
                                   nullptr, nullptr);
}

void Notify(const std::string& title, const std::string& text, bool sound) {
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (!bus) return;
    GVariantBuilder hints;
    g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
    if (sound) g_variant_builder_add(&hints, "{sv}", "sound-name", g_variant_new_string("message-new-instant"));
    GVariant* params = g_variant_new("(susssasa{sv}i)", "Aeria", (guint32)0, "", title.c_str(), text.c_str(), nullptr,
                                     &hints, (gint32)-1);
    g_dbus_connection_call(bus, "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                           "org.freedesktop.Notifications", "Notify", params, nullptr, G_DBUS_CALL_FLAGS_NONE, 2000, nullptr,
                           nullptr, nullptr);
    g_object_unref(bus);
}

} // namespace tray
