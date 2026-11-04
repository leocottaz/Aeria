#include "features/pc.h"

#include "common/util.h"
#include "features/media.h"
#include "web/bridge.h"
#include "web/webview_host.h"

#include <windows.h>
#include <tlhelp32.h>
#include <commdlg.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <msdasc.h>
#include <oledb.h>
#include <shellapi.h>

#include <algorithm>
#include <cstddef>
#include <cwctype>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace pc {
namespace {

std::set<std::wstring> g_found; // chemins (en minuscules) déjà renvoyés à la page
std::mutex g_foundMutex;

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)std::towlower(c); });
    return s;
}

// Minuscules sans accents (« Été » -> « ete ») : pour comparer les noms de fichiers.
std::wstring Fold(const std::wstring& s) {
    std::wstring in = s;
    int n = FoldStringW(MAP_COMPOSITE, s.c_str(), (int)s.size(), nullptr, 0); // lettre + accent séparés
    if (n > 0) {
        std::wstring dec((size_t)n, L'\0');
        if (FoldStringW(MAP_COMPOSITE, s.c_str(), (int)s.size(), dec.data(), n) > 0) in = dec;
    }
    std::wstring out;
    for (wchar_t c : in)
        if (c < 0x300 || c > 0x36F) out += (wchar_t)std::towlower(c);
    return out;
}

constexpr size_t kMaxResults = 15;

struct Hit {
    std::wstring path;
    long long modified = 0, size = 0;
};

void Reply(const std::string& id, const std::string& fields) {
    bridge::PostToPage("{\"t\":\"reply\",\"id\":" + util::Json(id) + "," + fields + "}");
}

void ReplyError(const std::string& id, const std::string& message) {
    Reply(id, "\"ok\":false,\"error\":" + util::Json(message));
}

// ------------------------------------------------------------------ requête SQL

// Les mots viennent de Claude : on ne garde que lettres, chiffres, - et _ (rien qui puisse
// sortir des guillemets de la requête).
std::vector<std::wstring> Words(const std::wstring& text) {
    std::vector<std::wstring> out;
    std::wstring cur;
    auto flush = [&] {
        if (!cur.empty() && out.size() < 6) out.push_back(cur);
        cur.clear();
    };
    for (wchar_t c : text) {
        if (std::iswalnum(c) || c == L'-' || c == L'_') cur += c;
        else flush();
    }
    flush();
    return out;
}

bool ParseDate(const std::string& s, SYSTEMTIME& st) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
    for (int i : {0, 1, 2, 3, 5, 6, 8, 9})
        if (!std::isdigit((unsigned char)s[i])) return false;
    st = {};
    st.wYear = (WORD)std::atoi(s.substr(0, 4).c_str());
    st.wMonth = (WORD)std::atoi(s.substr(5, 2).c_str());
    st.wDay = (WORD)std::atoi(s.substr(8, 2).c_str());
    FILETIME ft;
    return SystemTimeToFileTime(&st, &ft); // refuse les dates impossibles
}

std::wstring DateText(const SYSTEMTIME& st) {
    wchar_t buf[16];
    swprintf(buf, 16, L"%04u-%02u-%02u", st.wYear, st.wMonth, st.wDay);
    return buf;
}

SYSTEMTIME NextDay(const SYSTEMTIME& st) {
    FILETIME ft;
    SystemTimeToFileTime(&st, &ft);
    ULARGE_INTEGER u{{ft.dwLowDateTime, ft.dwHighDateTime}};
    u.QuadPart += 864000000000ULL; // 24 h en unités de 100 ns
    ft = {u.LowPart, u.HighPart};
    SYSTEMTIME out;
    FileTimeToSystemTime(&ft, &out);
    return out;
}

// Extensions séparées par des virgules, des espaces ou des points : « pdf, docx » -> {pdf, docx}.
std::vector<std::wstring> Extensions(const std::string& text) {
    std::vector<std::wstring> out;
    std::wstring cur;
    auto flush = [&] {
        if (!cur.empty() && out.size() < 5 && std::find(out.begin(), out.end(), cur) == out.end()) out.push_back(cur);
        cur.clear();
    };
    for (char c : text) {
        if (std::isalnum((unsigned char)c) && cur.size() < 8) cur += (wchar_t)std::tolower((unsigned char)c);
        else flush();
    }
    flush();
    return out;
}

