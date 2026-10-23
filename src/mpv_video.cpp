#include "mpv_video.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace vp {
namespace {

// The slice of the mpv C API this player uses. Declared here rather than
// included, so the build needs nothing but the DLL that already sits beside the
// executable. These signatures are part of mpv's public, versioned interface.
typedef void* (*fn_create)(void);
typedef int (*fn_initialize)(void*);
typedef void (*fn_terminate)(void*);
typedef int (*fn_set_option_string)(void*, const char*, const char*);
typedef int (*fn_set_property_string)(void*, const char*, const char*);
typedef int (*fn_set_property_async)(void*, unsigned long long, const char*, int, void*);
typedef void* (*fn_wait_event)(void*, double);
typedef int (*fn_get_property_string_ret)(void*, const char*, int, void*);
typedef int (*fn_command)(void*, const char* const*);
typedef void (*fn_free)(void*);

fn_create mpv_create_ = nullptr;
fn_initialize mpv_initialize_ = nullptr;
fn_terminate mpv_terminate_ = nullptr;
fn_set_option_string mpv_set_option_string_ = nullptr;
fn_set_property_string mpv_set_property_string_ = nullptr;
fn_set_property_async mpv_set_property_async_ = nullptr;
fn_wait_event mpv_wait_event_ = nullptr;
fn_get_property_string_ret mpv_get_property_ = nullptr;
fn_command mpv_command_ = nullptr;
fn_free mpv_free_ = nullptr;

constexpr int kFormatString = 1;   // MPV_FORMAT_STRING
constexpr int kFormatDouble = 5;   // MPV_FORMAT_DOUBLE

std::string narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int need = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1,
                                         nullptr, 0, nullptr, nullptr);
    if (need <= 1) return {};
    std::string out(static_cast<size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, &out[0], need, nullptr, nullptr);
    out.resize(static_cast<size_t>(need - 1));
    return out;
}

}  // namespace

MpvVideo::MpvVideo() = default;

MpvVideo::~MpvVideo() {
    if (handle_ && mpv_terminate_) mpv_terminate_(handle_);
    handle_ = nullptr;
    if (library_) FreeLibrary(library_);
}

bool MpvVideo::start(HWND host, std::string* error) {
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dir(exePath);
    const size_t slash = dir.find_last_of(L"\\/");
    dir = (slash == std::wstring::npos) ? L"." : dir.substr(0, slash);

    // Beside the executable first, then wherever Windows can find it.
    const std::wstring beside = dir + L"\\libmpv-2.dll";
    library_ = LoadLibraryW(beside.c_str());
    if (!library_) library_ = LoadLibraryW(L"libmpv-2.dll");
    if (!library_) {
        if (error) {
            *error = "libmpv-2.dll was not found. Copy it next to the player, "
                     "the same file the old version used.";
        }
        return false;
    }

    mpv_create_ = reinterpret_cast<fn_create>(GetProcAddress(library_, "mpv_create"));
    mpv_initialize_ = reinterpret_cast<fn_initialize>(GetProcAddress(library_, "mpv_initialize"));
    mpv_terminate_ = reinterpret_cast<fn_terminate>(GetProcAddress(library_, "mpv_terminate_destroy"));
    mpv_set_option_string_ = reinterpret_cast<fn_set_option_string>(GetProcAddress(library_, "mpv_set_option_string"));
    mpv_set_property_string_ = reinterpret_cast<fn_set_property_string>(GetProcAddress(library_, "mpv_set_property_string"));
    mpv_set_property_async_ = reinterpret_cast<fn_set_property_async>(GetProcAddress(library_, "mpv_set_property_async"));
    mpv_wait_event_ = reinterpret_cast<fn_wait_event>(GetProcAddress(library_, "mpv_wait_event"));
    mpv_get_property_ = reinterpret_cast<fn_get_property_string_ret>(GetProcAddress(library_, "mpv_get_property"));
    mpv_command_ = reinterpret_cast<fn_command>(GetProcAddress(library_, "mpv_command"));
    mpv_free_ = reinterpret_cast<fn_free>(GetProcAddress(library_, "mpv_free"));

    if (!mpv_create_ || !mpv_initialize_ || !mpv_terminate_ ||
        !mpv_set_option_string_ || !mpv_set_property_string_ || !mpv_get_property_ ||
        !mpv_free_ || !mpv_command_) {
        if (error) *error = "libmpv-2.dll is not the library this player expects";
        return false;
    }

    handle_ = mpv_create_();
    if (!handle_) {
        if (error) *error = "mpv would not start";
        return false;
    }

    // The picture goes into our own child window. Everything about sound is
    // switched off: this player has its own engine and its own clock.
    char wid[32];
    _snprintf_s(wid, sizeof(wid), _TRUNCATE, "%lld",
                static_cast<long long>(reinterpret_cast<intptr_t>(host)));
    mpv_set_option_string_(handle_, "wid", wid);
    mpv_set_option_string_(handle_, "audio", "no");
    mpv_set_option_string_(handle_, "vo", "gpu-next");
    mpv_set_option_string_(handle_, "gpu-api", "d3d11");
    mpv_set_option_string_(handle_, "hwdec", "auto");
    mpv_set_option_string_(handle_, "keep-open", "yes");
    mpv_set_option_string_(handle_, "osc", "no");
    mpv_set_option_string_(handle_, "input-default-bindings", "no");
    mpv_set_option_string_(handle_, "input-vo-keyboard", "no");
    mpv_set_option_string_(handle_, "auto-window-resize", "no");
    mpv_set_option_string_(handle_, "keepaspect-window", "no");
    mpv_set_option_string_(handle_, "idle", "yes");

    // Not display-resample. In the display-locked modes mpv changes the speed
    // of playback to match the monitor and expects to fix the audio up by
    // resampling it, which it cannot do here because it has no audio. The
    // picture follows the sound in this player, so mpv is left timing against
    // an ordinary clock and corrected from outside.
    mpv_set_option_string_(handle_, "video-sync", "audio");
    mpv_set_option_string_(handle_, "framedrop", "vo");

    if (mpv_initialize_(handle_) < 0) {
        if (error) *error = "mpv refused to initialise";
        mpv_terminate_(handle_);
        handle_ = nullptr;
        return false;
    }
    return true;
}

