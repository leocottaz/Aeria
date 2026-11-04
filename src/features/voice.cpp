// Commande vocale, partie commune à Windows et Linux : voir features/voice.h pour le principe.
// Deux threads : la capture du micro (platform::StartCapture) découpe le son en phrases ; le thread
// « worker » les transcrit une par une avec whisper.cpp, cherche « Aeria » puis transmet la commande.
#include "features/voice.h"

#include "common/util.h"
#include "features/voice_platform.h"
#include "web/bridge.h"

#include "whisper.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

namespace voice {
namespace {

// Modèles proposés (conversions quantifiées de whisper.cpp, Hugging Face ggerganov/whisper.cpp). Aucun n'est livré :
// l'utilisateur choisit et télécharge le sien. Taille et SHA-256 sont figés : un fichier différent est refusé.
struct ModelInfo {
    const char* id;
    const char* file;
    const char* sha256;
    uint64_t bytes;
};
constexpr ModelInfo kModels[] = {
    {"base", "ggml-base-q5_1.bin", "422f1ae452ade6f30a004d7e5c6a43195e4433bc370bf23fac9cc591f01a8898", 59707625},
    {"small", "ggml-small-q5_1.bin", "ae85e4a935d7a567bd102fe55afc16bb595bdb618e11b2fc7591bc08120411bb", 190085487},
    {"medium", "ggml-medium-q5_0.bin", "19fea4b380c3a618ec4723c3eef2eb785ffba0d0538cf43f8f235e7b3b34220f", 539212467},
};
constexpr char kModelUrlBase[] = "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/";
constexpr char kDefaultModel[] = "small";

const ModelInfo* FindModel(const std::string& id) {
    for (const auto& m : kModels) if (id == m.id) return &m;
    return nullptr;
}

constexpr int kRate = 16000;
constexpr int kFrame = kRate / 50;            // 20 ms
constexpr int kPreRoll = 15;                  // 300 ms gardées avant le début détecté
constexpr double kMinThreshold = 80;          // rms minimal d'une voix : le micro de l'utilisateur donne ~100-150 en parlant, ~5-15 au repos
constexpr int kStartFrames = 4;              // 80 ms de voix d'affilée pour déclencher
constexpr int kMinVoiced = 12;                // moins de 240 ms de voix : bruit, on jette
constexpr int kEndIdle = 35;                  // 700 ms de silence ferment une phrase
constexpr int kEndAwake = 55;                 // 1,1 s pendant une commande (on réfléchit en parlant)
constexpr size_t kMaxSamples = 12 * kRate;    // une phrase ne dépasse pas 12 s
constexpr size_t kMinSamples = 11 * kRate / 10; // whisper refuse moins d'une seconde : on complète de silence
constexpr auto kAwakeWindow = std::chrono::seconds(8); // temps laissé pour dicter après « Aeria »
constexpr size_t kWakeDraftSamples = kRate;            // au repos, brouillon pour repérer « Aeria » dès 1 s de phrase
constexpr size_t kDraftStep = kRate / 2;               // à l'écoute, nouveau brouillon dès 0,5 s de voix en plus
constexpr auto kDraftEvery = std::chrono::milliseconds(300); // pause minimale entre deux brouillons

bool Debug() {
    static const bool on = std::getenv("AERIA_DEBUG") != nullptr;
    return on;
}

// ------------------------------------------------------------------ modèle choisi

std::mutex g_modelMutex;
const ModelInfo* g_model = FindModel(kDefaultModel); // modèle choisi (réglage voiceModel)
const ModelInfo* g_loaded = nullptr;                // modèle chargé par le worker en cours
std::string g_downloadId;                           // modèle en cours de téléchargement

const ModelInfo& Selected() {
    std::lock_guard<std::mutex> lock(g_modelMutex);
    return *g_model;
}
std::string PathOf(const ModelInfo& m) { return platform::ModelPath(m.file); }
bool Present(const ModelInfo& m) {
    std::error_code ec;
    return std::filesystem::exists(PathOf(m), ec);
}
// ,"models":["base","small"] : les modèles déjà sur ce PC (la page y choisit, voir js/features/voice.js)
std::string ModelsJson() {
    std::string j = ",\"models\":[";
    bool first = true;
    for (const auto& m : kModels) {
        if (!Present(m)) continue;
        j += std::string(first ? "" : ",") + "\"" + m.id + "\"";
        first = false;
    }
    return j + "]";
}

// ------------------------------------------------------------------ état partagé avec la page

std::mutex g_stateMutex;
std::string g_lastState; // dernier message d'état, renvoyé par « status »
std::atomic<bool> g_notch{false}; // l'encoche « j'écoute » du dock est ouverte

void Post(const char* state, int pct = -1, const std::string& msg = "", const std::string& text = "") {
    // L'encoche suit l'écoute : ouverte tant qu'on attend ou transcrit la commande, fermée sinon (« partial » :
    // texte en direct pendant l'écoute, il ne change rien).
    const std::string name = state;
    const bool listening = name == "awake" || name == "working";
    if (name != "partial" && g_notch.exchange(listening) != listening) platform::ShowListening(listening);
    std::string j = std::string("{\"t\":\"voice\",\"state\":\"") + state + "\"";
    if (pct >= 0) j += ",\"pct\":" + std::to_string(pct);
    if (!msg.empty()) j += ",\"msg\":" + util::Json(msg);
    if (!text.empty()) j += ",\"text\":" + util::Json(text);
    j += ModelsJson();
    if (name == "downloading") {
        std::lock_guard<std::mutex> lock(g_modelMutex);
        j += ",\"model\":\"" + g_downloadId + "\"";
    }
    j += "}";
    if (text.empty()) {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_lastState = j;
    }
    bridge::PostToPage(std::move(j));
}

// ------------------------------------------------------------------ découpage en phrases

// Détecteur d'énergie : une phrase commence après 80 ms de voix et finit après un silence. Le seuil suit
// le bruit de fond (plancher mesuré pendant les silences), donc il s'adapte à chaque micro et à chaque pièce.
class Segmenter {
public:
    using Emit = std::function<void(std::vector<float>&&)>;
    explicit Segmenter(Emit emit) : emit_(std::move(emit)) {}
    double Floor() const { return floor_; }
    bool Speaking() const { return speaking_; } // une phrase est en cours (lu par le worker, écrit par le thread du micro)

