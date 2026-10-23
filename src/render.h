#pragma once
// Export: the cuts, the picture adjustments and the mix, written to one file.
//
// The work is ffmpeg's. What lives here is the filter graph that describes the
// edit, a window to choose the encoder in, and a progress reader - ffmpeg is
// asked for machine-readable progress on its own pipe rather than having its
// human output scraped, which is both simpler and does not break when the
// wording changes.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "settings.h"
#include "timeline.h"

namespace vp {

// What a single sound track contributes to the export. Separate from the live
// mixer controls: a person routinely wants the commentary quieter in the file
// than it is while editing, and wants the choice remembered per render.
struct RenderTrack {
    std::string name;
    bool include = true;
    bool normalise = false;
    float target = -14.0f;
    float measured = -70.0f;    // -70 when nothing has measured it yet
    float volume = 1.0f;
    float eqLow = 0.0f, eqMid = 0.0f, eqHigh = 0.0f;
    float pitch = 0.0f;         // semitones, this track only
    int sourceRate = 48000;
    int sourceIndex = 0;        // which audio stream of the file this is
    // Which track the editor knows this as. The per-piece settings and the
    // per-piece mutes are kept under that number, and it is not the same as
    // the number of the stream in the file.
    int editIndex = 0;
};

// Everything the graph needs that is not a preference.
struct RenderJob {
    std::wstring source;
    double duration = 0.0;
    std::vector<Segment> segments;      // kept stretches, in order
    std::vector<RowFades> fades;        // row 0 picture, then one per track
    double fadeStart = 0.0, fadeEnd = -1.0; // source time, before cuts and speed
    std::vector<RenderTrack> tracks;
    int contrast = 0, brightness = 0, saturation = 0, gamma = 0, hue = 0;
    int sharpen = 0, denoise = 0;

    // How the picture was being watched. An export that quietly threw the
    // rotation away, or the crop, or the speed, was an export of a different
    // film from the one on the screen - so all of it travels with the job and
    // the window decides whether to honour it.
    int rotate = 0;
    bool flipH = false, flipV = false, portrait = false;

    // How the picture was framed in the window: the zoom and where it was
    // dragged to. Both are cut off by the canvas, so a picture pushed off to
    // one side leaves black behind it in the file exactly as it does on screen.
    double zoom = 1.0;
    double panX = 0.0, panY = 0.0;

    // What came in, so the window can say what will go out.
    int sourceWidth = 0, sourceHeight = 0;
    double sourceFps = 0.0;
    double speed = 1.0;
    bool keepPitch = true;

    // The captions, as a subtitle file already written to disk. Empty when
    // there are none. The same file the player is showing, so the burned-in
    // result is the preview.
    std::wstring captions;
};

// Builds the ffmpeg command line. Exposed so the window can show it, which is
// the fastest way for anyone to find out why an export came out wrong.
std::wstring buildRenderCommand(const RenderJob& job, const RenderPreset& preset,
                                const std::wstring& output);

// Opens the export window and blocks until it is closed. Returns true when a
// file was written.
// `fromPlayer` is the same switch as the one in the combined clip window,
// so turning it off in either place means the same thing in both.
bool runRenderWindow(HWND owner, HINSTANCE instance, const RenderJob& job,
                     bool* fromPlayer = nullptr);

}  // namespace vp
