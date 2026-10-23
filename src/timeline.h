#pragma once
// The editor's time axis.
//
// A text row on top, then one row for the picture and one for each sound track,
// the way a cutting program lays them out. A split cuts every row at once,
// because the job this is mostly for is "drop the dead minute in the middle".
//
// Two things narrow that down when the whole column is not what was meant:
//
//   Alt+click selects a single row, so one track can be silenced or shaped over
//   one stretch without touching the others.
//
//   Every piece carries its own settings, which sit on top of whatever the
//   player's own panels are set to rather than replacing them: a clip can be a
//   few dB louder, a tone lower, darker, or cropped differently from the rest
//   of the film, and the mixer and the picture panel still mean what they say.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "media_tools.h"
#include "json.h"
#include "theme.h"

namespace vp {

enum class FadeCurve { Linear, Smooth };

// One sound track's settings over one piece, on top of the mixer's own. Gains
// multiply, everything else adds, so leaving this alone changes nothing.
struct ClipAudio {
    bool set = false;           // false means "follow the mixer and nothing else"
    float gain = 1.0f;          // multiplies the fader
    float eqLow = 0.0f, eqMid = 0.0f, eqHigh = 0.0f;   // added, in dB
    float pitch = 0.0f;         // added, in semitones
    bool silent = false;        // the old per-piece mute, kept as a switch
};

// The picture's settings over one piece, again on top of the picture panel.
// The crop is in fractions of the frame, so it survives a change of resolution.
struct ClipVideo {
    bool set = false;
    float opacity = 1.0f;       // 1 as shot, 0 fully dark
    int contrast = 0, brightness = 0, saturation = 0, gamma = 0, hue = 0;
    int sharpen = 0, denoise = 0;
    double cropX = 0.0, cropY = 0.0, cropW = 1.0, cropH = 1.0;
    double panX = 0.0, panY = 0.0;   // fractions of the frame, from the middle
    double zoom = 1.0;               // 1 as shot, 2 twice as close
    double speed = 1.0;              // this stretch alone, on top of the player
};

inline bool sameClipVideo(const ClipVideo& a, const ClipVideo& b) {
    auto fields=[](const ClipVideo& v) {
        return std::tie(v.set,v.opacity,v.contrast,v.brightness,v.saturation,v.gamma,v.hue,
                        v.sharpen,v.denoise,v.cropX,v.cropY,v.cropW,v.cropH,v.panX,v.panY,v.zoom,v.speed);
    };
    return fields(a)==fields(b);
}

// The crop, the zoom and the pan of one clip fold down into a single window
// over the frame, which is what both the player and the export then cut out.
// Doing the arithmetic in one place is what keeps the two the same picture.
// Returns false when that window is the whole frame and no crop is needed.
// What one piece is played at, on top of whatever the player's own speed chip
// says. Out of range values are refused rather than stopping playback.
// How far the frame is slid across the canvas, in fractions of it. Black takes
// the place it left, which is what "keep the original canvas" means.
inline void clipShift(const ClipVideo& v, double* x, double* y) {
    *x = v.set ? std::max(-1.0, std::min(1.0, v.panX)) : 0.0;
    *y = v.set ? std::max(-1.0, std::min(1.0, v.panY)) : 0.0;
}

inline double clipSpeed(const ClipVideo& v) {
    if (!v.set) return 1.0;
    return std::max(0.25, std::min(4.0, v.speed));
}

inline bool clipWindow(const ClipVideo& v, double* x, double* y, double* w,
                       double* h) {
    if (!v.set) return false;
    const double zoom = v.zoom > 0.01 ? v.zoom : 1.0;
    const double ww = std::max(0.02, std::min(1.0, v.cropW / zoom));
    const double hh = std::max(0.02, std::min(1.0, v.cropH / zoom));
    // The zoom closes in on the middle of the cropped rectangle. The pan is not
    // part of this: a picture with nothing cropped away has no slack to slide
    // inside, and asking a crop to do it meant the position did nothing at all
    // until something had been zoomed or trimmed. It moves the frame on the
    // canvas instead, which works whatever the picture is - a tall video in a
    // wide window included.
    const double xx = std::max(0.0, std::min(1.0 - ww, v.cropX + (v.cropW - ww) * 0.5));
    const double yy = std::max(0.0, std::min(1.0 - hh, v.cropY + (v.cropH - hh) * 0.5));
    *x = xx; *y = yy; *w = ww; *h = hh;
    return xx > 0.001 || yy > 0.001 || ww < 0.999 || hh < 0.999;
}

struct Segment {
    double start = 0.0;
    double end = 0.0;
    bool kept = true;
    std::set<int> muted;              // track indices silenced over this stretch
    ClipVideo video;
    std::map<int, ClipAudio> audio;   // track index -> what that track does here
};

// A row's fades, in seconds. Row 0 is the picture; the rest are sound tracks.
struct RowFades {
    double in = 0.0;
    double out = 0.0;
    FadeCurve curve = FadeCurve::Linear;
};

// One caption. Rendered by libass, both in the player and in the export, from
// the same generated file - so what is on screen while editing is what lands in
// the file, down to the outline width.
struct TextItem {
    double start = 0.0, end = 0.0;
    std::wstring text = L"Text";