    // Copie de la phrase en cours (même niveau que la phrase finale) pour la transcription en direct ;
    // vide tant que personne ne parle.
    std::vector<float> Snapshot() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!speaking_ || voiced_ < kMinVoiced) return {};
        return ToFloat(utt_);
    }

    void Feed(const int16_t* s, size_t n, int endFrames) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 0; i < n; i++) {
            frame_[fill_++] = s[i];
            if (fill_ == kFrame) {
                fill_ = 0;
                Frame(endFrames);
            }
        }
    }

private:
    void Frame(int endFrames) {
        double energy = 0;
        for (int16_t v : frame_) energy += double(v) * v;
        const double rms = std::sqrt(energy / kFrame);
        const bool loud = rms > std::max(floor_ * 3.0, kMinThreshold);
        if (!speaking_) {
            // Le plancher descend vite mais ne monte que lentement : une voix douce ne doit pas le tirer vers le haut.
            if (!loud) floor_ = std::clamp((rms < floor_ ? 0.95 : 0.995) * floor_ + (rms < floor_ ? 0.05 : 0.005) * rms, 20.0, 2000.0);
            pre_.emplace_back(frame_, frame_ + kFrame);
            if (pre_.size() > kPreRoll) pre_.pop_front();
            run_ = loud ? run_ + 1 : 0;
            if (run_ >= kStartFrames) {
                speaking_ = true;
                voiced_ = run_;
                silence_ = 0;
                for (const auto& f : pre_) utt_.insert(utt_.end(), f.begin(), f.end());
                pre_.clear();
            }
            return;
        }
        utt_.insert(utt_.end(), frame_, frame_ + kFrame);
        if (loud) {
            silence_ = 0;
            voiced_++;
        } else {
            silence_++;
        }
        if (silence_ >= endFrames || utt_.size() >= kMaxSamples) Finish();
    }

    void Finish() {
        std::vector<int16_t> utt = std::move(utt_);
        const int voiced = voiced_;
        utt_.clear();
        speaking_ = false;
        run_ = 0;
        voiced_ = 0;
        silence_ = 0;
        if (Debug()) std::fprintf(stderr, "[voice] phrase finie : %zu ms, %d images de voix, plancher %.0f%s\n", utt.size() / 16, voiced, floor_, voiced < kMinVoiced ? " (trop court : jetée)" : "");
        if (voiced < kMinVoiced) return;
        emit_(ToFloat(utt));
    }

    // Niveau : les micros d'ordinateur portable sont souvent faibles, whisper préfère un signal franc.
    static std::vector<float> ToFloat(const std::vector<int16_t>& utt) {
        int peak = 1;
        for (int16_t v : utt) peak = std::max(peak, std::abs((int)v));
        const float gain = std::min(8.0f, 0.9f * 32768.0f / peak);
        std::vector<float> pcm(utt.size());
        for (size_t i = 0; i < utt.size(); i++) pcm[i] = std::clamp(utt[i] * gain / 32768.0f, -1.0f, 1.0f);
        return pcm;
    }

    std::mutex mutex_; // le thread du micro écrit la phrase en cours, le worker en prend des copies (Snapshot)
    Emit emit_;
    int16_t frame_[kFrame]{};
    int fill_ = 0;
    std::deque<std::vector<int16_t>> pre_;
    std::vector<int16_t> utt_;
    std::atomic<bool> speaking_{false};
    int run_ = 0, voiced_ = 0, silence_ = 0;
    double floor_ = 100;
};