struct Query {
    std::vector<std::wstring> words, exts;
    bool hasFrom = false, hasTo = false;
    SYSTEMTIME from{}, to{};
    bool Empty() const { return words.empty() && exts.empty() && !hasFrom && !hasTo; }
};

// nameOnly : seulement le nom du fichier (sinon aussi le contenu) ; anyWord : un mot suffit (sinon tous).
std::wstring BuildQuery(const Query& q, bool nameOnly, bool anyWord, int top) {
    std::wstring where, words;
    for (const auto& w : q.words) {
        std::wstring c = L"CONTAINS(System.FileName, '\"" + w + L"*\"')";
        if (!nameOnly) c = L"(" + c + L" OR CONTAINS(*, '\"" + w + L"*\"'))";
        words += (words.empty() ? L"" : anyWord ? L" OR " : L" AND ") + c;
    }
    if (!words.empty()) where += L" AND (" + words + L")";
    if (!q.exts.empty()) {
        std::wstring e;
        for (const auto& x : q.exts) e += (e.empty() ? L"" : L" OR ") + std::wstring(L"System.FileExtension = '.") + x + L"'";
        where += L" AND (" + e + L")";
    }
    if (q.hasFrom) where += L" AND System.DateModified >= '" + DateText(q.from) + L"'";
    if (q.hasTo) where += L" AND System.DateModified < '" + DateText(NextDay(q.to)) + L"'";

    std::wstring scope = util::KnownFolder(FOLDERID_Profile);
    std::replace(scope.begin(), scope.end(), L'\\', L'/');
    size_t p = 0;
    while ((p = scope.find(L'\'', p)) != std::wstring::npos) scope.insert(p, 1, L'\''), p += 2;
    return L"SELECT TOP " + std::to_wstring(top) +
           L" System.ItemPathDisplay, System.DateModified, System.Size FROM SystemIndex"
           L" WHERE SCOPE='file:" + scope + L"' AND System.ItemType <> 'Directory'"
           L" AND NOT System.ItemPathDisplay LIKE '%\\AppData\\%'" + where +
           L" ORDER BY System.DateModified DESC";
}

// Nom du fichier d'abord (début de mot > ailleurs), contenu seul en dernier ; tous les mots = bonus.
int Score(const Hit& h, const std::vector<std::wstring>& foldedWords) {
    const wchar_t* slash = wcsrchr(h.path.c_str(), L'\\');
    std::wstring name = Fold(slash ? slash + 1 : h.path);
    int score = 0, inName = 0;
    for (const auto& w : foldedWords) {
        size_t at = name.find(w);
        if (at == std::wstring::npos) {
            score += 1; // trouvé dans le contenu
            continue;
        }
        ++inName;
        score += (at == 0 || !std::iswalnum(name[at - 1])) ? 15 : 10;
    }
    if (!foldedWords.empty() && inName == (int)foldedWords.size()) score += 20;
    return score;
}

// ------------------------------------------------------------------ OLE DB

// DBGUID_DEFAULT (dialecte SQL par défaut) : absent des bibliothèques de MinGW.
constexpr GUID kDialectDefault = {0xc8b521fb, 0x5cf3, 0x11ce, {0xad, 0xe5, 0x00, 0xaa, 0x00, 0x44, 0x77, 0x3d}};

template <class T>
struct Com {
    T* p = nullptr;
    ~Com() { if (p) p->Release(); }
    T** put() { return &p; }
    T* operator->() const { return p; }
};

struct Row {
    DBSTATUS status[3];
    VARIANT value[3];
};

long long VariantEpochMs(const VARIANT& v) {
    FILETIME ft{};
    if (v.vt == VT_FILETIME) { // pas un membre de VARIANT : les 8 octets sont dans ullVal
        ft = {(DWORD)(v.ullVal & 0xFFFFFFFF), (DWORD)(v.ullVal >> 32)};
    } else if (v.vt == VT_DATE) {
        SYSTEMTIME st;
        if (!VariantTimeToSystemTime(v.date, &st) || !SystemTimeToFileTime(&st, &ft)) return 0;
    } else {
        return 0;
    }
    ULARGE_INTEGER u{{ft.dwLowDateTime, ft.dwHighDateTime}};
    return u.QuadPart > 116444736000000000ULL ? (long long)((u.QuadPart - 116444736000000000ULL) / 10000) : 0;
}