    std::wstring font = L"Segoe UI";
    int size = 48;                    // points against a 1080-tall frame
    bool bold = false, italic = false;

    COLORREF colour = RGB(255, 255, 255);
    COLORREF outlineColour = RGB(0, 0, 0);
    COLORREF shadowColour = RGB(0, 0, 0);
    int outline = 2;                  // border width, 0 for none
    int shadow = 0;                   // drop shadow offset, 0 for none
    int opacity = 100;                // per cent

    // Where on the frame, as the nine cells of a numeric keypad: 1 is bottom
    // left, 5 the middle, 9 top right. The same numbering ASS itself uses, so
    // there is nothing to translate on the way out.
    int grid = 2;
    int marginX = 40, marginY = 40;   // against a 1920x1080 frame

    double fadeIn = 0.25, fadeOut = 0.25;
};

class Timeline {
public:
    static constexpr int kHeadHeight = 22;
    static constexpr int kTextRow = 30;
    static constexpr int kVideoRow = 46;
    static constexpr int kAudioRow = 34;
    static constexpr int kGutter = 88;
    static constexpr int kGap = 3;
    static constexpr int kRightPad = 12;

    // ------------------------------------------------------------ contents
    void setMedia(double duration, const std::vector<std::string>& trackNames);
    // Only the labels, leaving the cuts and everything else alone.
    // The tracks have been put in the file's order; everything the strip holds
    // about them by number follows.
    void reorderTracks(const std::vector<size_t>& order) {
        if (order.size() != trackNames_.size()) return;
        std::vector<int> place(order.size(), 0);
        for (size_t k = 0; k < order.size(); ++k) place[order[k]] = static_cast<int>(k);

        std::vector<std::string> names;
        std::vector<Waveform> waves;
        for (size_t k = 0; k < order.size(); ++k) {
            names.push_back(trackNames_[order[k]]);
            if (order[k] < waves_.size()) waves.push_back(waves_[order[k]]);
        }
        trackNames_ = std::move(names);
        if (waves.size() == waves_.size()) waves_ = std::move(waves);

        for (Segment& s : segments_) {
            std::set<int> muted;
            for (int t : s.muted) {
                if (t >= 0 && t < static_cast<int>(place.size())) muted.insert(place[t]);
            }
            s.muted = std::move(muted);
            std::map<int, ClipAudio> audio;
            for (const auto& pair : s.audio) {
                const int t = pair.first;
                audio[t >= 0 && t < static_cast<int>(place.size()) ? place[t] : t] =
                    pair.second;
            }
            s.audio = std::move(audio);
        }

        std::vector<RowFades> fades = fades_;
        for (size_t k = 0; k < order.size(); ++k) {
            const int to = rowForTrack(static_cast<int>(k));
            const int from = rowForTrack(static_cast<int>(order[k]));
            if (to > 0 && from > 0 && static_cast<size_t>(to) < fades_.size() &&
                static_cast<size_t>(from) < fades_.size()) {
                fades[static_cast<size_t>(to)] = fades_[static_cast<size_t>(from)];
            }
        }
        fades_ = std::move(fades);
    }