// ------------------------------------------------------------------ « Aeria »

// Minuscules sans accents (UTF-8 français) ; tout ce qui n'est pas une lettre ou un chiffre est un séparateur.
std::string FoldWord(const std::string& w) {
    struct Map { const char* from; const char* to; };
    static const Map kMap[] = {{"à", "a"}, {"â", "a"}, {"ä", "a"}, {"é", "e"}, {"è", "e"}, {"ê", "e"}, {"ë", "e"},
                               {"î", "i"}, {"ï", "i"}, {"ô", "o"}, {"ö", "o"}, {"ù", "u"}, {"û", "u"}, {"ü", "u"},
                               {"ç", "c"}, {"œ", "oe"}, {"À", "a"}, {"Â", "a"}, {"É", "e"}, {"È", "e"}, {"Ê", "e"},
                               {"Î", "i"}, {"Ô", "o"}, {"Ù", "u"}, {"Û", "u"}, {"Ç", "c"}};
    std::string out;
    for (size_t i = 0; i < w.size();) {
        bool mapped = false;
        if ((unsigned char)w[i] >= 0x80) {
            for (const auto& m : kMap) {
                size_t n = std::char_traits<char>::length(m.from);
                if (w.compare(i, n, m.from) == 0) {
                    out += m.to;
                    i += n;
                    mapped = true;
                    break;
                }
            }
            if (!mapped) i++; // autre caractère non ASCII : ignoré
            continue;
        }
        out += (char)std::tolower((unsigned char)w[i]);
        i++;
    }
    return out;
}

// Distance de Levenshtein, bornée : seules les petites différences nous intéressent.
int EditDistance(const std::string& a, const std::string& b) {
    std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); j++) prev[j] = (int)j;
    for (size_t i = 1; i <= a.size(); i++) {
        cur[0] = (int)i;
        for (size_t j = 1; j <= b.size(); j++)
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

bool EndsWithRia(const std::string& s) { return s.size() >= 3 && s.compare(s.size() - 3, 3, "ria") == 0; }

// Un mot : une lettre d'écart au plus (« aria », « eria »), ou deux si le mot commence par une voyelle
// et finit en « a » ou garde « ria » (« uriam », « aurea » : ce que whisper entend de « Aeria » dit seul). Deux petits mots
// que whisper a coupés (« Aia, Ria ») sont aussi plus tolérants : la coupure est déjà un écart.
bool LooksLikeAeria(const std::string& key, int maxDistance = 1) {
    if (key.size() < 4 || key.size() > 7) return false;
    const int d = EditDistance(key, "aeria");
    if (d <= maxDistance) return true;
    return d == 2 && std::string("aeiouy").find(key[0]) != std::string::npos &&
           (key.back() == 'a' || key.find("ria") != std::string::npos);
}

bool IsWordByte(unsigned char c) { return std::isalnum(c) || c >= 0x80; }

// Ce qui reste d'une phrase sans ponctuation ni espaces de tête.
std::string TrimLead(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && !IsWordByte((unsigned char)s[i])) i++;
    std::string out = s.substr(i);
    while (!out.empty() && std::isspace((unsigned char)out.back())) out.pop_back();
    return out;
}

