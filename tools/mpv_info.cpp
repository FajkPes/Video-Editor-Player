#include <windows.h>

#include <iostream>

using Create = void* (*)();
using Init = int (*)(void*);
using GetPropertyString = char* (*)(void*, const char*);
using Free = void (*)(void*);
using Destroy = void (*)(void*);

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "libmpv-2.dll";
    HMODULE module = LoadLibraryA(path);
    if (!module) return 1;
    auto create = reinterpret_cast<Create>(GetProcAddress(module, "mpv_create"));
    auto init = reinterpret_cast<Init>(GetProcAddress(module, "mpv_initialize"));
    auto get = reinterpret_cast<GetPropertyString>(GetProcAddress(module, "mpv_get_property_string"));
    auto freeValue = reinterpret_cast<Free>(GetProcAddress(module, "mpv_free"));
    auto destroy = reinterpret_cast<Destroy>(GetProcAddress(module, "mpv_terminate_destroy"));
    if (!create || !init || !get || !freeValue || !destroy) return 2;
    void* context = create();
    if (!context || init(context) < 0) return 3;
    for (const char* name : {"mpv-version", "mpv-configuration", "ffmpeg-version"}) {
        char* value = get(context, name);
        std::cout << name << ": " << (value ? value : "unavailable") << '\n';
        if (value) freeValue(value);
    }
    destroy(context);
    FreeLibrary(module);
    return 0;
}

// Created by FajkPes with assistance from ChatGPT Codex and Claude Code.
