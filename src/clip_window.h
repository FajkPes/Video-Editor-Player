#pragma once
// The two property sheets the editor opens on a double click.
//
// Both are modal, both edit a copy and only write it back on Save, and both are
// drawn the same way as the rest of the program. They are here rather than in
// the timeline because the timeline is a widget: it should not be able to open
// windows, and it does not know what an HINSTANCE is.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <functional>
#include <string>

#include "timeline.h"

namespace vp {

// One sound track over one piece. `trackName` and `when` only appear in the
// heading, so the window can say which of a dozen pieces is being edited.
// `preview` is called on every change while the window is open, so what is
// being set is heard and seen at once rather than on Save. Cancel is the
// caller's to undo - it knows what the piece held before.
bool runClipAudioWindow(HWND owner, HINSTANCE instance, ClipAudio& clip,
                        const std::wstring& trackName, const std::wstring& when,
                        std::function<void(const ClipAudio&)> preview = {});

// The picture over one piece.
bool runClipVideoWindow(HWND owner, HINSTANCE instance, ClipVideo& clip,
                        const std::wstring& when,
                        std::function<void(const ClipVideo&)> preview = {});

// ---------------------------------------------------------------------------
// The whole film's settings in one window: the picture panel and every sound
// track, the same values the panel down the right-hand side of the player is
// showing. Nothing is copied - the window reads and writes through these, so
// the two are one instance of the same thing and the panel is the quick way to
// the handful of them that are reached most often.

struct SheetTrack {
    std::wstring name;
    std::wstring note;                      // rate, channels, what levelling did
    std::function<bool()> playing;
    std::function<void(bool)> setPlaying;
    std::function<bool()> normalise;
    std::function<void(bool)> setNormalise;
    std::function<double()> gain;   std::function<void(double)> setGain;
    std::function<double()> low;    std::function<void(double)> setLow;
    std::function<double()> mid;    std::function<void(double)> setMid;
    std::function<double()> high;   std::function<void(double)> setHigh;
    std::function<double()> pitch;  std::function<void(double)> setPitch;
    std::function<void()> reset;

    // Which stream of the file this row is read from, and a way to point it at
    // another. Media Foundation does not always hand the streams over in the
    // file's order and there is no way to ask it which is which, so when the
    // wave under a row belongs to the other track this is the thing that fixes
    // it - and it stays fixed, rather than being guessed at again.
    std::function<std::wstring()> stream;
    std::function<void()> nextStream;
};

struct PlayerSheet {
    std::wstring subtitle;

    // On: what is set here is what is playing and what is exported. Off: the
    // player keeps what it has and these figures are used by the editor and the
    // export alone. The export window's own switch is this same switch.
    std::function<bool()> fromPlayer;
    std::function<void(bool)> setFromPlayer;

    // The seven picture bands, in the player's own order.
    std::function<double(int)> band;
    std::function<void(int, double)> setBand;
    std::function<void()> resetPicture;

    // The picture's framing as the player is showing it: 0 zoom, 1 across,
    // 2 down. Left empty when there is no picture to frame.
    std::function<double(int)> view;
    std::function<void(int, double)> setView;

    std::function<bool()> normaliseAll;
    std::function<void(bool)> setNormaliseAll;
    std::function<double()> target;
    std::function<void(double)> setTarget;

    std::vector<SheetTrack> tracks;
};

void runPlayerWindow(HWND owner, HINSTANCE instance, PlayerSheet& sheet);

// One caption. Returns false when Cancel was pressed; `deleted` comes back true
// when the window was closed with Delete, which the caller acts on because only
// it owns the list.
bool runTextWindow(HWND owner, HINSTANCE instance, TextItem& item, double duration,
                   double playhead, bool* deleted);

}  // namespace vp
