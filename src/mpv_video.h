#pragma once
//
// Video, by way of libmpv.
//
// mpv is loaded at run time out of the folder next to the executable, by name,
// through GetProcAddress. That keeps the build free of any mpv headers or import
// libraries: the C API is small and stable, and declaring the dozen entry points
// this player uses is less trouble than carrying a copy of somebody's SDK.
//
// mpv draws the picture and decodes nothing else. Its own audio is switched off
// - the sound comes from AudioEngine, which is also what the clock is. Video
// follows that clock, which is the way round every working player has it: a
// repeated or dropped frame is invisible, and audio that is stretched to catch
// up is not.

#ifndef NOMINMAX
#define NOMINMAX   // windows.h defines min and max as macros
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>

namespace vp {

class AudioEngine;

class MpvVideo {
public:
    MpvVideo();
    ~MpvVideo();

    MpvVideo(const MpvVideo&) = delete;
    MpvVideo& operator=(const MpvVideo&) = delete;

    // `host` is the window the picture is drawn into. Fails only when the
    // library is missing or refuses to start.
    bool start(HWND host, std::string* error);
    bool ready() const { return handle_ != nullptr; }

    void loadFile(const std::wstring& path);
    void setPaused(bool paused);
    void seek(double seconds);
    void frameStep(bool backwards);

    // The speed the listener asked for. Kept apart from the small corrections
    // this class makes on its own, which are applied on top of it - otherwise
    // every correction would wipe out the setting a moment after it was made.
    void setBaseSpeed(double speed);
    double baseSpeed() const { return baseSpeed_; }

    // mpv's own on-screen text. The picture is a child window that mpv repaints
    // continuously, so anything this program drew over it would be wiped within
    // a frame; the message has to come from whoever owns those pixels.
    void showText(const std::string& utf8, int milliseconds);

    // Called a few times a second. Nudges the picture towards `audioSeconds`,
    // which is where the sound actually is.
    void followAudioClock(double audioSeconds, bool playing);

    double videoPosition();
    double duration();
    int width();
    int height();
    double fps();
    std::string hwdec();

    void setProperty(const char* name, const std::string& value);
    // The same thing, but without waiting for the core to have done it. Panning
    // sends one of these per mouse move, and waiting for each made the picture
    // lag a long way behind the cursor.
    void setPropertyFast(const char* name, const std::string& value);
    // Throws away whatever the core has queued up for us. Nothing here reads
    // mpv's events, but the replies to the asynchronous property sets still
    // pile up in that queue if nobody ever takes them out.
    void drainEvents();
    // What mpv says the property is now, which is not always what it was
    // told: an option it refused keeps its old value, silently.
    std::string property(const char* name) { return stringProperty(name); }
    void command(const char* const* args);

private:
    double numberProperty(const char* name);
    std::string stringProperty(const char* name);

    HMODULE library_ = nullptr;
    void* handle_ = nullptr;

    // The last correction, so the picture is not shoved twice for the same
    // difference before it has had time to answer.
    double lastCorrection_ = 0.0;
    double speed_ = 1.0;
    double baseSpeed_ = 1.0;
};

}  // namespace vp
