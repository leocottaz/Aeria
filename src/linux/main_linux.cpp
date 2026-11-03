// Aeria — port Linux (GTK3 + WebKitGTK). La fenêtre du dock affiche l'interface ;
// les commandes de la page sont routées par bridge_linux.cpp. Modules déjà portés : clés API,
// requêtes HTTPS (libcurl), Claude Code. Protocole : docs/ARCHITECTURE.md.

#include "ai/api_key.h"
#include "app/autostart.h"
#include "app/dock.h"
#include "common/util.h"
#include "features/media.h"
#include "features/voice.h"
#include "linux/host.h"
#include "web/bridge.h"

#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>

namespace {

GtkWidget* g_window = nullptr;
WebKitWebView* g_view = nullptr;

// Page -> natif : "commande\nargument".
void OnMessage(WebKitUserContentManager*, WebKitJavascriptResult* result, gpointer) {
    JSCValue* value = webkit_javascript_result_get_js_value(result);
    if (!jsc_value_is_string(value)) return;
    char* raw = jsc_value_to_string(value);
    std::string msg = raw;
    g_free(raw);

    bridge::HandleMessage(msg); // routeur commun : src/linux/bridge_linux.cpp
}

// L'interface est servie sous aeria://app/… (comme https://aeria.local/ avec WebView2) : depuis
// file://, WebKit refuse les modules JavaScript. Seuls les fichiers de ui/ sont accessibles.
const char* MimeFor(const std::string& path) {
    static const struct { const char* ext; const char* mime; } kTypes[] = {
        {".html", "text/html"},       {".js", "text/javascript"}, {".mjs", "text/javascript"}, {".css", "text/css"},
        {".json", "application/json"}, {".svg", "image/svg+xml"},  {".png", "image/png"},        {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"},      {".webp", "image/webp"},    {".gif", "image/gif"},        {".ico", "image/x-icon"},
        {".woff2", "font/woff2"},     {".woff", "font/woff"},     {".ttf", "font/ttf"},         {".mp3", "audio/mpeg"},
        {".wav", "audio/wav"},        {".ogg", "audio/ogg"},
    };
    for (const auto& t : kTypes)
        if (path.size() >= strlen(t.ext) && path.compare(path.size() - strlen(t.ext), strlen(t.ext), t.ext) == 0) return t.mime;
    return "application/octet-stream";
}

void ServeUi(WebKitURISchemeRequest* request, gpointer) {
    namespace fs = std::filesystem;
    char* raw = g_uri_unescape_string(webkit_uri_scheme_request_get_path(request), nullptr);
    std::string rel = raw ? raw : "";
    g_free(raw);
    if (rel.empty() || rel == "/") rel = "/index.html";

    std::error_code ec;
    fs::path root = fs::weakly_canonical(fs::path(util::ExeDir()) / "ui", ec);
    fs::path file = fs::weakly_canonical(root / rel.substr(1), ec);
    // Jamais en dehors de ui/ (« .. », liens symboliques).
    bool inside = !ec && file.string().rfind(root.string() + "/", 0) == 0;
    std::ifstream in(file, std::ios::binary);
    if (!inside || !in) {
        GError* err = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Introuvable");
        webkit_uri_scheme_request_finish_error(request, err);
        g_error_free(err);
        return;
    }
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    gsize len = bytes.size();
    GInputStream* stream = g_memory_input_stream_new_from_data(g_memdup2(bytes.data(), len), (gssize)len, g_free);
    webkit_uri_scheme_request_finish(request, stream, (gint64)len, MimeFor(file.string()));
    g_object_unref(stream);
}

gboolean OnNavigate(WebKitWebView*, WebKitPolicyDecision* decision, WebKitPolicyDecisionType type, gpointer) {
    if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) return FALSE;
    auto* nav = WEBKIT_NAVIGATION_POLICY_DECISION(decision);
    WebKitURIRequest* req = webkit_navigation_action_get_request(webkit_navigation_policy_decision_get_navigation_action(nav));
    std::string uri = webkit_uri_request_get_uri(req);
    if (uri.rfind("aeria://", 0) == 0 || uri == "about:blank") return FALSE;
    webkit_policy_decision_ignore(decision); // liens externes : navigateur, pas la vue du dock
    if (uri.rfind("http", 0) == 0) gtk_show_uri_on_window(GTK_WINDOW(g_window), uri.c_str(), GDK_CURRENT_TIME, nullptr);
    return TRUE;
}

void Activate(GtkApplication* app, gpointer) {
    if (g_window) { // 2e instance : on ouvre le panneau de la première
        dock::Summon();
        return;
    }
    g_window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(g_window), "Aeria");
    gtk_window_set_decorated(GTK_WINDOW(g_window), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(g_window), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(g_window), TRUE);
    gtk_window_set_keep_above(GTK_WINDOW(g_window), TRUE);
    gtk_window_stick(GTK_WINDOW(g_window));
    gtk_window_set_focus_on_map(GTK_WINDOW(g_window), FALSE); // les états du dock ne volent pas le focus
    gtk_window_set_default_size(GTK_WINDOW(g_window), 1, 1);
    gtk_widget_set_app_paintable(g_window, TRUE);
    if (GdkVisual* rgba = gdk_screen_get_rgba_visual(gtk_widget_get_screen(g_window)))
        gtk_widget_set_visual(g_window, rgba); // fenêtre transparente : le verre arrondi est dessiné par la page
    g_signal_connect(g_window, "draw", G_CALLBACK(+[](GtkWidget*, cairo_t* cr, gpointer) -> gboolean {
                         cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE); // certains thèmes laissent un fond opaque
                         cairo_set_source_rgba(cr, 0, 0, 0, 0);
                         cairo_paint(cr);
                         return FALSE;
                     }),
                     nullptr);

    WebKitUserContentManager* ucm = webkit_user_content_manager_new();
    g_signal_connect(ucm, "script-message-received::aeria", G_CALLBACK(OnMessage), nullptr);
    webkit_user_content_manager_register_script_message_handler(ucm, "aeria");

    WebKitWebContext* context = webkit_web_context_new();
    webkit_web_context_register_uri_scheme(context, "aeria", ServeUi, nullptr, nullptr);
    WebKitSecurityManager* security = webkit_web_context_get_security_manager(context);
    webkit_security_manager_register_uri_scheme_as_secure(security, "aeria");
    webkit_security_manager_register_uri_scheme_as_cors_enabled(security, "aeria");

    g_view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "web-context", context, "user-content-manager", ucm, nullptr));
    GdkRGBA clear{0, 0, 0, 0};
    webkit_web_view_set_background_color(g_view, &clear);
    g_signal_connect(g_view, "decide-policy", G_CALLBACK(OnNavigate), nullptr);
    // GtkLayout : sa taille minimale est nulle, donc la fenêtre peut devenir plus petite que la vue,
    // qui garde la taille du panneau complet (dock_linux.cpp la centre et la découpe).
    GtkWidget* layout = gtk_layout_new(nullptr, nullptr);
    gtk_layout_set_size(GTK_LAYOUT(layout), 1, 1);
    gtk_layout_put(GTK_LAYOUT(layout), GTK_WIDGET(g_view), 0, 0);
    gtk_container_add(GTK_CONTAINER(g_window), layout);
    gtk_widget_show_all(layout);
    dock::Init(g_window, layout, g_view);

    webkit_web_view_load_uri(g_view, "aeria://app/index.html");
    // La fenêtre reste cachée : le dock l'affiche quand la page envoie « ready » (OnPageReady).
}

} // namespace

