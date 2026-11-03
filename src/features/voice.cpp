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

constexpr char kModelUrl[] = "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small-q5_1.bin";
constexpr char kModelSha256[] = "ae85e4a935d7a567bd102fe55afc16bb595bdb618e11b2fc7591bc08120411bb";
constexpr uint64_t kModelMaxBytes = 260ull * 1024 * 1024;

constexpr int kRate = 16000;
constexpr int kFrame = kRate / 50;            // 20 ms
constexpr int kPreRoll = 15;                  // 300 ms gardées avant le début détecté
constexpr int kStartFrames = 4;               // 80 ms de voix d'affilée pour déclencher
constexpr int kMinVoiced = 12;                // moins de 240 ms de voix : bruit, on jette
constexpr int kEndIdle = 35;                  // 700 ms de silence ferment une phrase
constexpr int kEndAwake = 55;                 // 1,1 s pendant une commande (on réfléchit en parlant)
constexpr size_t kMaxSamples = 12 * kRate;    // une phrase ne dépasse pas 12 s
constexpr size_t kMinSamples = 11 * kRate / 10; // whisper refuse moins d'une seconde : on complète de silence
constexpr auto kAwakeWindow = std::chrono::seconds(8); // temps laissé pour dicter après « Aeria »

bool Debug() {
    static const bool on = std::getenv("AERIA_DEBUG") != nullptr;
    return on;
}

// ------------------------------------------------------------------ état partagé avec la page

std::mutex g_stateMutex;
std::string g_lastState; // dernier message d'état, renvoyé par « status »
std::atomic<bool> g_notch{false}; // l'encoche « j'écoute » du dock est ouverte