    void setTrackNames(const std::vector<std::string>& names) {
        if (names.size() == trackNames_.size()) trackNames_ = names;
    }
    void clear();

    double duration() const { return duration_; }
    int trackCount() const { return static_cast<int>(trackNames_.size()); }
    int rowCount() const { return 1 + static_cast<int>(trackNames_.size()); }

    // Row 0 is the picture; the sound rows follow in the file's own order, so
    // A1 is the row under the picture and the first track of the file, exactly
    // as the panel lists it.
    int trackForRow(int row) const { return row <= 0 ? -1 : row - 1; }
    int rowForTrack(int track) const { return track < 0 ? 0 : track + 1; }
    int heightFor() const;

    void setWaveforms(std::vector<Waveform> waves) { waves_ = std::move(waves); }
    bool hasWaveforms() const { return !waves_.empty(); }
    void setThumbs(std::vector<Thumb> thumbs) { thumbs_ = std::move(thumbs); }
    bool hasThumbs() const { return !thumbs_.empty(); }

    // ------------------------------------------------------------- editing
    bool splitAt(double seconds);
    void deleteSelected();
    void restoreSelected();
    void resetCuts();
    void setMarkIn(double seconds);
    void setMarkOut(double seconds);
    void clearMarks();

    double markIn() const { return markIn_; }
    double markOut() const { return markOut_; }

    // The stretch that actually leaves the program: the marks narrow it, the
    // dropped segments punch holes in it.
    double outLow() const;
    double outHigh() const;
    std::vector<Segment> keptSegments() const;
    double keptDuration() const;

    // Every kept stretch with the set of tracks silenced over it, which is what
    // both the player and the render need to honour a per-track cut.
    std::vector<Segment> muteRanges() const;

    int selected() const { return selected_; }
    int selectedRow() const { return selectedRow_; }
    const std::vector<Segment>& segments() const { return segments_; }
    std::vector<Segment>& segments() { return segments_; }

    // Which piece covers a moment, or -1. The player asks this every tick to
    // know whose settings apply right now.
    int segmentAt(double seconds) const;

    // The settings of one piece. `clipAudio` makes the entry if it is not there
    // yet, which is what an editor opening the properties of a clip wants.
    ClipAudio& clipAudio(int segment, int track);
    const ClipAudio* findClipAudio(int segment, int track) const;
    ClipVideo& clipVideo(int segment);

    // Asked for before a file is open, and asked for by row number by code
    // that counts rows differently, so it answers rather than reading past the
    // end of the list.
    // What the mixer is actually doing to a row: the fader, and whatever
    // levelling is adding or taking away. The strip draws its waves at that
    // size, so a track pulled down looks pulled down.
    void setRowGain(int row, float gain) {
        if (row < 0) return;
        if (static_cast<size_t>(row) >= rowGain_.size()) {
            rowGain_.resize(static_cast<size_t>(row) + 1, 1.0f);
        }
        rowGain_[static_cast<size_t>(row)] = gain;
    }
    float rowGain(int row) const {
        if (row < 0 || static_cast<size_t>(row) >= rowGain_.size()) return 1.0f;
        return rowGain_[static_cast<size_t>(row)];
    }

    RowFades& fades(int row) {
        if (row < 0 || row >= static_cast<int>(fades_.size())) return spare_;
        return fades_[static_cast<size_t>(row)];
    }
    const RowFades& fades(int row) const {
        if (row < 0 || row >= static_cast<int>(fades_.size())) return spare_;
        return fades_[static_cast<size_t>(row)];
    }