namespace host {

void ChooseFile(std::function<void(const std::string&)> done) {
    GtkFileChooserNative* dialog = gtk_file_chooser_native_new("Choisir ce que la routine ouvre", GTK_WINDOW(g_window),
                                                               GTK_FILE_CHOOSER_ACTION_OPEN, "Ouvrir", "Annuler");
    auto* callback = new std::function<void(const std::string&)>(std::move(done));
    g_signal_connect(dialog, "response", G_CALLBACK(+[](GtkNativeDialog* d, gint response, gpointer data) {
                         std::unique_ptr<std::function<void(const std::string&)>> cb(
                             static_cast<std::function<void(const std::string&)>*>(data));
                         std::string path;
                         if (response == GTK_RESPONSE_ACCEPT) {
                             char* f = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d));
                             if (f) path = f;
                             g_free(f);
                         }
                         (*cb)(path);
                         g_object_unref(d);
                     }),
                     callback);
    gtk_native_dialog_show(GTK_NATIVE_DIALOG(dialog));
}

void Quit() { g_application_quit(g_application_get_default()); }

void OpenUrl(const std::string& url) { gtk_show_uri_on_window(GTK_WINDOW(g_window), url.c_str(), GDK_CURRENT_TIME, nullptr); }

// Natif -> page : window.__fromNative(<objet JSON>), défini dans ui/js/core/bridge.js.
void EvalJson(const std::string& json) {
    if (!g_view) return;
    std::string js = "window.__fromNative && window.__fromNative(" + json + ");";
    webkit_web_view_evaluate_javascript(g_view, js.c_str(), -1, nullptr, nullptr, nullptr, nullptr, nullptr);
}

} // namespace host

int main(int argc, char** argv) {
    // X11 (ou XWayland) d'abord : Wayland ne permet ni de placer la fenêtre, ni de lire la souris
    // sur le bord de l'écran, ni de capter un raccourci global.
    gdk_set_allowed_backends("x11,wayland");
    api_key::Load();
    GtkApplication* app = gtk_application_new("io.github.aeria", G_APPLICATION_DEFAULT_FLAGS); // instance unique via D-Bus
    g_signal_connect(app, "activate", G_CALLBACK(Activate), nullptr);
    media::Start();
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    voice::Shutdown();
    media::Stop();
    g_object_unref(app);
    return status;
}