// Texte sans valeur : bruit transcrit en points de suspension, ou phrases que whisper invente sur du silence.
bool IsNoise(const std::string& text) {
    std::string t = FoldWord(text);
    if (t.size() < 2) return true;
    for (const char* junk : {"sous-titr", "soustitr", "amara", "merci d'avoir regarde", "merci davoir regarde"})
        if (t.find(junk) != std::string::npos) return true;
    return false;
}

// ------------------------------------------------------------------ transcription

std::atomic<bool> g_stop{false};
std::atomic<bool> g_partialRunning{false}; // une transcription en direct tourne : la phrase finale la coupe (voir Enqueue)
std::atomic<bool> g_abortPartial{false};

void QuietLog(ggml_log_level, const char*, void*) {}

// precise : une commande à comprendre (vocabulaire de commandes) ; sinon on guette seulement « Aeria ».
// beam : recherche en faisceau, plus juste mais plus lente (la phrase finale) ; sinon glouton (le direct).
// draft : brouillon en direct, abandonné si la phrase finale arrive.
std::string Transcribe(whisper_context* ctx, std::vector<float>& pcm, bool precise, bool beam, bool draft = false) {
    if (pcm.size() < kMinSamples) pcm.resize(kMinSamples, 0.0f);
    whisper_full_params p = whisper_full_default_params(beam ? WHISPER_SAMPLING_BEAM_SEARCH : WHISPER_SAMPLING_GREEDY);
    if (beam) p.beam_search.beam_size = 4;
    p.language = "fr";
    p.translate = false;
    p.n_threads = (int)std::clamp<unsigned>(std::thread::hardware_concurrency() / 2, 2, 6);
    p.no_timestamps = true;
    p.single_segment = true;
    p.no_context = true;
    p.print_progress = p.print_realtime = p.print_timestamps = p.print_special = false;
    p.suppress_nst = true;
    p.temperature_inc = 0.0f; // pas de nouvelle tentative plus chaude : plus rapide, et ça évite les inventions
    // Le prompt oriente le vocabulaire : le nom seul pour guetter, des phrases types pour une commande
    // (« minuteur » était compris « mais un minuteur » sans).
    p.initial_prompt = precise ? "Aeria, mets un minuteur de dix minutes. Rappelle-moi d'appeler Paul demain à quatorze heures. Ajoute une tâche. Quelle heure est-il ?"
                               : "Aeria.";
    bool abortable = draft; // seul un brouillon est abandonné quand la phrase finale arrive
    p.abort_callback = [](void* data) { return g_stop.load() || (*static_cast<bool*>(data) && g_abortPartial.load()); };
    p.abort_callback_user_data = &abortable;
    if (whisper_full(ctx, p, pcm.data(), (int)pcm.size()) != 0) return {};
    std::string text;
    for (int i = 0, n = whisper_full_n_segments(ctx); i < n; i++) {
        const char* seg = whisper_full_get_segment_text(ctx, i);
        if (seg) text += seg;
    }
    return TrimLead(text);
}

// ------------------------------------------------------------------ cycle de vie

std::mutex g_lifecycle; // Apply/Command et la fin d'un téléchargement ne démarrent pas le worker en même temps
std::thread g_worker;
std::atomic<bool> g_running{false};
std::atomic<bool> g_want{false};
std::atomic<bool> g_downloading{false};

std::mutex g_queueMutex;
std::condition_variable g_queueCv;
std::deque<std::vector<float>> g_queue;
std::atomic<bool> g_awake{false};
std::atomic<long long> g_listenRequest{0}; // « listen » de la page : le worker passe en écoute
std::atomic<long long> g_cancelRequest{0}; // « cancel » de la page : le worker arrête d'écouter
std::atomic<bool> g_oneShot{false};        // bouton micro sans « Aeria » activé : le micro ne reste ouvert que le temps d'une phrase
std::atomic<bool> g_earlyWake{false};      // « Aeria » repéré dans la transcription en direct, avant la fin de la phrase

