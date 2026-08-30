#pragma once
// What the program remembers between runs.
//
// One file beside the executable, so the whole thing stays a folder that can be
// copied to another machine. Three kinds of thing live in it: preferences,
// named presets, and the position each file was last left at.

#include <map>
#include <string>
#include <vector>

namespace vp {

// A preset for the mixer. Track settings are positional - preset slot 1 goes to
// track 1 - because that is what a person means when they save "commentary
// quiet, game loud" on a folder full of recordings made the same way.
struct AudioPresetTrack {
    float gain = 1.0f;
    float eqLow = 0.0f, eqMid = 0.0f, eqHigh = 0.0f;
    bool enabled = true;
    bool normalise = false;     // whether the leveller may touch this track
    float pitch = 0.0f;         // semitones, this track only
};

struct AudioPreset {
    std::string name;
    bool normalise = false;
    float target = -14.0f;
    std::vector<AudioPresetTrack> tracks;
};

struct VideoPreset {
    std::string name;
    int contrast = 0, brightness = 0, saturation = 0, gamma = 0, hue = 0;
    int sharpen = 0, denoise = 0;
};

struct RenderPreset {
    std::string name;
    int encoder = 0;          // index into the encoder table
    int bitrate = 30000;      // kbps
    bool useQuality = false;
    int quality = 23;         // CQ or CRF
    int fps = 0;              // 0 keeps the source rate
    int speedPreset = 3;      // p1..p7 or ultrafast..veryslow
    int audioBitrate = 192;   // kbps
    double speed = 1.0;
    bool keepPitch = true;
    bool hardwareDecode = true;
    bool halfSize = false;

    // What shape and size the file comes out. 0 leaves the picture as the
    // player is showing it, 1 forces a landscape frame, 2 a portrait one. A
    // width of 0 keeps the source's own; otherwise the picture is fitted into
    // that frame and the rest is black, so nothing is ever quietly cut.
    int orientation = 0;
    int outWidth = 0;
    int outHeight = 0;
};

struct Settings {
    // ---- preferences
    float volume = 1.0f;
    bool normalise = false;
    float normaliseTarget = -14.0f;
    bool editorByDefault = false;
    // "editor" builds waveforms and thumbnails when the editor first opens,
    // "manual" waits to be asked, "off" never does. Both cost disk and a couple
    // of cores, which is worth a choice rather than an assumption.
    std::string waveMode = "editor";
    std::string thumbMode = "editor";
    bool rememberPosition = true;
    bool startFullscreen = false;
    bool alwaysOnTop = false;
    bool keepPitch = true;          // speed changes keep the pitch
    int volumeStep = 5;             // per arrow key press, in per cent
    // How often the panels redraw. The picture is mpv's and runs at its own
    // rate whatever this says; this is the meters, the playhead and the strip.
    int uiFps = 30;

    int seekSmall = 10;             // seconds, the plain arrow keys
    int seekLarge = 60;             // seconds, with shift held
    double avOffset = 0.0;          // sound against picture, in seconds
    int fitMode = 0;
    int contrast = 0, brightness = 0, saturation = 0, gamma = 0, hue = 0;
    int sharpen = 0, denoise = 0;
    std::string lastFolder;
    // Which lang\*.lang file to read. "en" is the built-in English and the
    // default, so a fresh copy of the program is in English for everybody.
    std::string language = "en";

    // ---- presets
    std::vector<AudioPreset> audioPresets;
    std::vector<VideoPreset> videoPresets;
    std::vector<RenderPreset> renderPresets;

    // ---- where each file was left
    std::map<std::string, double> positions;   // utf-8 path -> seconds

    void load();
    void save() const;

    void rememberPositionFor(const std::wstring& path, double seconds);
    double positionFor(const std::wstring& path) const;

    static std::wstring fileLocation();
};

// One instance, because there is one settings file and passing it through every
// layer of a single-window program buys nothing.
Settings& settings();

std::string toUtf8(const std::wstring& s);
std::wstring fromUtf8(const std::string& s);

}  // namespace vp
