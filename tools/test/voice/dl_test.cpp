// Banc d'essai du téléchargement du modèle (Linux : libcurl + SHA-256 maison). Télécharge pour de bon
// le modèle dans le dossier donné et affiche le résultat. Usage : dl_test <dossier>
#include "features/voice_platform.h"

#include <cstdio>

namespace dock { void OpenPanelAsync() {} } // faux dock : le banc n'ouvre rien

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    std::string error;
    int last = -10;
    bool ok = voice::platform::Download(
        "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small-q5_1.bin", std::string(argv[1]) + "/m.bin",
        260ull * 1024 * 1024, "ae85e4a935d7a567bd102fe55afc16bb595bdb618e11b2fc7591bc08120411bb",
        [&](int pct) { if (pct >= last + 20) { last = pct; std::printf("%d%% ", pct); std::fflush(stdout); } }, error);
    std::printf("\nok=%d erreur=%s\n", ok, error.c_str());
    // Mauvaise empreinte : le fichier doit être refusé et supprimé.
    ok = voice::platform::Download(
        "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small-q5_1.bin", std::string(argv[1]) + "/bad.bin",
        260ull * 1024 * 1024, std::string(64, '0'), [](int) {}, error);
    std::printf("mauvaise empreinte : ok=%d erreur=%s\n", ok, error.c_str());
    return 0;
}