void Enqueue(std::vector<float>&& pcm) {
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        if (g_partialRunning) g_abortPartial = true; // la phrase finale est prête : on abandonne le brouillon
        if (g_queue.size() >= 3) g_queue.pop_front(); // le worker est en retard : on garde le plus récent
        g_queue.push_back(std::move(pcm));
    }
    g_queueCv.notify_one();
}

// Un texte à envoyer à l'assistante : la page le pose dans la conversation.
// C'est la page qui décide quoi en faire : l'écrire dans le champ d'un bouton micro, l'afficher dans la pilule
// (dock rangé) ou ouvrir le panneau (dock déjà ouvert).
void Deliver(const std::string& text) {
    g_awake = false;
    g_earlyWake = false;
    Post("heard", -1, "", text);
    Post("idle");
}

void Handle(const std::string& text, std::chrono::steady_clock::time_point& awakeUntil) {
    if (Debug()) std::fprintf(stderr, "[voice] « %s »%s\n", text.c_str(), g_awake ? " (écoute)" : "");
    if (IsNoise(text)) return;
    std::string rest;
    const bool named = FindWakeWord(text, rest);
    if (g_awake) {
        // « Aeria » avait été repéré avant la fin de la phrase : la phrase finale le confirme (ou non).
        if (g_earlyWake.exchange(false)) {
            if (!named) { // fausse alerte du direct : on se rendort sans rien envoyer
                g_awake = false;
                Post("idle");
                return;
            }
            if (IsNoise(rest)) return; // « Aeria » seul : on attend la commande
            Deliver(rest);
            return;
        }
        Deliver(named && !IsNoise(rest) ? rest : text); // « Aeria, mets un minuteur » dit pendant l'écoute : on retire le nom
        return;
    }
    if (!named) return;
    if (!IsNoise(rest)) {
        Deliver(rest);
    } else {
        g_awake = true;
        awakeUntil = std::chrono::steady_clock::now() + kAwakeWindow;
        Post("awake");
    }
}