void MpvVideo::loadFile(const std::wstring& path) {
    if (!handle_) return;
    const std::string utf8 = narrow(path);
    const char* args[] = {"loadfile", utf8.c_str(), "replace", nullptr};
    mpv_command_(handle_, args);
    lastCorrection_ = 0.0;
    speed_ = 1.0;
}

void MpvVideo::setPaused(bool paused) {
    setProperty("pause", paused ? "yes" : "no");
}

void MpvVideo::seek(double seconds) {
    if (!handle_) return;
    char at[32];
    _snprintf_s(at, sizeof(at), _TRUNCATE, "%.3f", seconds);
    const char* args[] = {"seek", at, "absolute+exact", nullptr};
    mpv_command_(handle_, args);
    lastCorrection_ = 0.0;
}

void MpvVideo::frameStep(bool backwards) {
    if (!handle_) return;
    const char* args[] = {backwards ? "frame-back-step" : "frame-step", nullptr};
    mpv_command_(handle_, args);
}

void MpvVideo::setBaseSpeed(double speed) {
    baseSpeed_ = speed;
    speed_ = speed;
    char text[32];
    _snprintf_s(text, sizeof(text), _TRUNCATE, "%.4f", speed);
    setProperty("speed", text);
}

void MpvVideo::showText(const std::string& utf8, int milliseconds) {
    if (!handle_) return;
    char duration[16];
    _snprintf_s(duration, sizeof(duration), _TRUNCATE, "%d", milliseconds);
    const char* args[] = {"show-text", utf8.c_str(), duration, nullptr};
    mpv_command_(handle_, args);
}

void MpvVideo::command(const char* const* args) {
    if (!handle_) return;
    mpv_command_(handle_, args);
}

void MpvVideo::setProperty(const char* name, const std::string& value) {
    if (handle_ && mpv_set_property_string_) {
        mpv_set_property_string_(handle_, name, value.c_str());
    }
}

void MpvVideo::drainEvents() {
    if (!handle_ || !mpv_wait_event_) return;
    // The first field of an mpv_event is its id, and id 0 means the queue is
    // empty. That is all this needs to know about the structure.
    for (int guard = 0; guard < 64; ++guard) {
        const int* event = static_cast<const int*>(mpv_wait_event_(handle_, 0.0));
        if (!event || *event == 0) return;
    }
}

void MpvVideo::setPropertyFast(const char* name, const std::string& value) {
    if (!handle_) return;
    if (mpv_set_property_async_) {
        // MPV_FORMAT_STRING is 1, and it takes the address of the pointer.
        const char* text = value.c_str();
        mpv_set_property_async_(handle_, 0, name, 1, &text);
        return;
    }
    setProperty(name, value);
}

double MpvVideo::numberProperty(const char* name) {
    if (!handle_ || !mpv_get_property_) return 0.0;
    double value = 0.0;
    if (mpv_get_property_(handle_, name, kFormatDouble, &value) < 0) return 0.0;
    return value;
}

std::string MpvVideo::stringProperty(const char* name) {
    if (!handle_ || !mpv_get_property_) return {};
    char* value = nullptr;
    if (mpv_get_property_(handle_, name, kFormatString, &value) < 0 || !value) return {};
    std::string out(value);
    if (mpv_free_) mpv_free_(value);
    return out;
}

double MpvVideo::videoPosition() { return numberProperty("time-pos"); }
double MpvVideo::duration() { return numberProperty("duration"); }
int MpvVideo::width() { return static_cast<int>(numberProperty("width")); }
int MpvVideo::height() { return static_cast<int>(numberProperty("height")); }
int MpvVideo::displayWidth() {
    const int w = static_cast<int>(numberProperty("video-out-params/dw"));
    return w > 0 ? w : width();
}
int MpvVideo::displayHeight() {
    const int h = static_cast<int>(numberProperty("video-out-params/dh"));
    return h > 0 ? h : height();
}
double MpvVideo::fps() { return numberProperty("container-fps"); }
std::string MpvVideo::hwdec() { return stringProperty("hwdec-current"); }

void MpvVideo::followAudioClock(double audioSeconds, bool playing) {
    if (!handle_ || !playing) return;

    const double video = videoPosition();
    if (video <= 0.0 && audioSeconds <= 0.0) return;
    const double behind = audioSeconds - video;   // positive: the picture is late

    // A big difference is a jump, and a jump is corrected by jumping. Anything
    // else would take minutes of creeping to close, during which the two are
    // visibly apart.
    if (std::fabs(behind) > 0.35) {
        seek(audioSeconds);
        return;
    }

    // Otherwise the picture is asked to run a fraction faster or slower until
    // it lines up. Two per cent is far below what an eye notices in motion, and
    // it costs the sound nothing at all, because the sound is not involved.
    const double wanted =
        baseSpeed_ * (1.0 + std::max(-0.02, std::min(0.02, behind * 0.25)));
    if (std::fabs(wanted - speed_) > 0.0005) {
        speed_ = wanted;
        char text[32];
        _snprintf_s(text, sizeof(text), _TRUNCATE, "%.4f", speed_);
        setProperty("speed", text);
    }
}

}  // namespace vp