    // ---------------------------------------------------------------- text
    std::vector<TextItem>& texts() { return texts_; }
    const std::vector<TextItem>& texts() const { return texts_; }
    int selectedText() const { return selectedText_; }
    void selectText(int index) { selectedText_ = index; }
    // Adds one starting where the playhead is, and returns its index.
    int addText(double at);
    void deleteText(int index);

    // The whole text row as an ASS subtitle document. mpv renders it live and
    // ffmpeg burns the identical file in, so the preview is the output.
    std::string assDocument(int frameWidth, int frameHeight) const;

    // -------------------------------------------------------------- screen
    void setBounds(const RECT& r) { bounds_ = r; scrollRows(0); }
    void scrollRows(int pixels) {
        scrollY_ = std::clamp(scrollY_ + pixels, 0,
                              std::max(0, heightFor() - static_cast<int>(bounds_.bottom - bounds_.top)));
    }
    const RECT& bounds() const { return bounds_; }

    void paint(HDC dc, double playhead, POINT mouse, const theme::Fonts* fonts) const;

    // Returns true when the click was consumed. `seekTo` is filled when the
    // click asked for the playhead to move.
    bool onDown(POINT p, bool alt, bool* seeked, double* seekTo);
    bool onDrag(POINT p, bool* seeked, double* seekTo);
    void onUp();
    // Double click on a fade handle clears that fade; on a caption it opens it.
    bool onDoubleClick(POINT p);
    // The row under the cursor, or -1. Used to aim the fade-curve menu.
    // Everything the strip holds, written down and read back: the cuts, what
    // each piece does to each row, the fades, the marks and the captions.
    json::Value toJson() const;
    void fromJson(const json::Value& v);

    int rowAt(POINT p) const;
    int gapAfter(int row) const;
    // True when the point is in the caption row rather than in a track row.
    bool inTextRow(POINT p) const;
    // The caption under the cursor, or -1.
    int textAt(POINT p) const;

    bool dragging() const { return drag_ != Drag::None; }

    std::wstring statusLine() const;

private:
    int textTop() const { return bounds_.top + kHeadHeight - scrollY_; }
    int trackStart() const { return textTop() + kTextRow + kGap; }
    int rowTop(int row) const;
    int rowHeight(int row) const;
    int bodyLeft() const { return bounds_.left + kGutter; }
    int bodyRight() const { return bounds_.right - kRightPad; }
    double timeAt(int x) const;
    int xFor(double seconds) const;

    void paintRuler(HDC dc, const theme::Fonts* fonts) const;
    void paintTextRow(HDC dc, POINT mouse, const theme::Fonts* fonts) const;
    void paintRow(HDC dc, int row, POINT mouse, const theme::Fonts* fonts) const;
    void paintFadeHandles(HDC dc, int row, bool rowHot,
                          const theme::Fonts* fonts) const;

    enum class Drag { None, Head, FadeIn, FadeOut, TextMove, TextStart, TextEnd };
    Drag drag_ = Drag::None;
    int dragRow_ = -1;
    int dragText_ = -1;
    double dragGrab_ = 0.0;      // where in the caption it was picked up

    double duration_ = 0.0;
    std::vector<std::string> trackNames_;
    std::vector<Segment> segments_;
    std::vector<RowFades> fades_;
    mutable RowFades spare_;   // answered with when a row does not exist
    std::vector<float> rowGain_;   // one per row, the mixer's own doing
    std::vector<Waveform> waves_;
    std::vector<Thumb> thumbs_;
    std::vector<TextItem> texts_;

    double markIn_ = -1.0, markOut_ = -1.0;
    int selected_ = -1;
    int selectedRow_ = -1;      // -1 means the whole column
    int selectedText_ = -1;

    RECT bounds_{};
    int scrollY_ = 0;
};

}  // namespace vp