void Worker() {
    Post("loading");
    whisper_log_set(QuietLog, nullptr);
    whisper_context_params cp = whisper_context_default_params();
    cp.use_gpu = false;
    const ModelInfo& model = Selected();
    {
        std::lock_guard<std::mutex> lock(g_modelMutex);
        g_loaded = &model;
    }
    whisper_context* ctx = whisper_init_from_file_with_params(PathOf(model).c_str(), cp);
    if (!ctx) {
        Post("error", -1, "Modèle vocal illisible : télécharge-le de nouveau.");
        g_running = false;
        return;
    }

    long long zeroRun = 0; // échantillons nuls d'affilée : micro coupé par Windows ou muet
    bool zeroReported = false;
    Segmenter seg(Enqueue);
    std::string error;
    double levelSum = 0, levelPeak = 0; // traces (AERIA_DEBUG) : niveau du micro par seconde
    long levelN = 0;
    bool ok = platform::StartCapture(
        [&](const int16_t* s, size_t n) {
            if (Debug()) {
                for (size_t i = 0; i < n; i++) {
                    levelSum += double(s[i]) * s[i];
                    levelPeak = std::max(levelPeak, std::abs(double(s[i])));
                }
                levelN += (long)n;
                if (levelN >= kRate / 4) {
                    static const auto t0 = std::chrono::steady_clock::now();
                    std::fprintf(stderr, "[voice] %5.2f s niveau : rms %4.0f, crête %5.0f (plancher %.0f, seuil %.0f)\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), std::sqrt(levelSum / levelN), levelPeak, seg.Floor(), std::max(seg.Floor() * 3.0, kMinThreshold));
                    levelSum = levelPeak = 0;
                    levelN = 0;
                }
            }
            zeroRun = std::all_of(s, s + n, [](int16_t v) { return v == 0; }) ? zeroRun + (long long)n : 0;
            if (zeroRun > 5ll * kRate && !zeroReported) {
                zeroReported = true;
                Post("error", -1, "Le micro n'envoie aucun son : vérifie qu'il n'est pas coupé ou bloqué dans les paramètres de confidentialité.");
            } else if (zeroRun == 0 && zeroReported) {
                zeroReported = false;
                Post(g_awake ? "awake" : "idle");
            }
            seg.Feed(s, n, g_awake ? kEndAwake : kEndIdle);
        },
        error);
    if (!ok) {
        Post("error", -1, error.empty() ? "Micro introuvable." : error);
        whisper_free(ctx);
        g_running = false;
        return;
    }
    auto awakeUntil = std::chrono::steady_clock::now();
    if (Debug()) std::fprintf(stderr, "[voice] micro ouvert%s\n", g_oneShot ? " (une phrase, bouton)" : "");
    if (g_oneShot) { // bouton micro : on écoute tout de suite la commande
        g_awake = true;
        awakeUntil += kAwakeWindow;
        Post("awake");
    } else {
        Post("idle");
    }
    long long seenListen = g_listenRequest, seenCancel = g_cancelRequest;
    bool wakeChecked = false;     // brouillon de repérage de « Aeria » déjà fait pour la phrase en cours
    size_t draftLen = 0;          // taille de la phrase au dernier brouillon
    std::string draftShown;       // dernier texte en direct envoyé à la page
    auto lastDraft = std::chrono::steady_clock::now();
    while (!g_stop && !(g_oneShot && !g_awake)) {
        std::vector<float> pcm;
        {
            std::unique_lock<std::mutex> lock(g_queueMutex);
            g_queueCv.wait_for(lock, std::chrono::milliseconds(250), [] { return g_stop || !g_queue.empty(); });
            if (g_stop) break;
            if (!g_queue.empty()) {
                pcm = std::move(g_queue.front());
                g_queue.pop_front();
            }
        }
        if (g_cancelRequest != seenCancel) {
            seenCancel = g_cancelRequest;
            seenListen = g_listenRequest;
            if (g_awake) {
                g_awake = false;
                g_earlyWake = false;
                Post("idle");
                continue; // la phrase en cours d'écoute est jetée
            }
        }
        if (g_listenRequest != seenListen) {
            seenListen = g_listenRequest;
            g_earlyWake = false;
            g_awake = true;
            awakeUntil = std::chrono::steady_clock::now() + kAwakeWindow;
            Post("awake");
        }
        // Quelqu'un parle encore : le délai ne coupe pas la phrase en cours (3 s de grâce après la dernière voix,
        // le temps qu'elle arrive dans la file).
        if (g_awake && seg.Speaking()) awakeUntil = std::max(awakeUntil, std::chrono::steady_clock::now() + std::chrono::seconds(3));
        if (g_awake && std::chrono::steady_clock::now() > awakeUntil && pcm.empty()) {
            g_awake = false;
            g_earlyWake = false;
            Post("idle");
        }
        // Transcription en direct : pendant qu'on parle, la phrase en cours est retranscrite en brouillon.
        // Au repos, une seule passe par phrase, pour repérer « Aeria » sans attendre la fin ; à l'écoute, le texte
        // affiché suit la voix (la phrase finale, plus soignée, a le dernier mot).
        if (!seg.Speaking()) {
            wakeChecked = false;
            draftLen = 0;
            draftShown.clear();
        } else if (pcm.empty() && std::chrono::steady_clock::now() - lastDraft >= kDraftEvery) {
            std::vector<float> snap = seg.Snapshot();
            const bool wakeDraft = !g_awake && !wakeChecked && snap.size() >= kWakeDraftSamples;
            const bool textDraft = g_awake && snap.size() >= draftLen + kDraftStep;
            if (wakeDraft || textDraft) {
                wakeChecked = true;
                draftLen = snap.size();
                g_abortPartial = false;
                g_partialRunning = true;
                std::string draft = Transcribe(ctx, snap, g_awake, false, true);
                g_partialRunning = false;
                g_abortPartial = false;
                lastDraft = std::chrono::steady_clock::now();
                if (Debug()) std::fprintf(stderr, "[voice] brouillon (%zu ms) : « %s »\n", draftLen / 16, draft.c_str());
                std::string rest;
                const bool named = FindWakeWord(draft, rest);
                if (wakeDraft && named) { // « Aeria » est déjà dit : l'encoche s'ouvre sans attendre la fin de la phrase
                    g_awake = true;
                    g_earlyWake = true;
                    awakeUntil = std::chrono::steady_clock::now() + kAwakeWindow;
                    Post("awake");
                }
                const std::string shown = named ? rest : draft;
                if (g_awake && !IsNoise(shown) && shown != draftShown) {
                    draftShown = shown;
                    Post("partial", -1, "", shown);
                }
            }
        }
        if (pcm.empty()) continue;
        if (g_awake) Post("working");
        const auto transcribeStart = std::chrono::steady_clock::now();
        std::string text = Transcribe(ctx, pcm, g_awake, g_awake);
        if (Debug()) std::fprintf(stderr, "[voice] transcrit en %lld ms : « %s »\n", (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - transcribeStart).count(), text.c_str());
        std::string rest;
        // « Aeria, mets un minuteur… » d'une traite : le nom est trouvé, on refait la commande avec soin.
        if (!g_awake && FindWakeWord(text, rest) && !IsNoise(rest)) text = Transcribe(ctx, pcm, true, true);
        Handle(text, awakeUntil);
        if (g_awake) { // rien d'utilisable : on laisse le reste du délai
            awakeUntil = std::max(awakeUntil, std::chrono::steady_clock::now() + std::chrono::seconds(3));
            Post("awake");
        }
    }
    if (Debug()) std::fprintf(stderr, "[voice] micro fermé\n");
    platform::StopCapture();
    whisper_free(ctx);
    {
        std::lock_guard<std::mutex> lock(g_modelMutex);
        g_loaded = nullptr;
    }
    g_awake = false;
    g_earlyWake = false;
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        g_queue.clear();
    }
    g_running = false;
    if (g_oneShot.exchange(false) && !g_stop && !g_want) Post("off"); // la phrase du bouton micro est passée : micro refermé
}