void Post(const char* state, int pct = -1, const std::string& msg = "", const std::string& text = "") {
    // L'encoche suit l'écoute : ouverte tant qu'on attend ou transcrit la commande, fermée sinon.
    const std::string name = state;
    const bool listening = name == "awake" || name == "working";
    if (g_notch.exchange(listening) != listening) platform::ShowListening(listening);
    std::string j = std::string("{\"t\":\"voice\",\"state\":\"") + state + "\"";
    if (pct >= 0) j += ",\"pct\":" + std::to_string(pct);
    if (!msg.empty()) j += ",\"msg\":" + util::Json(msg);
    if (!text.empty()) j += ",\"text\":" + util::Json(text);
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

    void Feed(const int16_t* s, size_t n, int endFrames) {
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
        const bool loud = rms > std::max(floor_ * 3.0, 250.0);
        if (!speaking_) {
            if (!loud) floor_ = std::clamp(0.98 * floor_ + 0.02 * rms, 30.0, 2000.0);
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
        if (voiced < kMinVoiced) return;
        // Niveau : les micros d'ordinateur portable sont souvent faibles, whisper préfère un signal franc.
        int peak = 1;
        for (int16_t v : utt) peak = std::max(peak, std::abs((int)v));
        const float gain = std::min(8.0f, 0.9f * 32768.0f / peak);
        std::vector<float> pcm(utt.size());
        for (size_t i = 0; i < utt.size(); i++) pcm[i] = std::clamp(utt[i] * gain / 32768.0f, -1.0f, 1.0f);
        emit_(std::move(pcm));
    }

    Emit emit_;
    int16_t frame_[kFrame]{};
    int fill_ = 0;
    std::deque<std::vector<int16_t>> pre_;
    std::vector<int16_t> utt_;
    bool speaking_ = false;
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

void QuietLog(ggml_log_level, const char*, void*) {}

// precise : une commande à comprendre (recherche en faisceau, plus lente) ; sinon on guette seulement « Aeria ».
std::string Transcribe(whisper_context* ctx, std::vector<float>& pcm, bool precise) {
    if (pcm.size() < kMinSamples) pcm.resize(kMinSamples, 0.0f);
    whisper_full_params p = whisper_full_default_params(precise ? WHISPER_SAMPLING_BEAM_SEARCH : WHISPER_SAMPLING_GREEDY);
    if (precise) p.beam_search.beam_size = 4;
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
    p.abort_callback = [](void*) { return g_stop.load(); };
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

void Enqueue(std::vector<float>&& pcm) {
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        if (g_queue.size() >= 3) g_queue.pop_front(); // le worker est en retard : on garde le plus récent
        g_queue.push_back(std::move(pcm));
    }
    g_queueCv.notify_one();
}

// Un texte à envoyer à l'assistante : la page le pose dans la conversation.
void Deliver(const std::string& text) {
    g_awake = false;
    platform::OpenDock(); // avant la fin de l'écoute : le panneau prend la place de l'encoche
    Post("heard", -1, "", text);
    Post("idle");
}

void Handle(const std::string& text, std::chrono::steady_clock::time_point& awakeUntil) {
    if (Debug()) std::fprintf(stderr, "[voice] « %s »%s\n", text.c_str(), g_awake ? " (écoute)" : "");
    if (IsNoise(text)) return;
    std::string rest;
    const bool named = FindWakeWord(text, rest);
    if (g_awake) { // « Aeria, mets un minuteur » dit pendant l'écoute : on retire le nom
        Deliver(named && !IsNoise(rest) ? rest : text);
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
    whisper_context* ctx = whisper_init_from_file_with_params(platform::ModelPath().c_str(), cp);
    if (!ctx) {
        Post("error", -1, "Modèle vocal illisible : télécharge-le de nouveau.");
        g_running = false;
        return;
    }

    long long zeroRun = 0; // échantillons nuls d'affilée : micro coupé par Windows ou muet
    bool zeroReported = false;
    Segmenter seg(Enqueue);
    std::string error;
    bool ok = platform::StartCapture(
        [&](const int16_t* s, size_t n) {
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
    Post("idle");

    auto awakeUntil = std::chrono::steady_clock::now();
    long long seenListen = g_listenRequest;
    while (!g_stop) {
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
        if (g_listenRequest != seenListen) {
            seenListen = g_listenRequest;
            g_awake = true;
            awakeUntil = std::chrono::steady_clock::now() + kAwakeWindow;
            Post("awake");
        }
        if (g_awake && std::chrono::steady_clock::now() > awakeUntil && pcm.empty()) {
            g_awake = false;
            Post("idle");
        }
        if (pcm.empty()) continue;
        if (g_awake) Post("working");
        std::string text = Transcribe(ctx, pcm, g_awake);
        std::string rest;
        // « Aeria, mets un minuteur… » d'une traite : le nom est trouvé, on refait la commande avec soin.
        if (!g_awake && FindWakeWord(text, rest) && !IsNoise(rest)) text = Transcribe(ctx, pcm, true);
        Handle(text, awakeUntil);
        if (g_awake) { // rien d'utilisable : on laisse le reste du délai
            awakeUntil = std::max(awakeUntil, std::chrono::steady_clock::now() + std::chrono::seconds(3));
            Post("awake");
        }
    }
    platform::StopCapture();
    whisper_free(ctx);
    g_awake = false;
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        g_queue.clear();
    }
    g_running = false;
}

bool ModelPresent() {
    std::error_code ec;
    return std::filesystem::exists(platform::ModelPath(), ec);
}

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

void DownloadModel() {
    if (g_downloading.exchange(true)) return;
    std::thread([] {
        const std::string dest = platform::ModelPath();
        std::string error;
        Post("downloading", 0);
        int last = -1;
        bool ok = platform::Download(kModelUrl, dest, kModelMaxBytes, kModelSha256,
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
        } else if (g_want) {
            StartLocked();
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
        StartLocked();
    } else {
        StopLocked();
        if (!g_downloading) Post("off");
    }
}

void Command(const std::string& arg) {
    if (arg == "status") {
        std::string state;
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            state = g_lastState;
        }
        if (g_downloading) return; // la progression arrive d'elle-même
        if (!g_running) state = std::string("{\"t\":\"voice\",\"state\":\"") + (ModelPresent() ? "off" : "nomodel") + "\"}";
        if (!state.empty()) bridge::PostToPage(std::move(state));
    } else if (arg == "download") {
        DownloadModel();
    } else if (arg == "listen") {
        if (g_running) {
            g_listenRequest++;
            g_queueCv.notify_all();
        }
    }
}

} // namespace voice