long long VariantNumber(const VARIANT& v) {
    VARIANT tmp;
    VariantInit(&tmp);
    long long n = 0;
    if (SUCCEEDED(VariantChangeType(&tmp, const_cast<VARIANT*>(&v), 0, VT_R8))) n = (long long)tmp.dblVal;
    VariantClear(&tmp);
    return n;
}

// Exécute la requête ; remplit hits ou retourne un message d'erreur.
std::string RunQuery(const std::wstring& sql, std::vector<Hit>& hits) {
    Com<IDataInitialize> init;
    if (FAILED(CoCreateInstance(CLSID_MSDAINITIALIZE, nullptr, CLSCTX_INPROC_SERVER, IID_IDataInitialize,
                                (void**)init.put())))
        return "OLE DB est indisponible.";
    Com<IDBInitialize> db;
    wchar_t conn[] = L"Provider=Search.CollatorDSO;Extended Properties='Application=Windows';";
    if (FAILED(init->GetDataSource(nullptr, CLSCTX_INPROC_SERVER, conn, IID_IDBInitialize, (IUnknown**)db.put())) ||
        FAILED(db->Initialize()))
        return "La recherche Windows est indisponible (service Windows Search arrêté ?).";
    Com<IDBCreateSession> sessionFactory;
    Com<IDBCreateCommand> commandFactory;
    Com<ICommandText> command;
    Com<IRowset> rows;
    if (FAILED(db->QueryInterface(IID_IDBCreateSession, (void**)sessionFactory.put())) ||
        FAILED(sessionFactory->CreateSession(nullptr, IID_IDBCreateCommand, (IUnknown**)commandFactory.put())) ||
        FAILED(commandFactory->CreateCommand(nullptr, IID_ICommandText, (IUnknown**)command.put())) ||
        FAILED(command->SetCommandText(kDialectDefault, sql.c_str())) ||
        FAILED(command->Execute(nullptr, IID_IRowset, nullptr, nullptr, (IUnknown**)rows.put())) || !rows.p)
        return "La recherche a échoué.";

    DBBINDING bind[3]{};
    for (int i = 0; i < 3; ++i) {
        bind[i].iOrdinal = i + 1;
        bind[i].obValue = offsetof(Row, value) + i * sizeof(VARIANT);
        bind[i].obStatus = offsetof(Row, status) + i * sizeof(DBSTATUS);
        bind[i].dwPart = DBPART_VALUE | DBPART_STATUS;
        bind[i].dwMemOwner = DBMEMOWNER_CLIENTOWNED;
        bind[i].eParamIO = DBPARAMIO_NOTPARAM;
        bind[i].cbMaxLen = sizeof(VARIANT);
        bind[i].wType = DBTYPE_VARIANT;
    }
    Com<IAccessor> accessor;
    HACCESSOR hAcc = 0;
    if (FAILED(rows->QueryInterface(IID_IAccessor, (void**)accessor.put())) ||
        FAILED(accessor->CreateAccessor(DBACCESSOR_ROWDATA, 3, bind, sizeof(Row), &hAcc, nullptr)))
        return "Lecture des résultats impossible.";

    for (;;) {
        HROW handles[10];
        HROW* ph = handles;
        DBCOUNTITEM got = 0;
        HRESULT hr = rows->GetNextRows(DB_NULL_HCHAPTER, 0, 10, &got, &ph);
        for (DBCOUNTITEM i = 0; i < got; ++i) {
            Row r{};
            for (auto& v : r.value) VariantInit(&v);
            if (SUCCEEDED(rows->GetData(handles[i], hAcc, &r)) && r.status[0] == DBSTATUS_S_OK &&
                r.value[0].vt == VT_BSTR) {
                hits.push_back({r.value[0].bstrVal, VariantEpochMs(r.value[1]), VariantNumber(r.value[2])});
            }
            for (auto& v : r.value) VariantClear(&v);
        }
        if (got) rows->ReleaseRows(got, handles, nullptr, nullptr, nullptr);
        if (hr != S_OK || !got) break; // DB_S_ENDOFROWSET
    }
    accessor->ReleaseAccessor(hAcc, nullptr);
    return {};
}

