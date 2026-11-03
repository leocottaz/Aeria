// Écoute le vrai micro avec le vrai code (voice.cpp + voice_win.cpp) pendant N secondes et affiche ce
// qui est compris (AERIA_DEBUG=1 pour les transcriptions). Usage : live_test <secondes>
#include "features/voice.h"
#include "web/bridge.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>

namespace dock { void OpenPanelAsync() { std::printf("  [dock ouvert]\n"); } }
namespace bridge { void PostToPage(std::string json) { std::printf("  -> %s\n", json.c_str()); } }

int main(int argc, char** argv) {
    voice::Apply(true);
    Sleep((argc > 1 ? std::atoi(argv[1]) : 10) * 1000);
    voice::Apply(false);
}
