// Capture d'écran sous Linux : racine X11 (gdk_pixbuf_get_from_window). Sous Wayland (même via XWayland)
// la racine est noire : on le dit au lieu d'envoyer une image vide. Même interface que features/screen.cpp.
#include "features/screen.h"

#include "common/util.h"
#include "web/bridge.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gtk/gtk.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

namespace screen {
namespace {

constexpr int kMaxSide = 1568;
constexpr int kKeepFiles = 1; // la dernière seulement : elle montre l'écran de l'utilisateur

void Reply(const std::string& id, const std::string& fields) {
    bridge::PostToPage("{\"t\":\"reply\",\"id\":" + util::Json(id) + "," + fields + "}");
}

void Fail(const std::string& id, const std::string& message) {
    Reply(id, "\"ok\":false,\"error\":" + util::Json(message));
}

std::string SaveFile(const std::string& bytes) {
    namespace fs = std::filesystem;
    fs::path dir = fs::path(util::DataDir()) / "claude-chat" / "screens"; // = dossier de travail de claude_code_linux.cpp
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::vector<fs::path> old;
    for (auto& e : fs::directory_iterator(dir, ec))
        if (e.path().extension() == ".jpg") old.push_back(e.path());
    std::sort(old.begin(), old.end());
    while ((int)old.size() >= kKeepFiles) fs::remove(old.front(), ec), old.erase(old.begin());
    std::string name = "screen-" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                       std::chrono::system_clock::now().time_since_epoch()).count()) + ".jpg";
    std::ofstream(dir / name, std::ios::binary).write(bytes.data(), (std::streamsize)bytes.size());
    return "screens/" + name;
}

struct Job {
    std::string id;
    bool toFile;
    GdkRectangle area;
    std::vector<std::pair<GtkWindow*, double>> hidden; // fenêtres de l'appli rendues transparentes pendant la capture
};

gboolean Grab(gpointer data) {
    std::unique_ptr<Job> job(static_cast<Job*>(data));
    GdkPixbuf* shot = gdk_pixbuf_get_from_window(gdk_get_default_root_window(), job->area.x, job->area.y, job->area.width, job->area.height);
    for (auto& [w, opacity] : job->hidden) gtk_widget_set_opacity(GTK_WIDGET(w), opacity); // le dock revient
    if (!shot) {
        Fail(job->id, "La capture d'écran a échoué.");
        return G_SOURCE_REMOVE;
    }
    double k = std::min(1.0, kMaxSide / (double)std::max(job->area.width, job->area.height));
    int tw = std::max(1, (int)(job->area.width * k)), th = std::max(1, (int)(job->area.height * k));
    GdkPixbuf* small = k < 1.0 ? gdk_pixbuf_scale_simple(shot, tw, th, GDK_INTERP_BILINEAR) : g_object_ref(shot);
    g_object_unref(shot);
    gchar* buf = nullptr;
    gsize len = 0;
    GError* err = nullptr;
    bool ok = small && gdk_pixbuf_save_to_buffer(small, &buf, &len, "jpeg", &err, "quality", "80", nullptr);
    if (small) g_object_unref(small);
    if (err) g_error_free(err);
    if (!ok) {
        Fail(job->id, "La capture d'écran a échoué.");
        return G_SOURCE_REMOVE;
    }
    std::string jpeg(buf, len);
    g_free(buf);
    std::string fields = "\"ok\":true,\"media\":\"image/jpeg\",\"w\":" + std::to_string(tw) + ",\"h\":" + std::to_string(th) +
                         ",\"data\":" + util::Json(util::Base64(jpeg));
    if (job->toFile) fields += ",\"file\":" + util::Json(SaveFile(jpeg));
    Reply(job->id, fields);
    return G_SOURCE_REMOVE;
}

} // namespace

void Capture(const std::string& id, bool toFile) {
    if (g_getenv("WAYLAND_DISPLAY") || g_strcmp0(g_getenv("XDG_SESSION_TYPE"), "wayland") == 0)
        return Fail(id, "La capture d'écran n'est pas encore prise en charge sous Wayland.");
    GdkDisplay* display = gdk_display_get_default();
    GdkSeat* seat = gdk_display_get_default_seat(display);
    int px = 0, py = 0;
    gdk_device_get_position(gdk_seat_get_pointer(seat), nullptr, &px, &py);
    GdkMonitor* mon = gdk_display_get_monitor_at_point(display, px, py); // le dock suit la souris quand il est rangé
    if (!mon) return Fail(id, "Écran introuvable.");
    GdkRectangle g;
    gdk_monitor_get_geometry(mon, &g);
    int s = std::max(1, gdk_monitor_get_scale_factor(mon));
    auto* job = new Job{id, toFile, {g.x * s, g.y * s, g.width * s, g.height * s}, {}};
    // Le dock reste affiché pour l'utilisateur mais est rendu transparent le temps de la capture.
    for (GList* l = gtk_window_list_toplevels(); l; l = l->next) {
        auto* w = GTK_WINDOW(l->data);
        if (!gtk_widget_get_visible(GTK_WIDGET(w))) continue;
        job->hidden.emplace_back(w, gtk_widget_get_opacity(GTK_WIDGET(w)));
        gtk_widget_set_opacity(GTK_WIDGET(w), 0);
    }
    g_timeout_add(150, Grab, job); // le temps que le compositeur redessine
}

} // namespace screen