// Nom du fichier d'abord, puis contenu ; sans résultat avec tous les mots, on essaie « un des mots ».
std::string SearchAll(const Query& q, std::vector<Hit>& out) {
    std::set<std::wstring> seen;
    auto run = [&](bool nameOnly, bool anyWord, int top) {
        std::vector<Hit> hits;
        std::string err = RunQuery(BuildQuery(q, nameOnly, anyWord, top), hits);
        for (auto& h : hits)
            if (seen.insert(Lower(h.path)).second) out.push_back(std::move(h));
        return err;
    };
    std::string err;
    if (!q.words.empty()) err = run(true, false, 30);
    if (err.empty() && out.size() < kMaxResults) err = run(false, false, 40);
    if (err.empty() && out.empty() && q.words.size() > 1) err = run(false, true, 40);
    if (!err.empty()) return err;

    std::vector<std::wstring> folded;
    for (const auto& w : q.words) folded.push_back(Fold(w));
    std::vector<std::pair<int, size_t>> order; // (score, indice)
    for (size_t i = 0; i < out.size(); ++i) order.push_back({Score(out[i], folded), i});
    std::stable_sort(order.begin(), order.end(), [&](const auto& a, const auto& b) {
        return a.first != b.first ? a.first > b.first : out[a.second].modified > out[b.second].modified;
    });
    std::vector<Hit> ranked;
    for (size_t i = 0; i < order.size() && i < kMaxResults; ++i) ranked.push_back(out[order[i].second]);
    out = std::move(ranked);
    return {};
}

// ------------------------------------------------------------------ ouverture

bool IsRunnable(const std::wstring& lowerPath) {
    static const wchar_t* kExt[] = {L".exe", L".com", L".bat", L".cmd", L".ps1", L".psm1", L".vbs", L".vbe",
                                    L".js",  L".jse", L".wsf", L".wsh", L".msi", L".msp",  L".scr", L".lnk",
                                    L".url", L".hta", L".cpl", L".jar", L".reg", L".pif",  L".appref-ms"};
    const wchar_t* dot = wcsrchr(lowerPath.c_str(), L'.');
    if (!dot) return false;
    for (auto e : kExt)
        if (!wcscmp(dot, e)) return true;
    return false;
}

} // namespace

void Search(const std::string& payload) {
    auto f = util::Split(payload, '\n', 5);
    if (f.empty() || f[0].empty()) return;
    f.resize(5);
    Query q;
    q.words = Words(util::Widen(f[1]));
    q.exts = Extensions(f[2]);
    q.hasFrom = ParseDate(f[3], q.from);
    q.hasTo = ParseDate(f[4], q.to);
    if (q.Empty()) {
        ReplyError(f[0], "Donne au moins un mot, une extension ou une date.");
        return;
    }
    std::thread([id = f[0], q] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::vector<Hit> hits;
        std::string err = SearchAll(q, hits);
        if (err.empty()) {
            std::string json = "[";
            for (size_t i = 0; i < hits.size(); ++i) {
                const wchar_t* slash = wcsrchr(hits[i].path.c_str(), L'\\');
                json += std::string(i ? "," : "") + "{\"path\":" + util::Json(util::Narrow(hits[i].path)) +
                        ",\"name\":" + util::Json(util::Narrow(slash ? slash + 1 : hits[i].path.c_str())) +
                        ",\"modified\":" + std::to_string(hits[i].modified) +
                        ",\"size\":" + std::to_string(hits[i].size) + "}";
            }
            json += "]";
            {
                std::lock_guard lock(g_foundMutex);
                if (g_found.size() > 2000) g_found.clear();
                for (const auto& h : hits) g_found.insert(Lower(h.path));
            }
            Reply(id, "\"ok\":true,\"results\":" + json);
        } else {
            ReplyError(id, err);
        }
        CoUninitialize();
    }).detach();
}