bool ModelPresent() { return Present(Selected()); }

void StartLocked() {
    if (g_running || g_downloading) return;
    if (!ModelPresent()) {
        Post("nomodel");
        return;
    }
    if (g_worker.joinable()) g_worker.join();
    g_stop = false;
    g_running = true;
    g_worker = std::thread(Worker);
}

void StopLocked() {
    g_stop = true;
    g_queueCv.notify_all();
    if (g_worker.joinable()) g_worker.join();
    g_running = false;
}

void DownloadModel(const ModelInfo& model) {
    if (g_downloading.exchange(true)) return;
    {
        std::lock_guard<std::mutex> lock(g_modelMutex);
        g_downloadId = model.id;
    }
    std::thread([&model] {
        const std::string dest = PathOf(model);
        std::string error;
        Post("downloading", 0);
        int last = -1;
        bool ok = platform::Download(std::string(kModelUrlBase) + model.file, dest, model.bytes + 4ull * 1024 * 1024, model.sha256,
                                     [&](int pct) {
                                         if (pct != last) {
                                             last = pct;
                                             Post("downloading", pct);
                                         }
                                     },
                                     error);
        g_downloading = false;
        std::lock_guard<std::mutex> lock(g_lifecycle);
        if (!ok) {
            Post("error", -1, error.empty() ? "Téléchargement impossible." : error);
        } else if (g_want && !g_running) {
            g_oneShot = false;
            StartLocked();
        } else if (g_running) {
            Post(g_awake ? "awake" : "idle"); // une autre écoute tourne : on rafraîchit seulement la liste des modèles
        } else {
            Post("off");
        }
    }).detach();
}

} // namespace

bool FindWakeWord(const std::string& text, std::string& rest) {
    struct Word { std::string key; size_t end; };
    std::vector<Word> words;
    for (size_t i = 0; i < text.size();) {
        if (!IsWordByte((unsigned char)text[i])) {
            i++;
            continue;
        }
        size_t j = i;
        while (j < text.size() && IsWordByte((unsigned char)text[j])) j++;
        words.push_back({FoldWord(text.substr(i, j - i)), j});
        i = j;
    }
    // « A U R I A O » : whisper épelle parfois ce qu'il ne reconnaît pas ; une suite de lettres devient un mot.
    for (size_t k = 0; k + 2 < words.size();) {
        size_t run = k;
        while (run < words.size() && words[run].key.size() == 1) run++;
        if (run - k < 3) {
            k++;
            continue;
        }
        for (size_t i = k + 1; i < run; i++) words[k].key += words[i].key;
        words[k].end = words[run - 1].end;
        words.erase(words.begin() + (long)k + 1, words.begin() + (long)run);
        k++;
    }
    // On appelle quelqu'un au début de sa phrase : le nom au milieu d'une longue phrase (télé, conversation)
    // ne réveille pas l'assistante.
    for (size_t k = 0; k < words.size() && k < 3; k++) {
        bool hit = LooksLikeAeria(words[k].key);
        // « a éria », « Aia, Ria » : deux petits mots qui n'en font qu'un
        bool pair = !hit && k + 1 < words.size() && words[k].key.size() <= 3 && words[k + 1].key.size() <= 5 &&
                    LooksLikeAeria(words[k].key + words[k + 1].key, EndsWithRia(words[k + 1].key) ? 2 : 1);
        if (!hit && !pair) continue;
        rest = TrimLead(text.substr(words[pair ? k + 1 : k].end));
        return true;
    }
    return false;
}

