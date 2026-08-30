// A console harness for the audio engine alone.
//
// Exists so the engine can be judged before any window is built on top of it:
// it opens a file, plays it, and prints the clock, the dropout count and the
// per-track peaks once a second. Silent playback is available so it can be run
// without anyone having to listen to it.

#include "audio_engine.h"

#ifndef NOMINMAX
#define NOMINMAX   // windows.h defines min and max as macros
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>   // WIN32_LEAN_AND_MEAN leaves COM out
#include <cstdio>
#include <string>
#include <vector>
#include <mutex>

namespace {
std::mutex g_dumpLock;
std::vector<float> g_dump;
void collect(const float* block, long long frames, void*) {
    std::lock_guard<std::mutex> lock(g_dumpLock);
    g_dump.insert(g_dump.end(), block, block + frames * 2);
}
void writeWav(const wchar_t* path, const std::vector<float>& x) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") != 0 || !f) return;
    const int sr = 48000, ch = 2;
    const unsigned bytes = static_cast<unsigned>(x.size() * 2);
    unsigned riff = 36 + bytes;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    unsigned sz = 16; unsigned short fmt = 1, nch = ch, bits = 16;
    unsigned rate = sr, brate = sr * ch * 2; unsigned short align = ch * 2;
    fwrite(&sz,4,1,f); fwrite(&fmt,2,1,f); fwrite(&nch,2,1,f); fwrite(&rate,4,1,f);
    fwrite(&brate,4,1,f); fwrite(&align,2,1,f); fwrite(&bits,2,1,f);
    fwrite("data",1,4,f); fwrite(&bytes,4,1,f);
    for (float v : x) {
        if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
        short s = static_cast<short>(v * 32767.0f);
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::printf("usage: audio_probe <file> [seconds] [--silent]\n");
        return 2;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    double seconds = 15.0;
    double startAt = 0.0;
    bool silent = false;
    int only = -1;                       // 1-based track, -1 = all of them
    for (int i = 2; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--silent") { silent = true; }
        else if (a.rfind(L"--track=", 0) == 0) { only = _wtoi(a.c_str() + 8); }
        else if (a.rfind(L"--start=", 0) == 0) { startAt = _wtof(a.c_str() + 8); }
        else if (a.rfind(L"--", 0) != 0) seconds = _wtof(argv[i]);
    }

    vp::AudioEngine engine;
    std::string error;
    if (!engine.open(argv[1], &error)) {
        std::printf("open failed: %s\n", error.c_str());
        return 1;
    }

    std::printf("output   : %s\n", engine.outputDescription().c_str());
    std::printf("latency  : %.1f ms\n", engine.outputLatency() * 1000.0);
    std::printf("duration : %.1f s\n", engine.duration());
    std::printf("tracks   : %zu\n", engine.tracks().size());
    for (const auto& t : engine.tracks()) {
        std::printf("   stream %d  %d Hz  %d ch  %s\n", t.index, t.sourceRate,
                    t.sourceChannels, t.name.c_str());
    }

    const wchar_t* dumpPath = nullptr;
    for (int i = 2; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a.rfind(L"--dump=", 0) == 0) dumpPath = argv[i] + 7;
    }
    if (dumpPath) engine.setMixTap(&collect, nullptr);

    for (const auto& n : engine.decodeDescriptions()) {
        std::printf("   %s\n", n.c_str());
    }

    if (silent) engine.setMasterGain(0.0f);
    if (only > 0) {
        for (size_t i = 0; i < engine.tracks().size(); ++i) {
            engine.controls(i).enabled.store(static_cast<int>(i) == only - 1);
        }
        std::printf("playing track %d only\n", only);
    }
    if (startAt > 0.0) { engine.seek(startAt); Sleep(300); }
    engine.play();

    const DWORD start = GetTickCount();
    while ((GetTickCount() - start) < static_cast<DWORD>(seconds * 1000)) {
        Sleep(1000);
        std::printf("t=%6.2f  dropouts=%llu  peak=%.3f\n", engine.position(),
                    static_cast<unsigned long long>(engine.dropouts()),
                    engine.tracks().empty() ? 0.0f
                        : engine.controls(0).peak.load());
        std::fflush(stdout);
    }

    std::printf("FINAL dropouts: %llu\n",
                static_cast<unsigned long long>(engine.dropouts()));
    std::printf("%s\n", engine.continuityReport().c_str());
    if (dumpPath) {
        std::lock_guard<std::mutex> lock(g_dumpLock);
        writeWav(dumpPath, g_dump);
        std::printf("wrote %zu frames straight from the mixer\n", g_dump.size() / 2);
    }
    engine.close();
    CoUninitialize();
    return 0;
}