void OpenFound(const std::string& path) {
    std::wstring p = util::Widen(path), low = Lower(p);
    {
        std::lock_guard lock(g_foundMutex);
        if (!g_found.count(low)) return;
    }
    if (IsRunnable(low)) {
        std::wstring args = L"/select,\"" + p + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    } else {
        ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void RunRoutine(const std::string& steps) {
    std::thread([lines = util::Split(steps, '\n')] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); // ShellExecute et le volume utilisent COM
        for (const auto& line : lines) {
            auto f = util::Split(line, '\t', 2);
            if (f.size() != 2) continue;
            const std::string &type = f[0], &value = f[1];
            if (type == "open" && !value.empty()) {
                ShellExecuteW(nullptr, L"open", util::Widen(value).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            } else if (type == "volume") {
                SetVolume(std::atoi(value.c_str()));
            } else if (type == "music") {
                bool playing = media::IsPlaying();
                if ((value == "play" && !playing) || (value == "pause" && playing)) media::Command("toggle");
                else if (value == "next") media::Command("next");
                else if (value == "previous") media::Command("prev");
            } else if (type == "mute") {
                SetMute(value);
            } else if (type == "lock") {
                LockWorkStation();
            } else if (type == "wait") {
                Sleep((DWORD)std::clamp(std::atoi(value.c_str()), 0, 60) * 1000);
            }
        }
        CoUninitialize();
    }).detach();
}

// ------------------------------------------------------------------ applis qui déclenchent un mode

static std::set<std::wstring> g_watch;   // noms d'exécutable surveillés, en minuscules (thread de l'interface seulement)
static std::set<std::wstring> g_running; // parmi eux, ceux qui tournaient au dernier relevé

static std::set<std::wstring> RunningWatched() {
    std::set<std::wstring> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W e{};
    e.dwSize = sizeof e;
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e)) {
        std::wstring name = Lower(e.szExeFile);
        if (g_watch.count(name)) out.insert(name);
    }
    CloseHandle(snap);
    return out;
}

void WatchApps(const std::string& names) {
    g_watch.clear();
    for (const auto& line : util::Split(names, '\n')) {
        std::wstring n = Lower(util::Widen(line));
        while (!n.empty() && (n.back() == L'\r' || n.back() == L' ')) n.pop_back();
        if (n.empty() || n.size() > 128 || g_watch.size() >= 64) continue;
        if (n.find_first_of(L"\\/:") != std::wstring::npos) continue; // un nom seulement, jamais un chemin
        if (n.find(L'.') == std::wstring::npos) n += L".exe";
        g_watch.insert(n);
    }
    g_running = RunningWatched(); // ce qui tourne déjà ne déclenche rien
}

void CheckApps() {
    if (g_watch.empty()) return;
    std::set<std::wstring> now = RunningWatched();
    for (const auto& n : now) {
        if (g_running.count(n)) continue;
        webview::Wake(); // dock rangé : la page dort, il faut qu'elle puisse démarrer le mode
        bridge::PostToPage("{\"t\":\"appstart\",\"app\":" + util::Json(util::Narrow(n)) + "}");
    }
    g_running = std::move(now);
}

static bool DefaultEndpointVolume(Com<IAudioEndpointVolume>& volume) {
    Com<IMMDeviceEnumerator> devices;
    Com<IMMDevice> device;
    return SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                      (void**)devices.put())) &&
           SUCCEEDED(devices->GetDefaultAudioEndpoint(eRender, eConsole, device.put())) &&
           SUCCEEDED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)volume.put()));
}

void SetVolume(int percent) {
    percent = std::clamp(percent, 0, 100);
    Com<IAudioEndpointVolume> volume;
    if (!DefaultEndpointVolume(volume)) return;
    volume->SetMasterVolumeLevelScalar(percent / 100.0f, nullptr);
    volume->SetMute(percent == 0, nullptr);
}

void SetMute(const std::string& mode) {
    Com<IAudioEndpointVolume> volume;
    if (!DefaultEndpointVolume(volume)) return;
    BOOL muted = FALSE;
    if (mode == "toggle") volume->GetMute(&muted), muted = !muted;
    else muted = mode != "off";
    volume->SetMute(muted, nullptr);
}

void PickFile(const std::string& id, HWND owner) {
    // Fenêtre modale : dans son propre thread, pour ne pas bloquer l'évènement WebView2 en cours.
    std::thread([id, owner] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        wchar_t file[MAX_PATH] = L"";
        OPENFILENAMEW ofn{sizeof ofn};
        ofn.hwndOwner = owner;
        ofn.lpstrFilter = L"Applications et raccourcis\0*.exe;*.lnk;*.url\0Tous les fichiers\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrTitle = L"Choisir une application ou un fichier";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_NODEREFERENCELINKS | OFN_NOCHANGEDIR;
        bool ok = GetOpenFileNameW(&ofn);
        Reply(id, std::string("\"ok\":true,\"path\":") + util::Json(ok ? util::Narrow(file) : ""));
        CoUninitialize();
    }).detach();
}

} // namespace pc