void Apply(bool enabled) {
    std::lock_guard<std::mutex> lock(g_lifecycle);
    if (g_want.exchange(enabled) == enabled) return;
    if (enabled) {
        g_oneShot = false; // un micro ouvert par le bouton devient l'écoute permanente
        StartLocked();
    } else {
        StopLocked();
        if (!g_downloading) Post("off");
    }
}

void SetModel(const std::string& id) {
    const ModelInfo* model = FindModel(id);
    if (!model) return;
    std::lock_guard<std::mutex> lock(g_lifecycle);
    {
        std::lock_guard<std::mutex> modelLock(g_modelMutex);
        if (g_model == model) return;
        g_model = model;
    }
    if (g_running) {
        StopLocked(); // le micro est rechargé avec le nouveau modèle
        g_oneShot = false;
        if (g_want) StartLocked();
        else Post("off");
    } else if (g_want) {
        StartLocked();
    }
}

void Shutdown() {
    std::lock_guard<std::mutex> lock(g_lifecycle);
    g_want = false;
    g_oneShot = false;
    StopLocked(); // joint aussi un worker déjà fini de lui-même (bouton micro) : sinon std::terminate à la sortie
}

void Command(const std::string& command) {
    const size_t colon = command.find(':'); // « download:small », « delete:base »
    const std::string arg = command.substr(0, colon);
    const std::string param = colon == std::string::npos ? "" : command.substr(colon + 1);
    if (arg == "status") {
        std::string state;
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            state = g_lastState;
        }
        if (g_downloading) return; // la progression arrive d'elle-même
        if (!g_running) state = std::string("{\"t\":\"voice\",\"state\":\"") + (ModelPresent() ? "off" : "nomodel") + "\"" + ModelsJson() + "}";
        if (!state.empty()) bridge::PostToPage(std::move(state));
    } else if (arg == "download") {
        const ModelInfo* model = param.empty() ? &Selected() : FindModel(param);
        if (model) DownloadModel(*model);
    } else if (arg == "delete") {
        const ModelInfo* model = FindModel(param);
        if (!model) return;
        std::lock_guard<std::mutex> lock(g_lifecycle);
        bool inUse = false;
        {
            std::lock_guard<std::mutex> modelLock(g_modelMutex);
            if (g_downloading && g_downloadId == model->id) return; // en cours de téléchargement : on ne touche pas au fichier
            inUse = g_running && g_loaded == model;
        }
        if (inUse) StopLocked(); // le micro l'utilise : on l'arrête d'abord
        std::error_code ec;
        std::filesystem::remove(PathOf(*model), ec);
        if (g_want && !g_running && !ModelPresent()) Post("nomodel");
        else Post(g_running ? (g_awake ? "awake" : "idle") : "off");
    } else if (arg == "listen") {
        std::lock_guard<std::mutex> lock(g_lifecycle);
        if (g_running) {
            g_listenRequest++;
            g_queueCv.notify_all();
        } else if (!g_downloading) { // bouton micro avec « Aeria » désactivé : micro ouvert le temps d'une phrase
            g_oneShot = !g_want;
            StartLocked();
            if (!g_running) g_oneShot = false; // pas de modèle : StartLocked a déjà prévenu la page
        }
    } else if (arg == "cancel") {
        g_cancelRequest++;
        g_queueCv.notify_all();
    }
}

} // namespace voice
