// Vérifie le micro réel avec le vrai code Windows (voice_win.cpp) : niveau crête par seconde pendant 8 s.
#include "features/voice_platform.h"

#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace dock { void SetListeningAsync(bool) {} }

int main() {
    std::atomic<int> peak{0};
    std::string error;
    if (!voice::platform::StartCapture([&](const int16_t* s, size_t n) {
            for (size_t i = 0; i < n; i++) peak = std::max(peak.load(), std::abs((int)s[i]));
        }, error)) {
        std::printf("micro: %s\n", error.c_str());
        return 1;
    }
    for (int i = 0; i < 8; i++) {
        Sleep(1000);
        std::printf("s%d crête=%d\n", i, peak.exchange(0));
    }
    voice::platform::StopCapture();
}
