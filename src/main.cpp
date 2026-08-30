// The window.
//
// Video on the left, the mixer and the picture controls on the right, the
// editor's time axis under the picture and the transport along the bottom - the
// arrangement of the version this replaces.
//
// Everything is drawn by hand. The whole interface is a column of rectangles and
// four kinds of widget (button, chip, checkbox, slider), so a toolkit would cost
// a dependency and a build step to save very little, and would not theme this
// dark without a fight. Hit testing is the same rectangles read back.

#include "theme.h"

#include <windowsx.h>
#include <objbase.h>
#include <shellapi.h>
#include <commdlg.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "audio_engine.h"
#include "clip_window.h"
#include "json.h"
#include "lang.h"
#include "media_tools.h"
#include "mpv_video.h"
#include "render.h"
#include "settings.h"
#include "settings_window.h"
#include "timeline.h"

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

using namespace vp::theme;
using vp::tr;

namespace {

constexpr int kPanelWidth  = 480;
// The transport sits directly under the picture, the way it did in the version
// this replaces: seek bar, then one row of controls, or two when the window is
// too narrow to hold them side by side.
constexpr int kTransportOne = 66;
constexpr int kTransportTwo = 104;
constexpr int kTrackRow    = 56;
constexpr int kTrackOpen   = 56 + 100;   // a row with its equaliser showing
constexpr int kAdjustRow   = 26;
// Two rows of buttons and the hint line under them. One row stopped fitting
// once a piece could be opened and a caption added, and a row that does not
// fit hides the buttons at its right-hand end rather than shrinking.
constexpr int kEditorBar   = 104;

// Everything the single-row transport needs, so the layout can ask whether it
// fits before deciding to use two rows.
constexpr int kOneRowNeeds = 1290;

std::wstring formatTime(double seconds, bool withTenths = false) {
    if (!(seconds >= 0.0)) seconds = 0.0;
    const int total = static_cast<int>(seconds);
    wchar_t buf[40];
    if (withTenths) {
        _snwprintf_s(buf, _TRUNCATE, L"%d:%02d.%d", total / 60, total % 60,
                     static_cast<int>((seconds - total) * 10));
    } else if (total >= 3600) {
        _snwprintf_s(buf, _TRUNCATE, L"%d:%02d:%02d", total / 3600, (total / 60) % 60,
                     total % 60);
    } else {
        _snwprintf_s(buf, _TRUNCATE, L"%02d:%02d", total / 60, total % 60);
    }
    return buf;
}

std::wstring widen(const std::string& s) { return vp::fromUtf8(s); }

struct Hit {
    RECT r{};
    bool has(POINT p) const { return PtInRect(&r, p) != 0; }
};

// The picture is a child window, which means every click that lands on it goes
// to that window and not to this one. Without this the whole middle of the
// program is dead to the mouse: no click to pause, no double click for full
// screen, no wheel for volume.
//
// The coordinates have to be translated on the way through. The child used to
// sit at the origin of the parent's client area, where the two agree; it is now
// the canvas, which moves and grows with the zoom, so at 300 % it sits at some
// large negative offset - and a press forwarded unchanged looked to the window
// as though it had landed somewhere else entirely, which is why the picture
// could not be dragged at all once it was zoomed in far enough.
LRESULT CALLBACK videoHostProc(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    switch (message) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_MOUSEMOVE: {
            POINT at{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            MapWindowPoints(hwnd, GetParent(hwnd), &at, 1);
            return SendMessageW(GetParent(hwnd), message, w,
                                MAKELPARAM(at.x, at.y));
        }
        case WM_MOUSEWHEEL:
            // The wheel already speaks in screen coordinates.
            return SendMessageW(GetParent(hwnd), message, w, l);
        default:
            return DefWindowProcW(hwnd, message, w, l);
    }
}

// Most picture controls are an mpv property with a name. Two are not: sharpening
// is a shader that has to be rewritten and reloaded, and denoising is a filter
// chain. They live in the same list anyway, because to the person using them
// they are the same seven sliders.
enum class AdjustKind { Property, Sharpen, Denoise, Zoom, PanX, PanY };

struct Adjust {
    const wchar_t* label;
    const char* property;
    AdjustKind kind;
    int lo, hi, value;
    Hit slider;
};

const double kSpeeds[] = {0.1,  0.25, 0.5,  0.75, 0.9,  1.0,  1.1,  1.25,
                          1.5,  1.75, 2.0,  2.5,  3.0,  4.0};
// English here, translated where they are used: this array is built before
// the language file has been read.
const wchar_t* const kFitNames[] = {L"Fit", L"Fill", L"Original"};
const float kTargets[] = {-23.0f, -18.0f, -16.0f, -14.0f, -11.0f};

const wchar_t* const kSharpenShader =
    L"//!HOOK OUTPUT\n"
    L"//!BIND HOOKED\n"
    L"//!DESC Diamond adaptive sharpen (Video Player)\n"
    L"\n"
    L"// Contrast-adaptive sharpening on a diamond: the centre pixel and its four\n"
    L"// edge neighbours, five taps in one pass. The nine-tap version costs nearly\n"
    L"// twice the samples for a difference nobody sees in motion, and this runs\n"
    L"// at full rate on 4K on hardware that the nine-tap one made stutter.\n"
    L"//\n"
    L"// The weight is scaled by how much headroom the pixel has before it would\n"
    L"// clip, which is what stops a sharpener from ringing along hard edges.\n"
    L"\n"
    L"#define AMOUNT %.4f\n"
    L"\n"
    L"vec4 hook() {\n"
    L"    vec3 e = HOOKED_tex(HOOKED_pos).rgb;\n"
    L"    if (AMOUNT < 0.002) return vec4(e, 1.0);\n"
    L"\n"
    L"    vec2 p = HOOKED_pt;\n"
    L"    vec3 n = HOOKED_tex(HOOKED_pos + vec2(0.0, -p.y)).rgb;\n"
    L"    vec3 s = HOOKED_tex(HOOKED_pos + vec2(0.0,  p.y)).rgb;\n"
    L"    vec3 w = HOOKED_tex(HOOKED_pos + vec2(-p.x, 0.0)).rgb;\n"
    L"    vec3 t = HOOKED_tex(HOOKED_pos + vec2( p.x, 0.0)).rgb;\n"
    L"\n"
    L"    vec3 mn = min(min(min(n, s), min(w, t)), e);\n"
    L"    vec3 mx = max(max(max(n, s), max(w, t)), e);\n"
    L"    vec3 amp = sqrt(clamp(min(mn, 1.0 - mx) / max(mx, 1e-5), 0.0, 1.0));\n"
    L"\n"
    L"    // The slider runs to 2, so half of it is the curve's own 0..1. Scaling\n"
    L"    // the weight past that end is what tore the picture: the divisor below\n"
    L"    // reaches zero at k = -0.25 and turns negative after it, and a negative\n"
    L"    // divisor does not sharpen, it inverts. mix(11, 5) keeps k inside\n"
    L"    // -0.2, which is the strongest setting the published curve offers and\n"
    L"    // the strongest one that stays stable.\n"
    L"    float a = clamp(AMOUNT * 0.5, 0.0, 1.0);\n"
    L"    vec3 k = amp * (-1.0 / mix(11.0, 5.0, a));\n"
    L"\n"
    L"    vec3 sharp = (e + (n + s + w + t) * k) / (1.0 + 4.0 * k);\n"
    L"\n"
    L"    // The kernel itself cannot be pushed any harder without turning\n"
    L"    // negative, so what is left is to take the difference it found\n"
    L"    // and weigh it more. Amplifying the overshoot this way keeps the\n"
    L"    // filter stable and still bites the way the top of the slider\n"
    L"    // ought to.\n"
    L"    vec3 lifted = e + (sharp - e) * 1.6;\n"
    L"    return vec4(clamp(lifted, 0.0, 1.0), 1.0);\n"
    L"}\n";

}  // namespace

// ============================================================================

class Player {
public:
    bool create(HINSTANCE instance, const std::wstring& openThis);
    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);

private:
    void layout();
    void paint(HDC dc);
    void paintPanel(HDC dc, const RECT& r);
    void paintTracks(HDC dc, int x, int w, int& y);
    void paintPicture(HDC dc, int x, int w, int& y);
    void paintTransport(HDC dc, const RECT& r);
    void paintEditorBar(HDC dc);
    void paintOsd(HDC dc, const RECT& video);

    void onDown(POINT p);
    void onMove(POINT p);
    void onUp();
    void onWheel(int delta, POINT p);
    void onKey(WPARAM key);
    void tick();

    void open(const std::wstring& path);
    void choose();
    void togglePlay();
    void seekBy(double d);
    void seekTo(double s);
    void setVolume(float v);
    void cycleSpeed(int direction);
    void pickSpeed(POINT at);
    void setSpeedIndex(int index);
    void cycleFit();
    void cycleTarget();
    void applyAdjust(size_t i);
    int clipAdjust(size_t which) const;
    void applySharpen();
    void applyDenoise();
    void resetAdjust();
    void resetEq(size_t track);
    void toggleFullscreen();
    void toggleEditor();
    void applyVideoFilters();
    void applyGeometry();
    void applyFades(bool force = false);
    void applyClipPicture();
    void saveProject();
    void openProject();

    // Which track the panel's n-th row from the top is about. One list, one
    // order, the file's own: the first track of the file is the first row here,
    // the first row of the strip and the first stream of the export. Counting
    // this panel backwards while the strip counted forwards is what made the
    // two disagree about which track was which.
    size_t panelTrack(size_t row) const { return row; }

    // Where the frame that will be written sits on screen. It is the shape of
    // the file, fitted into the room the picture is given, then moved and
    // scaled by however the picture is being looked at - so zooming out shows
    // the whole canvas getting smaller with black around it, and dragging
    // carries the canvas along instead of sliding the picture out from under
    // its own edges.
    void placeCanvas() {
        RECT canvas = videoStage_;
        const int roomW = std::max(1L, canvas.right - canvas.left);
        const int roomH = std::max(1L, canvas.bottom - canvas.top);
        int vw = video_.width(), vh = video_.height();
        if (rotate_ == 90 || rotate_ == 270) std::swap(vw, vh);
        if (portrait_ && vh > 0) vw = std::max(1, vh * 9 / 16);
        double w = roomW, h = roomH;
        if (vw > 0 && vh > 0) {
            const double fit = std::min(static_cast<double>(roomW) / vw,
                                        static_cast<double>(roomH) / vh);
            w = vw * fit;
            h = vh * fit;
        }
        w *= zoom_;
        h *= zoom_;
        const double midX = roomW / 2.0 + panX_ * roomW;
        const double midY = roomH / 2.0 + panY_ * roomH;
        double left = midX - w / 2.0;
        double top = midY - h / 2.0;
        // It may not be pushed out of the room it was given, or it would end up
        // over the panel and the transport - a child window is only clipped by
        // its parent, and the parent is the whole window. When it is smaller
        // than the room it stays inside it; when it is larger its edges may not
        // come in past the edges of the room.
        // The one rule left: a quarter of the picture stays in sight. That is
        // all - and it means the further in it is zoomed the further it can be
        // dragged, which is the way round it should be. Holding its edges to
        // the edges of the room did the opposite: at a little over full size
        // there was almost nowhere to go.
        const double keepX = std::min(w, static_cast<double>(roomW)) * 0.25;
        const double keepY = std::min(h, static_cast<double>(roomH)) * 0.25;
        left = std::max(keepX - w, std::min(roomW - keepX, left));
        top = std::max(keepY - h, std::min(roomH - keepY, top));
        // What the clamp allowed is what the hand is holding, so a drag that
        // has reached the edge stops there instead of running away invisibly.
        panX_ = (left + w / 2.0 - roomW / 2.0) / roomW;
        panY_ = (top + h / 2.0 - roomH / 2.0) / roomH;

        videoCanvas_ = {static_cast<LONG>(left + 0.5), static_cast<LONG>(top + 0.5),
                        static_cast<LONG>(left + w + 0.5),
                        static_cast<LONG>(top + h + 0.5)};
        const int cw = std::max(1L, videoCanvas_.right - videoCanvas_.left);
        const int ch = std::max(1L, videoCanvas_.bottom - videoCanvas_.top);
        SetWindowPos(videoHost_, nullptr, videoCanvas_.left, videoCanvas_.top, cw, ch,
                     SWP_NOZORDER | SWP_NOACTIVATE);

        // Zoomed in, the canvas is larger than the room, and a child window is
        // clipped only by its parent - which is the whole window, panel and
        // all. The part that hangs over the room is cut away with a region, so
        // the picture can never be drawn over anything that is being read.
        RECT seen{};
        if (IntersectRect(&seen, &videoCanvas_, &videoStage_)) {
            OffsetRect(&seen, -videoCanvas_.left, -videoCanvas_.top);
        } else {
            seen = {0, 0, 0, 0};
        }
        // Recomputed and reapplied every time the picture is placed: a region
        // is in the window's own coordinates, so a stale one from a different
        // size or a different room is exactly how the picture ends up over the
        // panel again.
        SetWindowRgn(videoHost_, CreateRectRgnIndirect(&seen), TRUE);
    }
    // The waves, the loudness figures and the export all read the file
    // through ffmpeg, so when a row is pointed at a different stream they are
    // all read again.
    void rereadStreams() {
        std::vector<int> order;
        for (const vp::TrackInfo& t : audio_.tracks()) order.push_back(t.audioIndex);
        loudnessJob_.start(path_, order);
        timeline_.setWaveforms({});
        wavesWanted_ = false;
        if (editorOpen_) buildPreviews(true);
        InvalidateRect(window_, nullptr, TRUE);
    }
    // The three framing rows follow whatever the hand does to the picture, so
    // the panel is never telling a different story from the screen.
    void syncViewRows() {}
    // The panels' own frame rate, from the settings. mpv draws the picture at
    // the film's rate whatever this is; this is the meters and the strip.
    void startClock() {
        const int fps = std::max(10, std::min(144, vp::settings().uiFps));
        SetTimer(window_, 1, static_cast<UINT>(1000 / fps), nullptr);
    }
    void sleepVideo(bool asleep) {
        if (asleep == videoAsleep_ || path_.empty()) return;
        videoAsleep_ = asleep;
        video_.setProperty("vid", asleep ? "no" : "auto");
        if (!asleep) video_.seek(audio_.position() + avOffset_);
    }
    // A row picked with Alt, over a piece: what the row's own window needs.
    bool localReady() const {
        return timeline_.selected() >= 0 && timeline_.selectedRow() >= 0;
    }
    void openGlobalProperties();
    void openLocalProperties();
    void seedOverride();
    double sheetBand(int which) const;
    void setSheetBand(int which, double value);
    void releaseBuffer() {
        if (bufferDc_) {
            SelectObject(bufferDc_, bufferOld_);
            DeleteDC(bufferDc_);
            bufferDc_ = nullptr;
        }
        if (buffer_) { DeleteObject(buffer_); buffer_ = nullptr; }
        bufferSize_ = {0, 0};
    }

    HDC bufferDc_ = nullptr;
    HBITMAP buffer_ = nullptr;
    HGDIOBJ bufferOld_ = nullptr;
    SIZE bufferSize_{0, 0};
    RECT paintClip_{0, 0, 1 << 20, 1 << 20};   // what this frame must cover
    void applyPan();
    void bumpRotate(int degrees);
    void toggleFlip(bool horizontal);
    void cycleOrientation();
    void bumpZoom(double factor);
    void resetView();
    void frameStep(bool backwards);
    void toggleKeepPitch();
    void bumpAvOffset(int milliseconds);
    void toggleTrack(size_t index);
    void toggleTrackNorm(size_t index);
    void openClipProperties(int segment, int row);
    void openTextProperties(int index);
    void addCaption();
    void applyClipSettings(bool force);
    void refreshCaptions();
    RECT videoArea() const;
    void refreshStats();
    void buildPreviews(bool force);
    void openRender();
    void openSettings();
    void screenshot();
    void applyAudioPreset(int index);
    void saveAudioPreset();
    void deleteAudioPreset();
    void applyVideoPreset(int index);
    void saveVideoPreset();
    void deleteVideoPreset();
    void rememberPosition();
    void say(const std::wstring& text);
    double fraction(const Hit& h, POINT p) const;

    HWND window_ = nullptr;
    HWND videoHost_ = nullptr;
    HINSTANCE instance_ = nullptr;
    Fonts fonts_;

    vp::AudioEngine audio_;
    vp::MpvVideo video_;
    vp::Timeline timeline_;
    vp::WaveformJob waveJob_;
    vp::ThumbJob thumbJob_;
    vp::LoudnessJob loudnessJob_;

    std::wstring path_, fileName_, infoTop_, infoBottom_;
    std::string startupError_;

    Hit seek_, play_, back_, forward_, first_, last_, stepBack_, stepForward_;
    Hit volume_, open_, gear_, full_, speedChip_, fitChip_, normChip_, targetChip_;
    Hit editorChip_, loopChip_, resetPicture_, statsChip_, pitchChip_, muteButton_;
    Hit rotateLeft_, rotateRight_, flipHChip_, flipVChip_, orientChip_, zoomReset_;
    Hit audioPresetChip_, audioPresetSave_, audioPresetDelete_;
    Hit videoPresetChip_, videoPresetSave_, videoPresetDelete_;
    Hit editSplit_, editDelete_, editRestore_, editResetCuts_;
    Hit editLocalProps_;
    Hit editSaveProject_, editOpenProject_;
    Hit editMarkIn_, editMarkOut_, editClearMarks_, editPreviews_, editRender_;
    Hit editAddText_, editClipProps_;
    std::vector<Hit> trackEnable_, trackSolo_, trackNorm_, trackExpand_, trackVolume_;
    std::vector<Hit> eqLow_, eqMid_, eqHigh_, eqPitch_, eqReset_;
    std::vector<char> expanded_;
    std::vector<Adjust> adjust_;

    // The track list is the one part of the panel that can be taller than the
    // room it has, so it is the one part that scrolls. Everything below it keeps
    // its place whatever the file turns out to hold.
    RECT tracksView_{};
    int tracksContent_ = 0;
    int trackScroll_ = 0;
    bool twoRows_ = false;

    enum class Drag { None, Seek, Volume, TrackVolume, EqLow, EqMid, EqHigh, Pitch,
                      Adjust, Timeline };
    Drag drag_ = Drag::None;
    int dragIndex_ = -1;

    POINT mouse_{};
    bool isFull_ = false;
    bool editorOpen_ = false;
    bool looping_ = false;
    RECT beforeFull_{};
    float volume_level_ = 1.0f, volumeBeforeMute_ = 1.0f;
    int speedIndex_ = 5, fitIndex_ = 0, targetIndex_ = 3;
    int audioPresetIndex_ = -1, videoPresetIndex_ = -1;
    int shaderSlot_ = 0;
    int rotate_ = 0;
    bool flipH_ = false, flipV_ = false, portrait_ = false;
    double zoom_ = 1.0, panX_ = 0.0, panY_ = 0.0;
    double fadeSignature_ = -1.0;   // what the fades were when last pushed
    // The framing that is part of the film, as against zoom_/panX_/panY_ which
    // are only how it is being looked at here.
    double frameZoom_ = 1.0, framePanX_ = 0.0, framePanY_ = 0.0;
    double livePlaySpeed_ = 1.0;    // chip speed times the piece's own
    bool hwdecCopy_ = false;        // frames copied back for the filters
    bool videoAsleep_ = false;      // the picture is off while minimised
    std::wstring projectPath_;      // the work file this came from
    RECT videoStage_{0, 0, 0, 0};   // the room the picture is given
    RECT videoCanvas_{0, 0, 0, 0};  // the frame that will be written
    int stageW_ = 0, stageH_ = 0;   // the frame size the canvas was cut for
    double liveClipPanX_ = 0.0, liveClipPanY_ = 0.0;   // the piece's own shift

    // On, everything is taken from the player. Off, the figures below are what
    // the editor and the export use and the player is left playing as it is.
    bool fromPlayer_ = true;
    struct OverrideTrack {
        bool include = true, normalise = false;
        float gain = 1.0f, low = 0.0f, mid = 0.0f, high = 0.0f, pitch = 0.0f;
    };
    std::vector<OverrideTrack> overrideTracks_;
    int overrideBands_[7]{};
    bool overrideNormalise_ = false;
    float overrideTarget_ = -14.0f;
    bool keepPitch_ = true, statsOn_ = false;
    double avOffset_ = 0.0;
    DWORD statsRefreshed_ = 0;
    // Writing the shader file and making mpv rebuild it costs far more than
    // moving a slider does, so a drag sets this and the tick applies it once
    // the slider has been still for a moment.
    DWORD sharpenDueAt_ = 0;
    DWORD captionsDueAt_ = 0;      // a caption file is due to be rewritten
    DWORD pictureDueAt_ = 0;       // a clip's picture settings are due
    // Which piece of the edit the playhead is standing in. Its settings are
    // pushed to the mixer and to mpv when it changes, and only then.
    int liveSegment_ = -2;
    // The caption file mpv is showing, alternated between two names for the
    // same reason the sharpen shader is: mpv keys what it has loaded on the
    // path, so rewriting one file leaves the old captions on screen.
    int captionSlot_ = 0;
    bool captionsShown_ = false;
    POINT panFrom_{}, pressedAt_{};
    bool panning_ = false;
    // A drag over the picture pans it; a click without a drag plays or pauses.
    // Telling them apart is what this remembers.
    bool pressedVideo_ = false, movedWhilePressed_ = false;
    // Which tracks the editor is holding silent right now, and where their
    // faders were before it did.
    std::vector<char> mutedByEdit_;
    std::vector<float> gainBeforeEdit_;
    bool infoStale_ = true;
    bool wavesWanted_ = false, thumbsWanted_ = false;
    double lastPositionSaved_ = 0.0;

    std::wstring osd_;
    DWORD osdUntil_ = 0;
};

// ---------------------------------------------------------------- lifecycle

bool Player::create(HINSTANCE instance, const std::wstring& openThis) {
    instance_ = instance;
    vp::Settings& s = vp::settings();
    s.load();
    // Before any of the interface exists, so nothing is built in the wrong
    // language and then left that way.
    vp::loadLanguage(s.language);

    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    cls.lpfnWndProc = &Player::proc;
    cls.hInstance = instance;
    cls.hCursor = LoadCursor(nullptr, IDC_ARROW);
    cls.hbrBackground = CreateSolidBrush(kBack);
    cls.lpszClassName = L"VideoPlayerCppMain";
    RegisterClassExW(&cls);

    // WS_CLIPCHILDREN keeps this window's painting out of the picture's window.
    // Without it every repaint of the panel or the transport also brushed over
    // the frame mpv had just drawn, which is what made the picture flicker.
    window_ = CreateWindowExW(WS_EX_ACCEPTFILES, cls.lpszClassName, L"Video Player",
                              WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                              CW_USEDEFAULT, 1500, 900, nullptr, nullptr, instance,
                              this);
    if (!window_) return false;

    WNDCLASSEXW host{};
    host.cbSize = sizeof(host);
    host.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    host.lpfnWndProc = &videoHostProc;
    host.hInstance = instance;
    host.hbrBackground = CreateSolidBrush(RGB(0, 0, 0));
    host.lpszClassName = L"VideoPlayerCppVideo";
    RegisterClassExW(&host);
    videoHost_ = CreateWindowExW(0, host.lpszClassName, L"", WS_CHILD | WS_CLIPSIBLINGS,
                                 0, 0, 100, 100,
                                 window_, nullptr, instance, nullptr);

    fonts_.create();

    adjust_ = {
        {L"Contrast", "contrast", AdjustKind::Property, -100, 100, s.contrast, {}},
        {L"Brightness", "brightness", AdjustKind::Property, -100, 100, s.brightness, {}},
        {L"Saturation", "saturation", AdjustKind::Property, -100, 100, s.saturation, {}},
        {L"Gamma", "gamma", AdjustKind::Property, -100, 100, s.gamma, {}},
        {L"Hue", "hue", AdjustKind::Property, -100, 100, s.hue, {}},
        {L"Sharpness", nullptr, AdjustKind::Sharpen, 0, 200, s.sharpen, {}},
        {L"Denoise", nullptr, AdjustKind::Denoise, 0, 100, s.denoise, {}},
        {L"Zoom", nullptr, AdjustKind::Zoom, 25, 400, 100, {}},
        {L"Across", nullptr, AdjustKind::PanX, -100, 100, 0, {}},
        {L"Down", nullptr, AdjustKind::PanY, -100, 100, 0, {}},
    };

    if (!video_.start(videoHost_, &startupError_)) {
        // Not fatal: sound still plays and the panel says what is missing.
    }

    // Never silent to begin with. A player that opens at nothing looks broken
    // for the second it takes to work out why, and the figure it was left at is
    // usually an accident of the last wheel turn before it was closed.
    volume_level_ = s.volume > 0.005f ? s.volume : 0.20f;
    audio_.setMasterGain(volume_level_);
    audio_.setNormalise(s.normalise);
    for (int i = 0; i < static_cast<int>(_countof(kTargets)); ++i) {
        if (std::fabs(kTargets[i] - s.normaliseTarget) < 0.01f) targetIndex_ = i;
    }
    audio_.setNormaliseTarget(kTargets[targetIndex_]);
    fitIndex_ = std::max(0, std::min(2, s.fitMode));
    editorOpen_ = s.editorByDefault;
    keepPitch_ = s.keepPitch;
    avOffset_ = s.avOffset;
    audio_.setKeepPitch(keepPitch_);
    if (s.alwaysOnTop) {
        SetWindowPos(window_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }

    for (size_t i = 0; i < adjust_.size(); ++i) applyAdjust(i);
    applySharpen();
    sharpenDueAt_ = 0;

    layout();
    ShowWindow(window_, SW_SHOW);
    if (s.startFullscreen) toggleFullscreen();
    startClock();
    if (!openThis.empty()) open(openThis);
    return true;
}

void Player::open(const std::wstring& path) {
    rememberPosition();
    waveJob_.cancel();
    thumbJob_.cancel();
    thumbJob_.clear();
    loudnessJob_.cancel();
    wavesWanted_ = thumbsWanted_ = false;

    std::string error;
    if (!audio_.open(path, &error)) {
        MessageBoxW(window_, widen(error).c_str(), L"Video Player", MB_ICONWARNING);
    }
    audio_.setMasterGain(volume_level_);

    path_ = path;
    const size_t slash = path.find_last_of(L"\\/");
    fileName_ = (slash == std::wstring::npos) ? path : path.substr(slash + 1);
    if (slash != std::wstring::npos) {
        vp::settings().lastFolder = vp::toUtf8(path.substr(0, slash));
    }
    SetWindowTextW(window_, (fileName_ + L"  -  Video Player").c_str());

    const size_t n = audio_.tracks().size();
    trackScroll_ = 0;
    trackEnable_.assign(n, Hit{});
    trackSolo_.assign(n, Hit{});
    trackNorm_.assign(n, Hit{});
    trackExpand_.assign(n, Hit{});
    trackVolume_.assign(n, Hit{});
    eqLow_.assign(n, Hit{});
    eqMid_.assign(n, Hit{});
    eqHigh_.assign(n, Hit{});
    eqPitch_.assign(n, Hit{});
    eqReset_.assign(n, Hit{});
    expanded_.assign(n, 0);
    mutedByEdit_.assign(n, 0);
    gainBeforeEdit_.assign(n, 1.0f);

    std::vector<std::string> names;
    for (const vp::TrackInfo& t : audio_.tracks()) {
        names.push_back("A" + std::to_string(names.size() + 1));
    }
    timeline_.setMedia(audio_.duration(), names);

    // Shown only now: an empty mpv window over the middle of the program is
    // what was flickering there before a file was ever opened.
    ShowWindow(videoHost_, SW_SHOW);
    // A new file starts on the card again; the copy back is turned on
    // only once something actually needs to filter a frame.
    hwdecCopy_ = false;
    overrideTracks_.clear();       // seeded again from this file
    video_.loadFile(path);
    video_.setPaused(false);
    video_.setBaseSpeed(kSpeeds[speedIndex_]);
    audio_.setSpeed(kSpeeds[speedIndex_]);
    applyGeometry();
    applyVideoFilters();

    // Where it was left last time, if it was left anywhere worth returning to.
    const double resume = vp::settings().positionFor(path);
    audio_.seek(resume);
    if (resume > 0.0) {
        video_.seek(resume);
        say(tr(L"Resuming at ") + formatTime(resume));
    }
    audio_.play();

    // Loudness is measured whatever the editor is doing: normalisation in the
    // export wants a real figure rather than the running estimate, and it costs
    // one idle-priority pass that nobody notices.
    if (!vp::ffmpegPath().empty() && n > 0) {
        std::vector<int> streams;
        for (const vp::TrackInfo& t : audio_.tracks()) streams.push_back(t.audioIndex);
        loudnessJob_.start(path, streams);
    }
    if (editorOpen_) buildPreviews(false);
    captionsShown_ = false;
    refreshCaptions();

    infoStale_ = true;
    layout();
    InvalidateRect(window_, nullptr, TRUE);
}

void Player::choose() {
    wchar_t file[MAX_PATH]{};
    const std::wstring folder = vp::fromUtf8(vp::settings().lastFolder);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = window_;
    ofn.lpstrFilter = L"Video\0*.mp4;*.mkv;*.mov;*.avi;*.webm;*.m4v;*.ts\0Vse\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    if (!folder.empty()) ofn.lpstrInitialDir = folder.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn)) open(file);
}

void Player::rememberPosition() {
    if (path_.empty()) return;
    const double at = audio_.position();
    // Not right at the end: a file watched to the last minute should open at
    // the start next time, not at the credits.
    vp::settings().rememberPositionFor(path_,
                                       at > audio_.duration() - 30.0 ? 0.0 : at);
}

// ------------------------------------------------------------------- layout

// The picture, in the coordinates of the main window. One place, because the
// hit testing, the placeholder and the on-screen messages all need it and they
// were each working it out slightly differently.
// The space the picture is given, which is not the same as the picture: the
// canvas inside it is only as big as the frame, so everything else on the
// window still knows where the transport begins.
RECT Player::videoArea() const { return videoStage_; }

void Player::layout() {
    RECT client{};
    GetClientRect(window_, &client);
    const int panelLeft = isFull_ ? client.right : client.right - kPanelWidth;
    const int wide = panelLeft - 36;

    // One row when the window is wide enough to hold the controls side by side,
    // which is the arrangement of the version this replaces; two only when they
    // would otherwise be squeezed into targets nobody can hit.
    twoRows_ = wide < kOneRowNeeds;
    const int transport = isFull_ ? 0 : (twoRows_ ? kTransportTwo : kTransportOne);

    const int editorHeight =
        (editorOpen_ && !isFull_) ? kEditorBar + timeline_.heightFor() + 10 : 0;
    const int videoBottom =
        isFull_ ? client.bottom : client.bottom - transport - editorHeight;
    videoStage_ = {0, 0, std::max(1, panelLeft), std::max(1, videoBottom)};
    placeCanvas();

    // ---- transport, directly under the picture
    const int y = videoBottom;
    seek_.r = {14, y + 8, panelLeft - 14, y + 22};

    const int row = y + 30;
    const int rowH = 30;
    int x = 14;
    auto put = [&](Hit& h, int w, int gap = 3) {
        h.r = {x, row, x + w, row + rowH};
        x += w + gap;
    };
    put(play_, 34);
    put(back_, 30);
    put(stepBack_, 30);
    put(stepForward_, 30);
    put(forward_, 30, 8);
    // The time reads between the transport and the speed, as it did before.
    x += 118;
    put(first_, 0, 0);
    put(last_, 0, 0);
    x += 8;
    put(speedChip_, 84);
    put(pitchChip_, 86, 8);
    put(rotateLeft_, 30);
    put(rotateRight_, 30);
    put(flipHChip_, 92);
    put(flipVChip_, 128, 6);
    put(orientChip_, 96, 6);
    put(fitChip_, 92, 6);
    put(zoomReset_, 92, 6);
    put(loopChip_, 90);

    // The right-hand group is placed from the right edge inwards, so it stays
    // put whatever happens to the left of it.
    int rx = panelLeft - 14;
    const int rightRow = twoRows_ ? y + 30 : row;
    auto putRight = [&](Hit& h, int w, int gap = 3) {
        h.r = {rx - w, rightRow, rx, rightRow + rowH};
        rx -= w + gap;
    };
    putRight(full_, 30);
    putRight(gear_, 96);
    putRight(statsChip_, 30, 6);
    putRight(editorChip_, 88, 8);
    rx -= 40;                       // the reading of the volume
    putRight(volume_, 104, 3);
    volume_.r.top = rightRow + (rowH - 16) / 2;
    volume_.r.bottom = volume_.r.top + 16;
    putRight(muteButton_, 30);

    if (twoRows_) {
        // Playing on the first row, the shape of the picture on the second.
        const int second = y + 30 + rowH + 6;
        int gx = 14;
        auto geo = [&](Hit& h, int w, int gap = 3) {
            h.r = {gx, second, gx + w, second + rowH};
            gx += w + gap;
        };
        // Everything after the speed moves down; the seek row keeps the
        // transport, the time, the speed and the pitch.
        x = 14;
        put(play_, 34);
        put(back_, 30);
        put(stepBack_, 30);
        put(stepForward_, 30);
        put(forward_, 30, 8);
        x += 118;
        x += 8;
        put(speedChip_, 84);
        put(pitchChip_, 86);

        geo(rotateLeft_, 30);
        geo(rotateRight_, 30);
        geo(flipHChip_, 92);
        geo(flipVChip_, 128, 8);
        geo(orientChip_, 96, 6);
        geo(fitChip_, 92, 6);
        geo(zoomReset_, 92, 6);
        geo(loopChip_, 90);
    }

    // ---- editor, below the transport
    if (editorHeight > 0) {
        const int barTop = client.bottom - editorHeight + 8;
        int ex = 14;
        int rowTop = barTop;
        auto putEdit = [&](Hit& h, int w, int gap = 5) {
            h.r = {ex, rowTop, ex + w, rowTop + 26};
            ex += w + gap;
        };
        // Cutting on the first row, everything that opens something on the
        // second: the two are reached at different moments and reading either
        // one is easier when it is not mixed with the other.
        putEdit(editSplit_, 108, 4);
        putEdit(editDelete_, 92, 4);
        putEdit(editRestore_, 96, 4);
        putEdit(editResetCuts_, 130, 12);
        putEdit(editMarkIn_, 94, 4);
        putEdit(editMarkOut_, 86, 4);
        putEdit(editClearMarks_, 98);

        ex = 14;
        rowTop = barTop + 30;
        putEdit(editAddText_, 96, 4);
        putEdit(editPreviews_, 126, 12);
        putEdit(editSaveProject_, 108, 4);
        putEdit(editOpenProject_, 112);

        // The two property windows sit at the right-hand end, beside Render:
        // they are what the row is opened for, and the cutting is on the row
        // above. The one for a single row only appears when a single row has
        // been picked, because that is the only time it means anything.
        editRender_.r = {panelLeft - 110, barTop + 30, panelLeft - 14, barTop + 56};
        editClipProps_.r = {editRender_.r.left - 158, barTop + 30,
                            editRender_.r.left - 12, barTop + 56};
        // Always laid out, drawn only when a row is picked: deciding it here
        // meant it could not appear, because the layout does not run again
        // when something is selected.
        editLocalProps_.r = {editClipProps_.r.left - 156, barTop + 30,
                             editClipProps_.r.left - 6, barTop + 56};

        RECT strip{10, barTop + kEditorBar - 8, panelLeft - 10, client.bottom - 6};
        timeline_.setBounds(strip);
    } else {
        timeline_.setBounds(RECT{0, 0, 0, 0});
    }

    // ---- panel
    const int px = panelLeft + 14;
    const int pw = kPanelWidth - 28;
    open_.r = {px + pw - 92, 14, px + pw, 42};

    // heading + normalise, then the preset on its own line: on one line the two
    // ran into each other as soon as a preset had a name worth reading.
    normChip_.r = {px + pw - 236, 98, px + pw - 126, 124};
    targetChip_.r = {px + pw - 120, 98, px + pw, 124};

    audioPresetChip_.r = {px + 84, 130, px + pw - 66, 156};
    audioPresetSave_.r = {px + pw - 60, 130, px + pw - 32, 156};
    audioPresetDelete_.r = {px + pw - 28, 130, px + pw, 156};

    // What is left over between the preset row and the picture section is what
    // the tracks get; more tracks than that scrolls rather than overflowing.
    int tracksBottom = client.bottom - 20 -
                       (kAdjustRow * static_cast<int>(adjust_.size()) + 56);
    tracksBottom = std::max(tracksBottom, 220);
    tracksView_ = {px, 164, px + pw, tracksBottom};

    trackEnable_.resize(expanded_.size());
    trackSolo_.resize(expanded_.size());
    trackNorm_.resize(expanded_.size());
    trackExpand_.resize(expanded_.size());
    trackVolume_.resize(expanded_.size());
    eqLow_.resize(expanded_.size());
    eqMid_.resize(expanded_.size());
    eqHigh_.resize(expanded_.size());
    eqPitch_.resize(expanded_.size());
    eqReset_.resize(expanded_.size());

    int ty = tracksView_.top - trackScroll_;
    for (size_t i = 0; i < expanded_.size(); ++i) {
        const int top = ty;
        // The switches sit on the top line beside the name, so the line of
        // figures underneath has the whole width of the row instead of being
        // squeezed into what was left beside them and cut off.
        trackEnable_[i].r = {px + 10, top + 11, px + 26, top + 27};
        trackSolo_[i].r = {px + pw - 240, top + 6, px + pw - 214, top + 32};
        trackNorm_[i].r = {px + pw - 210, top + 6, px + pw - 130, top + 32};
        trackVolume_[i].r = {px + pw - 124, top + 11, px + pw - 62, top + 27};
        trackExpand_[i].r = {px + pw - 32, top + 6, px + pw - 6, top + 32};

        const int e = top + kTrackRow;
        eqLow_[i].r = {px + 58, e + 2, px + pw - 74, e + 16};
        eqMid_[i].r = {px + 58, e + 24, px + pw - 74, e + 38};
        eqHigh_[i].r = {px + 58, e + 46, px + pw - 74, e + 60};
        eqPitch_[i].r = {px + 58, e + 70, px + pw - 74, e + 84};
        eqReset_[i].r = {px + pw - 66, e + 30, px + pw - 8, e + 54};

        ty += (expanded_[i] ? kTrackOpen : kTrackRow) + 5;
    }
    tracksContent_ = ty + trackScroll_ - tracksView_.top;

    // ---- picture, pinned to the bottom of the panel
    const int adjTop = tracksBottom + 44;
    videoPresetChip_.r = {px + 56, adjTop - 30, px + pw - 158, adjTop - 4};
    videoPresetSave_.r = {px + pw - 152, adjTop - 30, px + pw - 124, adjTop - 4};
    videoPresetDelete_.r = {px + pw - 120, adjTop - 30, px + pw - 92, adjTop - 4};
    resetPicture_.r = {px + pw - 86, adjTop - 30, px + pw, adjTop - 4};
    for (size_t i = 0; i < adjust_.size(); ++i) {
        const int top = adjTop + 12 + static_cast<int>(i) * kAdjustRow;
        adjust_[i].slider.r = {px + 84, top, px + pw - 52, top + 16};
    }
}

// ------------------------------------------------------------------ painting

void Player::paint(HDC dc) {
    RECT client{};
    GetClientRect(window_, &client);
    if (isFull_) {
        paintOsd(dc, {0, 0, client.right, client.bottom});
        return;
    }

    // What the window was asked to repaint. Whatever falls outside it is left
    // as it was in the buffer, because only the asked-for part is copied to the
    // screen anyway - and drawing the rest was what made a drag stutter.
    auto needed = [this](const RECT& r) {
        RECT both{};
        return IntersectRect(&both, &r, &paintClip_) != 0;
    };

    RECT panel{client.right - kPanelWidth, 0, client.right, client.bottom};
    if (needed(panel)) {
        fill(dc, panel, kBack);
        paintPanel(dc, panel);
    }

    const RECT video = videoArea();
    if (!path_.empty() && needed(video)) {
        // The letterbox. The picture's own window sits inside this and is
        // clipped away from it, so nothing is drawn twice.
        fill(dc, video, RGB(0x08, 0x09, 0x0c));

        // And a line round the canvas: everything inside it is the file that
        // will be written, everything outside is only the room it sits in. A
        // picture pushed past this line is cut here and cut in the export.
        // One pixel clear of the frame on every side: the line marks the
        // canvas, it is not part of the picture, and the export knows nothing
        // about it - what is written is the file's own resolution.
        RECT edge{videoCanvas_.left - 2, videoCanvas_.top - 2,
                  videoCanvas_.right + 2, videoCanvas_.bottom + 2};
        HRGN hold = CreateRectRgn(video.left, video.top, video.right, video.bottom);
        SelectClipRgn(dc, hold);
        HPEN pen = CreatePen(PS_SOLID, 1, kLineHot);
        HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        HGDIOBJ op = SelectObject(dc, pen);
        Rectangle(dc, edge.left, edge.top, edge.right, edge.bottom);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(pen);

        // Corners, so the canvas is legible even against a bright picture.
        const int arm = 14;
        auto corner = [&](int x, int y, int dx, int dy) {
            RECT a{x, y, x + dx * arm, y + dy * 2};
            RECT b{x, y, x + dx * 2, y + dy * arm};
            if (a.left > a.right) std::swap(a.left, a.right);
            if (a.top > a.bottom) std::swap(a.top, a.bottom);
            if (b.left > b.right) std::swap(b.left, b.right);
            if (b.top > b.bottom) std::swap(b.top, b.bottom);
            fill(dc, a, kAccentLit);
            fill(dc, b, kAccentLit);
        };
        corner(edge.left, edge.top, 1, 1);
        corner(edge.right, edge.top, -1, 1);
        corner(edge.left, edge.bottom, 1, -1);
        corner(edge.right, edge.bottom, -1, -1);
        SelectClipRgn(dc, nullptr);
        DeleteObject(hold);
    }

    // With no file open there is nothing for mpv to draw and its window is
    // hidden, so the space it would occupy is painted here instead. Leaving the
    // empty child window in place is what made that area flicker.
    if (path_.empty()) {
        RECT hole{0, 0, client.right - kPanelWidth, video.bottom};
        if (!needed(hole)) return;
        fill(dc, hole, RGB(0x0b, 0x0c, 0x0f));
        RECT line{hole.left, (hole.top + hole.bottom) / 2 - 16, hole.right,
                  (hole.top + hole.bottom) / 2 + 16};
        text(dc, line, tr(L"Open a video   \x00b7   Ctrl+O   \x00b7   or drop one here"),
             kTextFaint, DT_CENTER | DT_SINGLELINE | DT_VCENTER, fonts_.ui);
    }

    const int transportH = twoRows_ ? kTransportTwo : kTransportOne;
    RECT transport{0, video.bottom, client.right - kPanelWidth,
                   video.bottom + transportH};
    if (needed(transport)) {
        fill(dc, transport, kPanel);
        RECT edge{transport.left, transport.top, transport.right, transport.top + 1};
        fill(dc, edge, kLine);
        paintTransport(dc, transport);
    }

    if (editorOpen_) {
        RECT bar{0, transport.bottom, client.right - kPanelWidth, client.bottom};
        if (needed(bar)) {
            fill(dc, bar, kPanel);
            RECT rule{bar.left, bar.top, bar.right, bar.top + 1};
            fill(dc, rule, kLine);
            paintEditorBar(dc);
            timeline_.paint(dc, audio_.position(), mouse_, &fonts_);
        }
    }

    paintOsd(dc, video);
}

void Player::paintPanel(HDC dc, const RECT& r) {
    const int x = r.left + 14;
    const int w = kPanelWidth - 28;

    text(dc, {x, 14, open_.r.left - 10, 42}, L"Video Player", kText,
         DT_LEFT | DT_SINGLELINE | DT_VCENTER, fonts_.bold);
    roundBox(dc, open_.r, open_.has(mouse_) ? kAccentDk : kAccent, kAccentDk, 6);
    text(dc, open_.r, tr(L"Open\x2026"), RGB(255, 255, 255),
         DT_CENTER | DT_SINGLELINE | DT_VCENTER, fonts_.semi);

    text(dc, {x, 46, r.right - 14, 64},
         fileName_.empty() ? L"—" : fileName_, kTextDim,
         DT_LEFT | DT_SINGLELINE | DT_PATH_ELLIPSIS, fonts_.ui);
    text(dc, {x, 64, r.right - 14, 80}, infoTop_, kTextFaint,
         DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS, fonts_.tiny);
    text(dc, {x, 80, r.right - 14, 96}, infoBottom_, kTextFaint,
         DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS, fonts_.tiny);

    // Where the first row is drawn has to be where the layout put its hit
    // rectangles, scroll and all, or a click lands on the row above.
    int y = tracksView_.top - trackScroll_;
    paintTracks(dc, x, w, y);
    paintPicture(dc, x, w, y);

    if (!startupError_.empty()) {
        text(dc, {x, r.bottom - 40, x + w, r.bottom - 6}, widen(startupError_), kWarn,
             DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS, fonts_.tiny);
    }
}

void Player::paintTracks(HDC dc, int x, int w, int& y) {
    text(dc, {x, 98, normChip_.r.left - 10, 124}, tr(L"Audio tracks"), kText,
         DT_LEFT | DT_SINGLELINE | DT_VCENTER, fonts_.semi);

    const bool norm = audio_.normalising();
    RECT normBox{normChip_.r.left, (normChip_.r.top + normChip_.r.bottom) / 2 - 8,
                 normChip_.r.left + 16, (normChip_.r.top + normChip_.r.bottom) / 2 + 8};
    checkBox(dc, normBox, norm, normChip_.has(mouse_));
    text(dc, {normBox.right + 8, normChip_.r.top, normChip_.r.right, normChip_.r.bottom},
         tr(L"Normalize"), norm ? kText : kTextDim,
         DT_LEFT | DT_SINGLELINE | DT_VCENTER, fonts_.ui);

    roundBox(dc, targetChip_.r, targetChip_.has(mouse_) ? kRowHot : kSection,
             targetChip_.has(mouse_) ? kLineHot : kLine, 6);
    wchar_t target[24];
    _snwprintf_s(target, _TRUNCATE, L"%g LUFS", kTargets[targetIndex_]);
    text(dc, targetChip_.r, target, norm ? kText : kTextDim,
         DT_CENTER | DT_SINGLELINE | DT_VCENTER, fonts_.ui);

    // ---- preset row
    text(dc, {x, audioPresetChip_.r.top, x + 80, audioPresetChip_.r.bottom},
         tr(L"Audio preset"), kTextDim, DT_LEFT | DT_SINGLELINE | DT_VCENTER, fonts_.ui);
    const auto& presets = vp::settings().audioPresets;
    const std::wstring presetName =
        audioPresetIndex_ >= 0 && audioPresetIndex_ < static_cast<int>(presets.size())
            ? widen(presets[static_cast<size_t>(audioPresetIndex_)].name)
            : tr(L"\x2014 none \x2014");
    auto field = [&](const Hit& h, const std::wstring& label, COLORREF colour,
                     HFONT font) {
        roundBox(dc, h.r, h.has(mouse_) ? kRowHot : kSection,
                 h.has(mouse_) ? kLineHot : kLine, 6);
        text(dc, h.r, label, colour, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS,
             font);
    };
    field(audioPresetChip_, presetName, kText, fonts_.ui);
    field(audioPresetSave_, L"\xff0b", kText, fonts_.ui);
    field(audioPresetDelete_, L"\x2715", kTextDim, fonts_.ui);

    const bool solo = audio_.anySolo();

    // The list is clipped to its own box, so a file with a dozen tracks scrolls
    // instead of running over the picture controls under it.
    const HRGN clip = CreateRectRgn(tracksView_.left, tracksView_.top,
                                    tracksView_.right, tracksView_.bottom);
    SelectClipRgn(dc, clip);

    for (size_t row = 0; row < audio_.tracks().size() && row < trackEnable_.size();
         ++row) {
        const size_t i = panelTrack(row);
        const int height = expanded_[row] ? kTrackOpen : kTrackRow;
        RECT box{x, y, x + w, y + height};
        // Anything scrolled entirely out of sight costs nothing to skip.
        if (box.bottom < tracksView_.top || box.top > tracksView_.bottom) {
            y += height + 5;
            continue;
        }
        const bool hot = PtInRect(&box, mouse_) && PtInRect(&tracksView_, mouse_);
        roundBox(dc, box, kSection, hot ? kLineHot : kLine, 6);

        auto& c = audio_.controls(i);
        const bool on = solo ? c.solo.load() : c.enabled.load();

        checkBox(dc, trackEnable_[row].r, c.enabled.load(), trackEnable_[row].has(mouse_));

        // The meter next to the switch, standing up, as it did before: it is
        // read at a glance and a vertical bar is what that shape means.
        const float peak = c.peak.load();
        // In decibels, like every meter anybody has used: full scale at the
        // top, fifty below it at the bottom. On amplitude a levelled track sat
        // a fifth of the way up and green, which said nothing.
        RECT meter{x + 32, y + 10, x + 38, y + height - 12};
        roundFill(dc, meter, kTrack, 3);
        if (on && peak > 0.0005f) {
            const double db = 20.0 * std::log10(std::min(1.0f, peak));
            const double part = std::max(0.0, std::min(1.0, (db + 50.0) / 50.0));
            RECT lit = meter;
            lit.top = meter.bottom -
                      static_cast<int>((meter.bottom - meter.top) * part);
            // Red from three below, amber from twelve: a track brought to the
            // levelling target lands on the amber, which is where it belongs.
            const COLORREF colour = db > -3.0 ? kRed : (db > -12.0 ? kWarn : kGreen);
            roundFill(dc, lit, colour, 3);
        }

        wchar_t title[96];
        _snwprintf_s(title, _TRUNCATE, L"#%d  %s", static_cast<int>(i) + 1,
                     widen(audio_.tracks()[i].name).c_str());
        const int textRight = trackSolo_[row].r.left - 10;
        text(dc, {x + 46, y + 6, textRight, y + 26}, title, on ? kText : kTextDim,
             DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS, fonts_.semi);

        wchar_t sub[128];
        const float lift = c.normalisationDb.load();
        const bool levelling = audio_.normalising() && c.normalise.load();
        const bool lifting = levelling && std::fabs(lift) > 0.05f;
        // Switched on but still listening: say so, rather than looking like
        // nothing happened.
        const bool measuring = levelling && !c.levelReady.load();
        const double kHz = audio_.tracks()[i].sourceRate / 1000.0;
        if (measuring) {
            _snwprintf_s(sub, _TRUNCATE, tr(L"%.4g kHz \x00b7 %d ch. \x00b7 normalizing, listening\x2026"),
                         kHz, audio_.tracks()[i].sourceChannels);
        } else if (lifting) {
            _snwprintf_s(sub, _TRUNCATE,
                         tr(L"%.4g kHz \x00b7 %d ch. \x00b7 %.1f \x2192 %+.1f dB"),
                         kHz, audio_.tracks()[i].sourceChannels,
                         c.measuredDb.load(), lift);
        } else {
            _snwprintf_s(sub, _TRUNCATE, tr(L"%.4g kHz \x00b7 %d ch."), kHz,
                         audio_.tracks()[i].sourceChannels);
        }
        text(dc, {x + 46, y + 30, x + w - 10, y + 48}, sub,
             (measuring || lifting) ? kTextDim : kTextFaint,
             DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS, fonts_.tiny);

        // ---- solo, normalise, fader
        const bool isSolo = c.solo.load();
        roundBox(dc, trackSolo_[row].r, isSolo ? kAccentDim : kSection,
                 isSolo ? kAccent : (trackSolo_[row].has(mouse_) ? kLineHot : kLine), 5);
        text(dc, trackSolo_[row].r, L"S", isSolo ? RGB(255, 255, 255) : kTextDim,
             DT_CENTER | DT_SINGLELINE | DT_VCENTER, fonts_.semi);

        // The switch that was missing: normalising this one track. Drawn solid
        // rather than outlined when it is doing something, because which tracks
        // are being levelled is the thing this row is asked about most, and an
        // outline the same weight as everything else around it did not answer.
        const bool trackNorm = c.normalise.load();
        const bool normLive = trackNorm && norm;
        roundBox(dc, trackNorm_[row].r,
                 normLive ? kAccent : (trackNorm_[row].has(mouse_) ? kRowHot : kSection),
                 normLive ? kAccentLit : (trackNorm_[row].has(mouse_) ? kLineHot : kLine), 5);
        text(dc, trackNorm_[row].r, normLive ? tr(L"NORMALIZE") : tr(L"normalize"),
             normLive ? RGB(255, 255, 255) : (norm || trackNorm ? kTextDim : kTextFaint),
             DT_CENTER | DT_SINGLELINE | DT_VCENTER, fonts_.tiny);
        // Levelling is on for this track but off overall: the row would
        // otherwise look the same as one that is simply switched off.
        if (trackNorm && !norm) {
            RECT waiting{trackNorm_[row].r.left + 4, trackNorm_[row].r.bottom - 4,
                         trackNorm_[row].r.right - 4, trackNorm_[row].r.bottom - 2};
            fill(dc, waiting, kAccentDim);
        }

        // The fader runs to 200 %, as it did before: a quiet track has to be
        // able to reach the others rather than stopping at its own level.
        const float gain = c.gain.load();
        slider(dc, trackVolume_[row].r, gain / 2.0f, on, false);
        wchar_t pct[16];
        _snwprintf_s(pct, _TRUNCATE, L"%d%%", static_cast<int>(gain * 100.0f + 0.5f));
        text(dc, {trackVolume_[row].r.right + 2, trackVolume_[row].r.top - 4,
                  trackExpand_[row].r.left - 2, trackVolume_[row].r.bottom + 4},
             pct, kTextDim, DT_RIGHT | DT_SINGLELINE | DT_VCENTER, fonts_.mono);

        const bool eqLive = std::fabs(c.eqLow.load()) > 0.05f ||
                            std::fabs(c.eqMid.load()) > 0.05f ||
                            std::fabs(c.eqHigh.load()) > 0.05f ||
                            std::fabs(c.pitch.load()) > 0.05f;
        roundBox(dc, trackExpand_[row].r,
                 expanded_[row] ? kAccentDim : kSection,
                 expanded_[row] || eqLive ? kAccent
                                        : (trackExpand_[row].has(mouse_) ? kLineHot : kLine),
                 5);
        text(dc, trackExpand_[row].r, L"EQ",
             expanded_[row] || eqLive ? RGB(255, 255, 255) : kTextDim,
             DT_CENTER | DT_SINGLELINE | DT_VCENTER, fonts_.tiny);

        if (expanded_[row]) {
            const struct { const wchar_t* name; Hit* hit; float value; } bands[] = {
                {tr(L"Bass"), &eqLow_[row], c.eqLow.load()},
                {tr(L"Mids"), &eqMid_[row], c.eqMid.load()},
                {tr(L"Treble"), &eqHigh_[row], c.eqHigh.load()},
            };
            for (const auto& b : bands) {
                text(dc, {x + 10, b.hit->r.top - 2, x + 54, b.hit->r.bottom + 2}, b.name,
                     kTextDim, DT_LEFT | DT_SINGLELINE | DT_VCENTER, fonts_.tiny);
                slider(dc, b.hit->r, (b.value + 12.0) / 24.0, std::fabs(b.value) > 0.05f,
                       true);
                wchar_t db[16];
                _snwprintf_s(db, _TRUNCATE, L"%+.1f", b.value);
                text(dc, {b.hit->r.right + 4, b.hit->r.top - 2, b.hit->r.right + 40,
                          b.hit->r.bottom + 2},
                     std::fabs(b.value) > 0.05f ? db : L"0", kTextFaint,
                     DT_LEFT | DT_SINGLELINE | DT_VCENTER, fonts_.tiny);
            }

            // Pitch belongs with the bands: it is the fourth thing anyone does
            // to one track against the others, and it is the only one of the
            // four that the speed control cannot already do.
            const float semitones = c.pitch.load();
            text(dc, {x + 10, eqPitch_[row].r.top - 2, x + 54, eqPitch_[row].r.bottom + 2},
                 tr(L"Pitch"), kTextDim, DT_LEFT | DT_SINGLELINE | DT_VCENTER, fonts_.tiny);
            slider(dc, eqPitch_[row].r, (semitones + 12.0) / 24.0,
                   std::fabs(semitones) > 0.05f, true);
            wchar_t note[24];
            _snwprintf_s(note, _TRUNCATE, L"%+.0f", semitones);
            text(dc, {eqPitch_[row].r.right + 4, eqPitch_[row].r.top - 2,
                      eqPitch_[row].r.right + 40, eqPitch_[row].r.bottom + 2},
                 std::fabs(semitones) > 0.05f ? note : L"0", kTextFaint,
                 DT_LEFT | DT_SINGLELINE | DT_VCENTER, fonts_.tiny);

            roundBox(dc, eqReset_[row].r, eqReset_[row].has(mouse_) ? kRowHot : kSection,
                     eqReset_[row].has(mouse_) ? kLineHot : kLine, 5);
            text(dc, eqReset_[row].r, L"Reset", kTextDim,
                 DT_CENTER | DT_SINGLELINE | DT_VCENTER, fonts_.tiny);
        }

        y += height + 5;
    }

    if (audio_.tracks().empty()) {
        text(dc, tracksView_, tr(L"no audio tracks"), kTextFaint,
             DT_CENTER | DT_SINGLELINE | DT_VCENTER, fonts_.ui);
    }

    SelectClipRgn(dc, nullptr);
    DeleteObject(clip);

    // A hint that the list goes on, in the one place there is room for it.
    if (tracksContent_ > tracksView_.bottom - tracksView_.top) {
        const int span = tracksView_.bottom - tracksView_.top;
        const int barTop = tracksView_.top +
                           span * trackScroll_ / std::max(1, tracksContent_);
        const int barLen = std::max(24, span * span / std::max(1, tracksContent_));
        RECT bar{tracksView_.right - 4, barTop, tracksView_.right - 1,
                 std::min<LONG>(tracksView_.bottom, barTop + barLen)};
        roundFill(dc, bar, kLineHot, 2);
    }

    y = tracksView_.bottom;
}

void Player::paintPicture(HDC dc, int x, int w, int& y) {
    const int head = videoPresetChip_.r.top;
    text(dc, {x, head, x + 50, head + 26}, tr(L"Picture"), kText,
         DT_LEFT | DT_SINGLELINE | DT_VCENTER, fonts_.semi);

    auto field = [&](const Hit& h, const std::wstring& label, COLORREF colour) {
        roundBox(dc, h.r, h.has(mouse_) ? kRowHot : kSection,
                 h.has(mouse_) ? kLineHot : kLine, 6);
        text(dc, h.r, label, colour,
             DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS, fonts_.ui);
    };

    const auto& presets = vp::settings().videoPresets;
    const std::wstring name =
        videoPresetIndex_ >= 0 && videoPresetIndex_ < static_cast<int>(presets.size())
            ? widen(presets[static_cast<size_t>(videoPresetIndex_)].name)
            : L"\x2014 preset \x2014";
    field(videoPresetChip_, name, kText);
    field(videoPresetSave_, L"\xff0b", kText);
    field(videoPresetDelete_, L"\x2715", kTextDim);
    field(resetPicture_, tr(L"Reset"), kText);

    // The sliders sit in a panel of their own, as they did before, so the block
    // reads as one group rather than as seven loose rows.
    if (!adjust_.empty()) {
        RECT box{x, adjust_.front().slider.r.top - 12, x + w,
                 adjust_.back().slider.r.bottom + 12};
        roundBox(dc, box, kPanel, kLine, 8);
    }

    for (const Adjust& a : adjust_) {
        const RECT& s = a.slider.r;
        text(dc, {x + 12, s.top - 3, x + 78, s.bottom + 3}, tr(a.label), kTextDim,
             DT_LEFT | DT_SINGLELINE | DT_VCENTER, fonts_.ui);
        const bool centred = a.lo < 0;
        const bool live = a.kind == AdjustKind::Zoom ? a.value != 100 : a.value != 0;
        slider(dc, s, static_cast<double>(a.value - a.lo) / (a.hi - a.lo), live, centred);
        wchar_t value[16];
        if (a.kind == AdjustKind::Sharpen) {
            _snwprintf_s(value, _TRUNCATE, L"%.2f", a.value / 100.0);
        } else if (a.kind == AdjustKind::Zoom) {
            _snwprintf_s(value, _TRUNCATE, L"%d %%", a.value);
        } else if (a.kind == AdjustKind::PanX || a.kind == AdjustKind::PanY) {
            _snwprintf_s(value, _TRUNCATE, L"%+d %%", a.value);
        } else if (a.value == 0) {
            wcscpy_s(value, L"0");
        } else {
            _snwprintf_s(value, _TRUNCATE, L"%+d", a.value);
        }
        text(dc, {s.right + 6, s.top - 3, x + w - 10, s.bottom + 3}, value, kTextDim,
             DT_RIGHT | DT_SINGLELINE | DT_VCENTER, fonts_.mono);
    }
    y = adjust_.empty() ? y : adjust_.back().slider.r.bottom + 12;
}

void Player::paintEditorBar(HDC dc) {
    const bool haveMedia = timeline_.duration() > 0.0;
    auto button = [&](const Hit& h, const wchar_t* label, bool primary = false,
                      bool live = true) {
        COLORREF back = primary ? (h.has(mouse_) ? kAccentDk : kAccent)
                                : (h.has(mouse_) ? kRowHot : kSection);
        COLORREF edge = primary ? kAccentDk : (h.has(mouse_) ? kLineHot : kLine);
        if (!live) { back = kPanel; edge = kLine; }
        roundBox(dc, h.r, back, edge, 6);
        text(dc, h.r, label,
             !live ? kTextFaint : (primary ? RGB(255, 255, 255) : kText),
             DT_CENTER | DT_SINGLELINE | DT_VCENTER, primary ? fonts_.semi : fonts_.ui);
    };

    const int barTop = editSplit_.r.top;

    // The one the row exists for, so it carries the row the way Render
    // carries the one below it.
    button(editSplit_, tr(L"Split here"), true, haveMedia);
    button(editDelete_, tr(L"Drop piece"), false, timeline_.selected() >= 0);
    button(editRestore_, tr(L"Restore piece"), false, timeline_.selected() >= 0);
    button(editResetCuts_, tr(L"Clear every cut"), false, haveMedia);
    button(editMarkIn_, tr(L"Start here"), false, haveMedia);
    button(editMarkOut_, tr(L"End here"), false, haveMedia);
    button(editClearMarks_, tr(L"Clear marks"), false,
           timeline_.markIn() >= 0.0 || timeline_.markOut() >= 0.0);
    button(editAddText_, tr(L"Add text"), false, haveMedia);
    button(editClipProps_, tr(L"Clip properties\x2026"), false, haveMedia);
    if (localReady()) {
        wchar_t which[64];
        if (timeline_.selectedRow() == 0) {
            wcscpy_s(which, tr(L"This picture clip\x2026"));
        } else {
            _snwprintf_s(which, _TRUNCATE, tr(L"This A%d clip\x2026"),
                         timeline_.trackForRow(timeline_.selectedRow()) + 1);
        }
        button(editLocalProps_, which, false, true);
    }

    std::wstring previews = tr(L"Build previews");
    if (waveJob_.running() || thumbJob_.running()) {
        wchar_t s[48];
        _snwprintf_s(s, _TRUNCATE, tr(L"Previews %d %%"),
                     std::min(waveJob_.progress(), thumbJob_.progress()));
        previews = s;
    } else if (timeline_.hasWaveforms() || timeline_.hasThumbs()) {
        previews = tr(L"Rebuild previews");
    }
    button(editPreviews_, previews.c_str(), false,
           haveMedia && !vp::ffmpegPath().empty());
    button(editSaveProject_, tr(L"Save work"), false, haveMedia);
    button(editOpenProject_, tr(L"Open work"), false, true);
    button(editRender_, L"Render\x2026", true, haveMedia && !vp::ffmpegPath().empty());

    // The line that says how the strip is worked, word for word from the
    // version this replaces - the gestures it describes are the same ones -
    // with what the cuts add up to at its right-hand end, where there is room
    // for it however many buttons the row above happens to hold.
    const LONG rightEdge =
        localReady() ? editLocalProps_.r.left : editClipProps_.r.left;
    const std::wstring status = timeline_.statusLine();
    RECT total{editPreviews_.r.right + 12, editRender_.r.top, rightEdge - 12,
               editRender_.r.bottom};
    text(dc, total, status, kTextDim, DT_RIGHT | DT_SINGLELINE | DT_END_ELLIPSIS,
         fonts_.tiny);

    // Two hairlines between the groups. Nine buttons in a row read as one
    // undifferentiated wall otherwise, and which of them belong together is
    // the first thing somebody looking for one of them needs.
    auto divider = [&](int x, int top) {
        RECT line{x, top + 3, x + 1, top + 23};
        fill(dc, line, kLine);
    };
    divider((editResetCuts_.r.right + editMarkIn_.r.left) / 2, editSplit_.r.top);
    divider(rightEdge - 8, editPreviews_.r.top);

    // One line, and about whatever is in hand rather than about everything
    // at once: the paragraph that used to sit here said four things and was
    // read as none of them.
    std::wstring hintText;
    if (timeline_.selectedText() >= 0) {
        hintText = tr(L"Caption picked  \x00b7  double click it to write it, drag its "
                      L"ends to move it, Clip properties for the rest");
    } else if (timeline_.selected() >= 0 && timeline_.selectedRow() > 0) {
        wchar_t line[160];
        _snwprintf_s(line, _TRUNCATE,
                     tr(L"Piece %d on A%d picked  \x00b7  Drop piece silences this "
                        L"track only  \x00b7  double click it for its own volume, EQ "
                        L"and pitch"),
                     timeline_.selected() + 1,
                     timeline_.trackForRow(timeline_.selectedRow()) + 1);
        hintText = line;
    } else if (timeline_.selected() >= 0) {
        wchar_t line[160];
        _snwprintf_s(line, _TRUNCATE,
                     tr(L"Piece %d picked, every row  \x00b7  Alt+click one row to work "
                        L"on it alone  \x00b7  double click for crop, zoom and visibility"),
                     timeline_.selected() + 1);
        hintText = line;
    } else {
        hintText = tr(L"S splits every row  \x00b7  Alt+click picks one row  \x00b7  double "
                      L"click a piece for its settings  \x00b7  the knobs at a row\x2019s "
                      L"edges are its fades");
    }
    RECT hint{14, barTop + 62, editRender_.r.right, barTop + 84};
    text(dc, hint, hintText, kTextFaint, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS,
         fonts_.tiny);
}

void Player::paintTransport(HDC dc, const RECT& r) {
    const double duration = std::max(audio_.duration(), 0.001);
    slider(dc, seek_.r, audio_.position() / duration, true, false);

    // The dropped stretches, marked on the seek bar as well as in the editor:
    // the editor can be closed and the cuts still apply.
    const int mid = (seek_.r.top + seek_.r.bottom) / 2;
    for (const vp::Segment& s : timeline_.segments()) {
        if (s.kept) continue;
        const int left = seek_.r.left + static_cast<int>((seek_.r.right - seek_.r.left) *
                                                         s.start / duration);
        const int right = seek_.r.left + static_cast<int>((seek_.r.right - seek_.r.left) *
                                                          s.end / duration);
        if (right <= left) continue;
        RECT gone{left, mid - 2, right, mid + 2};
        roundFill(dc, gone, RGB(0x6a, 0x2c, 0x2c), 4);
    }
    auto mark = [&](double at, COLORREF colour) {
        if (at < 0.0) return;
        const int x = seek_.r.left +
                      static_cast<int>((seek_.r.right - seek_.r.left) * at / duration);
        RECT flag{x - 1, seek_.r.top - 5, x + 2, seek_.r.bottom + 5};
        fill(dc, flag, colour);
    };
    mark(timeline_.markIn(), kAccentLit);
    mark(timeline_.markOut(), kAccentLit);

    // A flat icon button: no frame at rest, a soft one under the mouse, the way
    // the tool buttons in the version this replaces behaved.
    auto button = [&](const Hit& h, const wchar_t* glyph, HFONT font,
                      bool active = false) {
        if (active) roundBox(dc, h.r, kAccentDim, kAccent, 6);
        else if (h.has(mouse_)) roundBox(dc, h.r, kSection, kLineHot, 6);
        text(dc, h.r, glyph, active ? RGB(255, 255, 255) : (h.has(mouse_) ? kText : kTextDim),
             DT_CENTER | DT_SINGLELINE | DT_VCENTER, font);
    };
    // Play, pause and full screen come from the icon font, which has dependable
    // codes for those. Skipping does not - the code that is a rewind arrow in
    // one build of the font is a swap arrow in another - so those say what they
    // do instead.
    button(play_, audio_.playing() ? L"\xE769" : L"\xE768", fonts_.icon);
    button(back_, L"\x00ab", fonts_.ui);
    button(stepBack_, L"\x25c2\x2758", fonts_.tiny);
    button(stepForward_, L"\x2758\x25b8", fonts_.tiny);
    button(forward_, L"\x00bb", fonts_.ui);
    button(full_, isFull_ ? L"\xE73F" : L"\xE740", fonts_.icon);

    text(dc, {forward_.r.right + 10, play_.r.top, forward_.r.right + 126, play_.r.bottom},
         formatTime(audio_.position()) + L" / " + formatTime(audio_.duration()),
         kTextDim, DT_LEFT | DT_SINGLELINE | DT_VCENTER, fonts_.mono);

    // A chip that only shows a value - the speed, the fit, the zoom - and
    // cycles to the next one when it is pressed. There is nothing to be on or
    // off about, so it is never lit: the label already says where it stands.
    auto chip = [&](const Hit& h, const std::wstring& s) {
        roundBox(dc, h.r, h.has(mouse_) ? kRowHot : kSection,
                 h.has(mouse_) ? kLineHot : kLine, 6);
        text(dc, h.r, s, kText, DT_CENTER | DT_SINGLELINE | DT_VCENTER, fonts_.ui);
    };
    // A chip that is genuinely a switch. Those get a tick, because a colour on
    // its own never says whether it means on or simply "this one".
    auto toggle = [&](const Hit& h, const std::wstring& s, bool on) {
        roundBox(dc, h.r, on ? kAccentDim : (h.has(mouse_) ? kRowHot : kSection),
                 on ? kAccent : (h.has(mouse_) ? kLineHot : kLine), 6);
        // The empty box is drawn too. Showing it only once the switch was on
        // meant the word moved sideways when it was pressed, and a switch that
        // is off looked no different from a chip that is not a switch at all.
        const int mid = (h.r.top + h.r.bottom) / 2;
        RECT box{h.r.left + 8, mid - 7, h.r.left + 22, mid + 7};
        checkBox(dc, box, on, h.has(mouse_));
        text(dc, {box.right + 4, h.r.top, h.r.right - 6, h.r.bottom}, s,
             on ? RGB(255, 255, 255) : kText,
             DT_CENTER | DT_SINGLELINE | DT_VCENTER, fonts_.ui);
    };
    toggle(editorChip_, tr(L"Editor"), editorOpen_);
    button(statsChip_, L"i", fonts_.semi, statsOn_);

    // The cog on its own was guessed at rather than read, so it says what it
    // is: an icon and the word, the way every other button here is read.
    roundBox(dc, gear_.r, gear_.has(mouse_) ? kRowHot : kSection,
             gear_.has(mouse_) ? kLineHot : kLine, 6);
    text(dc, {gear_.r.left + 8, gear_.r.top, gear_.r.left + 28, gear_.r.bottom},
         L"\xE713", gear_.has(mouse_) ? kText : kTextDim,
         DT_CENTER | DT_SINGLELINE | DT_VCENTER, fonts_.icon);
    text(dc, {gear_.r.left + 30, gear_.r.top, gear_.r.right - 6, gear_.r.bottom},
         tr(L"Settings"), gear_.has(mouse_) ? kText : kTextDim,
         DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS, fonts_.ui);

    // Volume runs to 150 %, as it did before: a quiet recording played through
    // a laptop needs headroom above the file's own level, and a sum that clips
    // shows on the meters rather than being hidden by a ceiling.
    button(muteButton_, volume_level_ <= 0.0f ? L"\xE74F" : L"\xE767", fonts_.icon);
    slider(dc, volume_.r, volume_level_ / 1.5f, volume_level_ > 0.0f, false);
    wchar_t pct[16];
    _snwprintf_s(pct, _TRUNCATE, L"%d%%", static_cast<int>(volume_level_ * 100.0f + 0.5f));
    text(dc, {volume_.r.right + 6, muteButton_.r.top, volume_.r.right + 44,
              muteButton_.r.bottom},
         pct, kTextDim, DT_LEFT | DT_SINGLELINE | DT_VCENTER, fonts_.mono);

    // ---- how it is played, then the shape of the picture
    wchar_t speed[24];
    _snwprintf_s(speed, _TRUNCATE, tr(L"Speed %.3g\x00d7"), kSpeeds[speedIndex_]);
    chip(speedChip_, speed);
    toggle(pitchChip_, tr(L"Pitch"), keepPitch_);

    // The two rotations are actions rather than states, so neither is lit;
    // the orientation and the fit say where they stand in words already.
    chip(rotateLeft_, L"\x21ba");
    chip(rotateRight_, L"\x21bb");
    toggle(flipHChip_, tr(L"Mirror"), flipH_);
    toggle(flipVChip_, tr(L"Upside down"), flipV_);
    chip(orientChip_, portrait_ ? tr(L"Portrait") : tr(L"Landscape"));
    chip(fitChip_, tr(kFitNames[fitIndex_]));
    wchar_t zoom[32];
    _snwprintf_s(zoom, _TRUNCATE, tr(L"Zoom %d%%"), static_cast<int>(zoom_ * 100.0 + 0.5));
    chip(zoomReset_, zoom);
    toggle(loopChip_, tr(L"Loop"), looping_);
}

void Player::paintOsd(HDC dc, const RECT& video) {
    if (osd_.empty() || GetTickCount() > osdUntil_) return;
    // One message, in one place. While there is a picture, mpv draws it over
    // the picture - it repaints every frame, so nothing drawn from here would
    // survive there anyway - and this one would only be the same words a second
    // time, a little to the left. With no file open there is no mpv to draw it
    // and this is the only one.
    if (video_.ready() && !path_.empty()) return;
    RECT box{video.left + 24, video.top + 20, video.left + 424, video.top + 62};
    roundFill(dc, box, RGB(0x0a, 0x0b, 0x0e), 8);
    text(dc, box, osd_, RGB(255, 255, 255), DT_CENTER | DT_SINGLELINE | DT_VCENTER,
         fonts_.osd);
}

void Player::say(const std::wstring& s) {
    osd_ = s;
    osdUntil_ = GetTickCount() + 1300;
    // Over the picture the message has to come from mpv. The picture is a child
    // window that mpv repaints on every frame, so anything drawn there from
    // here survives about sixteen milliseconds. The drawn one is the fallback
    // for when no file is open and mpv has nothing to draw on.
    if (video_.ready() && !path_.empty()) video_.showText(vp::toUtf8(s), 1300);
    InvalidateRect(window_, nullptr, FALSE);
}

// -------------------------------------------------------------- interaction

double Player::fraction(const Hit& h, POINT p) const {
    // The knob has a width of its own, so the last few pixels at either end
    // were unreachable and the far left of a slider stopped at -99 instead of
    // -100. The travel is measured between the knob's own two end positions.
    const double edge = 7.0;
    const double span = static_cast<double>(h.r.right - h.r.left) - edge * 2.0;
    if (span <= 0.0) return 0.0;
    return std::max(0.0, std::min(1.0, (p.x - h.r.left - edge) / span));
}

void Player::onDown(POINT p) {
    mouse_ = p;

    const RECT picture = videoArea();
    if (PtInRect(&picture, p)) {
        // Whether this is a click or a drag is not known yet: a drag pans the
        // picture, a click plays or pauses, and deciding on the way down was
        // what made every attempt to move the picture also stop it.
        pressedVideo_ = true;
        movedWhilePressed_ = false;
        pressedAt_ = p;
        panFrom_ = p;
        return;
    }

    if (open_.has(p)) { choose(); return; }
    if (gear_.has(p)) { openSettings(); return; }
    if (play_.has(p)) { togglePlay(); return; }
    if (back_.has(p)) { seekBy(-vp::settings().seekSmall); return; }
    if (forward_.has(p)) { seekBy(vp::settings().seekSmall); return; }
    if (stepBack_.has(p)) { frameStep(true); return; }
    if (stepForward_.has(p)) { frameStep(false); return; }
    if (full_.has(p)) { toggleFullscreen(); return; }
    if (editorChip_.has(p)) { toggleEditor(); return; }
    if (statsChip_.has(p)) {
        statsOn_ = !statsOn_;
        statsRefreshed_ = 0;
        if (!statsOn_) video_.showText("", 1);
        return;
    }
    if (muteButton_.has(p)) {
        if (volume_level_ > 0.0f) { volumeBeforeMute_ = volume_level_; setVolume(0.0f); }
        else setVolume(volumeBeforeMute_);
        return;
    }
    if (rotateLeft_.has(p)) { bumpRotate(-90); return; }
    if (rotateRight_.has(p)) { bumpRotate(90); return; }
    if (flipHChip_.has(p)) { toggleFlip(true); return; }
    if (flipVChip_.has(p)) { toggleFlip(false); return; }
    if (orientChip_.has(p)) { cycleOrientation(); return; }
    if (zoomReset_.has(p)) { resetView(); return; }
    if (pitchChip_.has(p)) { toggleKeepPitch(); return; }
    if (loopChip_.has(p)) {
        looping_ = !looping_;
        say(looping_ ? tr(L"Loop on") : tr(L"Loop off"));
        return;
    }
    if (speedChip_.has(p)) { pickSpeed({speedChip_.r.left, speedChip_.r.bottom}); return; }
    if (fitChip_.has(p)) { cycleFit(); return; }
    if (resetPicture_.has(p)) { resetAdjust(); return; }
    if (normChip_.has(p)) {
        const bool on = !audio_.normalising();
        audio_.setNormalise(on);
        // The master switch means every track, so it says so: turning it on
        // turns each track's own switch on, and turning it off stops the lot.
        if (on) {
            for (size_t i = 0; i < audio_.tracks().size(); ++i) {
                audio_.controls(i).normalise.store(true);
            }
        }
        vp::settings().normalise = audio_.normalising();
        say(audio_.normalising() ? tr(L"Normalization on")
                                 : tr(L"Normalization off"));
        return;
    }
    if (targetChip_.has(p)) { cycleTarget(); return; }

    if (audioPresetChip_.has(p)) {
        const int count = static_cast<int>(vp::settings().audioPresets.size());
        if (count > 0) applyAudioPreset((audioPresetIndex_ + 2) % (count + 1) - 1);
        return;
    }
    if (audioPresetSave_.has(p)) { saveAudioPreset(); return; }
    if (audioPresetDelete_.has(p)) { deleteAudioPreset(); return; }
    if (videoPresetChip_.has(p)) {
        const int count = static_cast<int>(vp::settings().videoPresets.size());
        if (count > 0) applyVideoPreset((videoPresetIndex_ + 2) % (count + 1) - 1);
        return;
    }
    if (videoPresetSave_.has(p)) { saveVideoPreset(); return; }
    if (videoPresetDelete_.has(p)) { deleteVideoPreset(); return; }

    if (editorOpen_) {
        if (editSplit_.has(p)) {
            if (timeline_.splitAt(audio_.position())) {
                say(tr(L"Split at ") + formatTime(audio_.position(), true));
            }
            InvalidateRect(window_, nullptr, TRUE);
            return;
        }
        if (editDelete_.has(p)) {
            timeline_.deleteSelected();
            InvalidateRect(window_, nullptr, TRUE);
            return;
        }
        if (editRestore_.has(p)) {
            timeline_.restoreSelected();
            InvalidateRect(window_, nullptr, TRUE);
            return;
        }
        if (editResetCuts_.has(p)) {
            timeline_.resetCuts();
            say(tr(L"Cuts cleared"));
            InvalidateRect(window_, nullptr, TRUE);
            return;
        }
        if (editMarkIn_.has(p)) {
            timeline_.setMarkIn(audio_.position());
            say(tr(L"Start ") + formatTime(audio_.position(), true));
            InvalidateRect(window_, nullptr, TRUE);
            return;
        }
        if (editMarkOut_.has(p)) {
            timeline_.setMarkOut(audio_.position());
            say(tr(L"End ") + formatTime(audio_.position(), true));
            InvalidateRect(window_, nullptr, TRUE);
            return;
        }
        if (editClearMarks_.has(p)) {
            timeline_.clearMarks();
            InvalidateRect(window_, nullptr, TRUE);
            return;
        }
        if (editAddText_.has(p)) { addCaption(); return; }
        if (editClipProps_.has(p)) { openGlobalProperties(); return; }
        if (localReady() && editLocalProps_.has(p)) {
            openLocalProperties();
            return;
        }
        if (editPreviews_.has(p)) { buildPreviews(true); return; }
        if (editSaveProject_.has(p)) { saveProject(); return; }
        if (editOpenProject_.has(p)) { openProject(); return; }
        if (editRender_.has(p)) { openRender(); return; }

        bool seeked = false;
        double where = 0.0;
        if (timeline_.onDown(p, (GetKeyState(VK_MENU) & 0x8000) != 0, &seeked, &where)) {
            drag_ = Drag::Timeline;
            if (seeked) seekTo(where);
            InvalidateRect(window_, nullptr, TRUE);
            return;
        }
    }

    if (seek_.has(p)) {
        drag_ = Drag::Seek;
        seekTo(fraction(seek_, p) * audio_.duration());
        return;
    }
    if (volume_.has(p)) {
        drag_ = Drag::Volume;
        setVolume(static_cast<float>(fraction(volume_, p) * 1.5));
        return;
    }

    // Only inside the list's own box: a row scrolled out of sight must not still
    // answer the mouse from where it would have been.
    for (size_t row = 0; PtInRect(&tracksView_, p) && row < trackEnable_.size() &&
                         row < audio_.tracks().size();
         ++row) {
        const size_t i = panelTrack(row);
        auto& c = audio_.controls(i);
        if (trackEnable_[row].has(p)) {
            const bool on = !c.enabled.load();
            c.enabled.store(on);
            say(on ? tr(L"Track on") : tr(L"Track off"));
            return;
        }
        if (trackSolo_[row].has(p)) {
            c.solo.store(!c.solo.load());
            say(c.solo.load() ? L"Solo" : tr(L"Solo off"));
            return;
        }
        if (trackExpand_[row].has(p)) {
            expanded_[row] = !expanded_[row];
            layout();
            InvalidateRect(window_, nullptr, TRUE);
            return;
        }
        if (trackNorm_[row].has(p)) { toggleTrackNorm(i); return; }
        if (trackVolume_[row].has(p)) {
            drag_ = Drag::TrackVolume;
            dragIndex_ = static_cast<int>(row);
            c.gain.store(static_cast<float>(fraction(trackVolume_[row], p) * 2.0));
            return;
        }
        if (!expanded_[row]) continue;
        if (eqReset_[row].has(p)) { resetEq(i); return; }
        auto band = [&](const Hit& h, std::atomic<float>& value, Drag which) {
            if (!h.has(p)) return false;
            drag_ = which;
            dragIndex_ = static_cast<int>(row);
            value.store(static_cast<float>(fraction(h, p) * 24.0 - 12.0));
            return true;
        };
        if (band(eqLow_[row], c.eqLow, Drag::EqLow)) return;
        if (band(eqMid_[row], c.eqMid, Drag::EqMid)) return;
        if (band(eqHigh_[row], c.eqHigh, Drag::EqHigh)) return;
        if (eqPitch_[row].has(p)) {
            drag_ = Drag::Pitch;
            dragIndex_ = static_cast<int>(row);
            // Whole semitones: a quarter of one is not a musical interval, and
            // a slider that lands between notes is a slider nobody can aim.
            c.pitch.store(static_cast<float>(
                std::floor(fraction(eqPitch_[row], p) * 24.0 + 0.5) - 12.0));
            return;
        }
    }

    for (size_t i = 0; i < adjust_.size(); ++i) {
        if (adjust_[i].slider.has(p)) {
            drag_ = Drag::Adjust;
            dragIndex_ = static_cast<int>(i);
            const double f = fraction(adjust_[i].slider, p);
            adjust_[i].value =
                adjust_[i].lo + static_cast<int>(f * (adjust_[i].hi - adjust_[i].lo) + 0.5);
            applyAdjust(i);
            return;
        }
    }
}

void Player::onMove(POINT p) {
    // A press on the picture becomes a pan once the mouse has actually moved,
    // and only while there is something to move: at 100 % the picture fills
    // nothing it could be slid inside.
    if (pressedVideo_ && !panning_) {
        if (std::abs(p.x - pressedAt_.x) + std::abs(p.y - pressedAt_.y) > 3) {
            movedWhilePressed_ = true;
            // The canvas itself is what moves, so this is worth doing at any
            // zoom - it is how the frame is brought under the eye, not how the
            // film is framed.
            panning_ = true;
            panFrom_ = p;
        }
    }

    if (panning_) {
        // Measured against the room, not against the picture. The position is
        // kept as a fraction of the room, so dividing the mouse movement by the
        // width of the picture was the bug behind "it will not move when it is
        // zoomed in": at twice its size the same drag moved it half as far, at
        // four times a quarter. Against the room it is one to one with the hand
        // at every zoom.
        const double wide = std::max(1L, videoStage_.right - videoStage_.left);
        const double tall = std::max(1L, videoStage_.bottom - videoStage_.top);
        panX_ = std::max(-4.0, std::min(4.0, panX_ + (p.x - panFrom_.x) / wide));
        panY_ = std::max(-4.0, std::min(4.0, panY_ + (p.y - panFrom_.y) / tall));
        panFrom_ = p;
        placeCanvas();
        InvalidateRect(window_, &videoStage_, FALSE);
        mouse_ = p;
        return;
    }

    mouse_ = p;
    const size_t i = static_cast<size_t>(std::max(0, dragIndex_));
    switch (drag_) {
        case Drag::Seek: seekTo(fraction(seek_, p) * audio_.duration()); break;
        case Drag::Volume:
            setVolume(static_cast<float>(fraction(volume_, p) * 1.5));
            break;
        case Drag::Timeline: {
            bool seeked = false;
            double where = 0.0;
            if (timeline_.onDrag(p, &seeked, &where) && seeked) seekTo(where);
            break;
        }
        case Drag::TrackVolume:
            if (dragIndex_ >= 0) {
                audio_.controls(panelTrack(i)).gain.store(
                    static_cast<float>(fraction(trackVolume_[i], p) * 2.0));
            }
            break;
        case Drag::EqLow:
            if (dragIndex_ >= 0) {
                audio_.controls(panelTrack(i)).eqLow.store(
                    static_cast<float>(fraction(eqLow_[i], p) * 24.0 - 12.0));
            }
            break;
        case Drag::EqMid:
            if (dragIndex_ >= 0) {
                audio_.controls(panelTrack(i)).eqMid.store(
                    static_cast<float>(fraction(eqMid_[i], p) * 24.0 - 12.0));
            }
            break;
        case Drag::EqHigh:
            if (dragIndex_ >= 0) {
                audio_.controls(panelTrack(i)).eqHigh.store(
                    static_cast<float>(fraction(eqHigh_[i], p) * 24.0 - 12.0));
            }
            break;
        case Drag::Pitch:
            if (dragIndex_ >= 0) {
                audio_.controls(panelTrack(i)).pitch.store(static_cast<float>(
                    std::floor(fraction(eqPitch_[i], p) * 24.0 + 0.5) - 12.0));
            }
            break;
        case Drag::Adjust:
            if (dragIndex_ >= 0) {
                Adjust& a = adjust_[i];
                const double f = fraction(a.slider, p);
                a.value = a.lo + static_cast<int>(f * (a.hi - a.lo) + 0.5);
                applyAdjust(i);
            }
            break;
        default: break;
    }
    InvalidateRect(window_, nullptr, FALSE);
}

void Player::onUp() {
    // A caption that was dragged has new times, and mpv is holding the old
    // ones until it is handed the file again.
    if (drag_ == Drag::Timeline && !timeline_.texts().empty()) {
        captionsDueAt_ = GetTickCount() + 120;
    }

    // A clean click on the picture plays or pauses; one that turned into a drag
    // has already done its job by moving the picture.
    if (pressedVideo_ && !movedWhilePressed_) togglePlay();
    pressedVideo_ = false;
    movedWhilePressed_ = false;
    panning_ = false;
    if (drag_ == Drag::Timeline) timeline_.onUp();
    drag_ = Drag::None;
    dragIndex_ = -1;
}

// Where the wheel points decides what it does, which is what it did before:
// over the picture it zooms, over the volume it is the volume, over the list of
// tracks it scrolls that list. Putting the volume on the whole window is what
// made zooming and panning impossible.
void Player::onWheel(int delta, POINT p) {
    const RECT picture = videoArea();
    if (!isFull_ && PtInRect(&tracksView_, p) &&
        tracksContent_ > tracksView_.bottom - tracksView_.top) {
        const int span = tracksView_.bottom - tracksView_.top;
        trackScroll_ = std::max(0, std::min(tracksContent_ - span,
                                            trackScroll_ - (delta > 0 ? 60 : -60)));
        layout();
        InvalidateRect(window_, nullptr, FALSE);
        return;
    }
    if (volume_.has(p) || muteButton_.has(p)) {
        setVolume(volume_level_ + (delta > 0 ? 1.0f : -1.0f) *
                                      vp::settings().volumeStep / 100.0f);
        return;
    }
    if (seek_.has(p)) {
        seekBy(delta > 0 ? vp::settings().seekSmall : -vp::settings().seekSmall);
        return;
    }
    if (PtInRect(&picture, p)) {
        bumpZoom(delta > 0 ? 1.1 : 1.0 / 1.1);
        return;
    }
    setVolume(volume_level_ +
              (delta > 0 ? 1.0f : -1.0f) * vp::settings().volumeStep / 100.0f);
}

void Player::togglePlay() {
    const bool was = audio_.playing();
    if (was) { audio_.pause(); video_.setPaused(true); }
    else     { audio_.play();  video_.setPaused(false); }
    say(was ? tr(L"Paused") : tr(L"Playing"));
}

void Player::seekBy(double d) { seekTo(audio_.position() + d); }

void Player::seekTo(double s) {
    s = std::max(0.0, std::min(audio_.duration(), s));
    audio_.seek(s);
    video_.seek(s);
    InvalidateRect(window_, nullptr, FALSE);
}

void Player::setVolume(float v) {
    volume_level_ = std::max(0.0f, std::min(1.5f, v));
    audio_.setMasterGain(volume_level_);
    vp::settings().volume = volume_level_;
    wchar_t t[32];
    _snwprintf_s(t, _TRUNCATE, tr(L"Volume %d %%"),
                 static_cast<int>(volume_level_ * 100.0f + 0.5f));
    say(t);
}

// The chip used to step to the next speed and no further, so reaching 1.25 x
// from 2 x meant going all the way round. It opens the list instead, and the
// wheel over it still steps one at a time.
void Player::pickSpeed(POINT at) {
    HMENU menu = CreatePopupMenu();
    if (!menu) { cycleSpeed(1); return; }
    const int count = static_cast<int>(_countof(kSpeeds));
    for (int i = 0; i < count; ++i) {
        wchar_t label[32];
        _snwprintf_s(label, _TRUNCATE, L"%.3g\x00d7", kSpeeds[i]);
        AppendMenuW(menu, MF_STRING | (i == speedIndex_ ? MF_CHECKED : 0),
                    static_cast<UINT_PTR>(i + 1), label);
    }
    POINT screen = at;
    ClientToScreen(window_, &screen);
    const int chosen = static_cast<int>(TrackPopupMenu(
        menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD | TPM_NONOTIFY,
        screen.x, screen.y, 0, window_, nullptr));
    DestroyMenu(menu);
    if (chosen <= 0 || chosen > count) return;
    setSpeedIndex(chosen - 1);
}

void Player::setSpeedIndex(int index) {
    const int count = static_cast<int>(_countof(kSpeeds));
    speedIndex_ = std::max(0, std::min(count - 1, index));
    applyClipSettings(true);
    wchar_t osd[40];
    _snwprintf_s(osd, _TRUNCATE, tr(L"Speed %.3g\x00d7"), kSpeeds[speedIndex_]);
    say(osd);
    InvalidateRect(window_, nullptr, FALSE);
}

void Player::cycleSpeed(int direction) {
    const int count = static_cast<int>(_countof(kSpeeds));
    speedIndex_ = (speedIndex_ + direction + count) % count;

    // Both halves have to be told. The sound is stretched or resampled by the
    // engine; the picture is simply played faster, on top of whatever small
    // correction it is making to stay level with the sound.
    applyClipSettings(true);
    wchar_t osd[32];
    _snwprintf_s(osd, _TRUNCATE, tr(L"Speed %.3g\x00d7"), kSpeeds[speedIndex_]);
    say(osd);
}

void Player::cycleFit() {
    fitIndex_ = (fitIndex_ + 1) % 3;
    video_.setProperty("panscan", fitIndex_ == 1 ? "1.0" : "0.0");
    video_.setProperty("video-unscaled", fitIndex_ == 2 ? "yes" : "no");
    vp::settings().fitMode = fitIndex_;
    say(tr(kFitNames[fitIndex_]));
}

void Player::cycleTarget() {
    const int count = static_cast<int>(_countof(kTargets));
    targetIndex_ = (targetIndex_ + 1) % count;
    audio_.setNormaliseTarget(kTargets[targetIndex_]);
    vp::settings().normaliseTarget = kTargets[targetIndex_];
    wchar_t t[32];
    _snwprintf_s(t, _TRUNCATE, tr(L"Target %g LUFS"), kTargets[targetIndex_]);
    say(t);
}

// ------------------------------------------------------------------ picture

// What the piece under the playhead adds to one picture control, or zero when
// it has nothing to say about it. Kept in one place so the live picture, the
// shader and the filter chain all read the same number.
int Player::clipAdjust(size_t which) const {
    if (liveSegment_ < 0 ||
        liveSegment_ >= static_cast<int>(timeline_.segments().size())) {
        return 0;
    }
    const vp::ClipVideo& v =
        timeline_.segments()[static_cast<size_t>(liveSegment_)].video;
    if (!v.set) return 0;
    switch (which) {
        case 0: return v.contrast;
        case 1: return v.brightness;
        case 2: return v.saturation;
        case 3: return v.gamma;
        case 4: return v.hue;
        case 5: return v.sharpen;
        case 6: return v.denoise;
        default: return 0;
    }
}

void Player::applyAdjust(size_t i) {
    const Adjust& a = adjust_[i];
    switch (a.kind) {
        case AdjustKind::Property: {
            const int total = std::max(a.lo, std::min(a.hi,
                                        a.value + clipAdjust(i)));
            char value[16];
            _snprintf_s(value, sizeof(value), _TRUNCATE, "%d", total);
            video_.setProperty(a.property, value);
            break;
        }
        case AdjustKind::Sharpen: sharpenDueAt_ = GetTickCount() + 70; break;
        case AdjustKind::Denoise: applyDenoise(); break;
        case AdjustKind::Zoom:
            frameZoom_ = std::max(0.25, std::min(4.0, a.value / 100.0));
            applyGeometry();
            break;
        case AdjustKind::PanX:
            framePanX_ = std::max(-1.0, std::min(1.0, a.value / 100.0));
            if (portrait_) pictureDueAt_ = GetTickCount() + 60;
            applyPan();
            break;
        case AdjustKind::PanY:
            framePanY_ = std::max(-1.0, std::min(1.0, a.value / 100.0));
            applyPan();
            break;
    }

    vp::Settings& s = vp::settings();
    s.contrast = adjust_[0].value;
    s.brightness = adjust_[1].value;
    s.saturation = adjust_[2].value;
    s.gamma = adjust_[3].value;
    s.hue = adjust_[4].value;
    s.sharpen = adjust_[5].value;
    s.denoise = adjust_[6].value;
    InvalidateRect(window_, nullptr, FALSE);
}

void Player::applySharpen() {
    const double amount =
        std::max(0, std::min(200, adjust_[5].value + clipAdjust(5))) / 100.0;
    if (amount < 0.005) {
        video_.setProperty("glsl-shaders", "");
        return;
    }

    // Written to alternating file names. mpv keys its shader cache on the path,
    // so rewriting the same file leaves the old shader running; two slots make
    // every change take effect.
    shaderSlot_ = 1 - shaderSlot_;
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir.resize(slash + 1);

    wchar_t name[MAX_PATH];
    _snwprintf_s(name, _TRUNCATE, L"%s_sharpen%d.glsl", dir.c_str(), shaderSlot_);
    wchar_t body[4096];
    _snwprintf_s(body, _TRUNCATE, kSharpenShader, amount);

    std::ofstream out(name, std::ios::binary | std::ios::trunc);
    if (!out) return;
    out << vp::toUtf8(body);
    out.close();

    video_.setProperty("glsl-shaders", vp::toUtf8(name));
}

void Player::applyDenoise() { applyVideoFilters(); }

void Player::resetAdjust() {
    for (size_t i = 0; i < adjust_.size(); ++i) {
        adjust_[i].value = adjust_[i].kind == AdjustKind::Zoom ? 100 : 0;
        applyAdjust(i);
    }
    videoPresetIndex_ = -1;
    say(tr(L"Picture reset"));
}

void Player::resetEq(size_t track) {
    auto& c = audio_.controls(track);
    c.eqLow.store(0.0f);
    c.eqMid.store(0.0f);
    c.eqHigh.store(0.0f);
    c.pitch.store(0.0f);
    say(tr(L"Equaliser reset"));
}

// ------------------------------------------------------------------ presets

void Player::applyAudioPreset(int index) {
    audioPresetIndex_ = index;
    if (index < 0 || index >= static_cast<int>(vp::settings().audioPresets.size())) return;
    const vp::AudioPreset& p = vp::settings().audioPresets[static_cast<size_t>(index)];

    audio_.setNormalise(p.normalise);
    audio_.setNormaliseTarget(p.target);
    for (int i = 0; i < static_cast<int>(_countof(kTargets)); ++i) {
        if (std::fabs(kTargets[i] - p.target) < 0.01f) targetIndex_ = i;
    }
    for (size_t i = 0; i < audio_.tracks().size() && i < p.tracks.size(); ++i) {
        auto& c = audio_.controls(i);
        c.gain.store(p.tracks[i].gain);
        c.eqLow.store(p.tracks[i].eqLow);
        c.eqMid.store(p.tracks[i].eqMid);
        c.eqHigh.store(p.tracks[i].eqHigh);
        c.enabled.store(p.tracks[i].enabled);
        c.normalise.store(p.tracks[i].normalise);
        c.pitch.store(p.tracks[i].pitch);
    }
    say(tr(L"Preset ") + widen(p.name));
}

void Player::saveAudioPreset() {
    vp::AudioPreset p;
    p.normalise = audio_.normalising();
    p.target = audio_.normaliseTarget();
    for (size_t i = 0; i < audio_.tracks().size(); ++i) {
        auto& c = audio_.controls(i);
        vp::AudioPresetTrack t;
        t.gain = c.gain.load();
        t.eqLow = c.eqLow.load();
        t.eqMid = c.eqMid.load();
        t.eqHigh = c.eqHigh.load();
        t.enabled = c.enabled.load();
        t.normalise = c.normalise.load();
        t.pitch = c.pitch.load();
        p.tracks.push_back(t);
    }
    wchar_t name[64];
    SYSTEMTIME now{};
    GetLocalTime(&now);
    _snwprintf_s(name, _TRUNCATE, tr(L"%d tracks %02d:%02d"),
                 static_cast<int>(p.tracks.size()), now.wHour, now.wMinute);
    p.name = vp::toUtf8(name);

    vp::settings().audioPresets.push_back(p);
    audioPresetIndex_ = static_cast<int>(vp::settings().audioPresets.size()) - 1;
    vp::settings().save();
    say(tr(L"Preset saved"));
}

void Player::deleteAudioPreset() {
    auto& presets = vp::settings().audioPresets;
    if (audioPresetIndex_ < 0 || audioPresetIndex_ >= static_cast<int>(presets.size())) {
        return;
    }
    presets.erase(presets.begin() + audioPresetIndex_);
    audioPresetIndex_ = -1;
    vp::settings().save();
    say(tr(L"Preset deleted"));
}

void Player::applyVideoPreset(int index) {
    videoPresetIndex_ = index;
    if (index < 0 || index >= static_cast<int>(vp::settings().videoPresets.size())) return;
    const vp::VideoPreset& p = vp::settings().videoPresets[static_cast<size_t>(index)];
    const int values[] = {p.contrast, p.brightness, p.saturation, p.gamma,
                          p.hue,      p.sharpen,    p.denoise};
    for (size_t i = 0; i < adjust_.size() && i < _countof(values); ++i) {
        adjust_[i].value = values[i];
        applyAdjust(i);
    }
    say(tr(L"Preset ") + widen(p.name));
}

void Player::saveVideoPreset() {
    vp::VideoPreset p;
    p.contrast = adjust_[0].value;
    p.brightness = adjust_[1].value;
    p.saturation = adjust_[2].value;
    p.gamma = adjust_[3].value;
    p.hue = adjust_[4].value;
    p.sharpen = adjust_[5].value;
    p.denoise = adjust_[6].value;

    wchar_t name[64];
    SYSTEMTIME now{};
    GetLocalTime(&now);
    _snwprintf_s(name, _TRUNCATE, tr(L"Picture %02d:%02d"), now.wHour, now.wMinute);
    p.name = vp::toUtf8(name);

    vp::settings().videoPresets.push_back(p);
    videoPresetIndex_ = static_cast<int>(vp::settings().videoPresets.size()) - 1;
    vp::settings().save();
    say(tr(L"Preset saved"));
}

void Player::deleteVideoPreset() {
    auto& presets = vp::settings().videoPresets;
    if (videoPresetIndex_ < 0 || videoPresetIndex_ >= static_cast<int>(presets.size())) {
        return;
    }
    presets.erase(presets.begin() + videoPresetIndex_);
    videoPresetIndex_ = -1;
    vp::settings().save();
    say(tr(L"Preset deleted"));
}

// ------------------------------------------------------------------- editor

// ------------------------------------------------------ geometry and filters

// Flips, the portrait crop and denoising all end up in mpv's filter chain, so
// they are built together: setting the chain to one of them alone would drop
// the others.
void Player::applyVideoFilters() {
    std::string chain;
    auto add = [&](const std::string& part) {
        if (!chain.empty()) chain += ",";
        chain += part;
    };
    // The clip's own crop comes first: it picks the part of the frame
    // everything after it works on, which is what a pan zone means.
    if (liveSegment_ >= 0 &&
        liveSegment_ < static_cast<int>(timeline_.segments().size())) {
        const vp::ClipVideo& v =
            timeline_.segments()[static_cast<size_t>(liveSegment_)].video;
        double cx = 0.0, cy = 0.0, cw = 1.0, ch = 1.0;
        if (vp::clipWindow(v, &cx, &cy, &cw, &ch)) {
            char crop[200];
            _snprintf_s(crop, sizeof(crop), _TRUNCATE,
                        "lavfi=[crop=iw*%.4f:ih*%.4f:iw*%.4f:ih*%.4f]", cw, ch,
                        cx, cy);
            add(crop);
        }
    }

    // The picture's fade in and out. It reads the timestamp of the frame, so
    // it stays right through a seek, and it is the same ramp the export writes.
    {
        const vp::RowFades& f = timeline_.fades(0);
        const double lo = timeline_.outLow(), hi = timeline_.outHigh();
        char fade[160];
        if (f.in > 0.01) {
            _snprintf_s(fade, sizeof(fade), _TRUNCATE,
                        "lavfi=[fade=t=in:st=%.3f:d=%.3f]", lo, f.in);
            add(fade);
        }
        if (f.out > 0.01 && hi > lo) {
            _snprintf_s(fade, sizeof(fade), _TRUNCATE,
                        "lavfi=[fade=t=out:st=%.3f:d=%.3f]", hi - f.out, f.out);
            add(fade);
        }
    }

    if (portrait_) {
        // The tall canvas slides across the wide picture, so what it shows can
        // be chosen instead of always being the middle.
        const double t = std::max(0.0, std::min(1.0, 0.5 - framePanX_ * 0.5));
        char crop[160];
        _snprintf_s(crop, sizeof(crop), _TRUNCATE,
                    "lavfi=[crop=w=ih*9/16:h=ih:x=(iw-ih*9/16)*%.4f:y=0]", t);
        add(crop);
    }
    if (flipH_) add("hflip");
    if (flipV_) add("vflip");

    const int denoise = std::max(0, std::min(100, adjust_[6].value + clipAdjust(6)));
    if (denoise > 0) {
        const double d = denoise / 100.0;
        char filter[160];
        _snprintf_s(filter, sizeof(filter), _TRUNCATE,
                    "lavfi=[hqdn3d=%.2f:%.2f:%.2f:%.2f]", 2.0 * d, 1.5 * d, 4.0 * d,
                    4.0 * d);
        add(filter);
    }

    // Visibility, as a wash towards black. mpv has no opacity of its own, and
    // scaling the channels is what "darker" means anyway.
    if (liveSegment_ >= 0 &&
        liveSegment_ < static_cast<int>(timeline_.segments().size())) {
        const vp::ClipVideo& v =
            timeline_.segments()[static_cast<size_t>(liveSegment_)].video;
        if (v.set && v.opacity < 0.999f) {
            char wash[200];
            _snprintf_s(wash, sizeof(wash), _TRUNCATE,
                        "lavfi=[colorchannelmixer=rr=%.3f:gg=%.3f:bb=%.3f]",
                        v.opacity, v.opacity, v.opacity);
            add(wash);
        }
    }

    // Software filters cannot touch a frame that never leaves the graphics
    // card, and with plain hardware decoding mpv takes the chain without
    // applying any of it - which is why the crop, the wash and the fades were
    // set and did nothing. Frames are copied back only while there is a chain
    // to run; with none, decoding stays entirely on the card.
    if (!chain.empty() && !hwdecCopy_) {
        hwdecCopy_ = true;
        video_.setProperty("hwdec", "auto-copy");
    }
    video_.setProperty("vf", chain);
}

// Rotation, zoom and pan are properties rather than filters: mpv does them on
// the way to the screen, so they cost nothing and survive a filter change.
void Player::applyGeometry() {
    video_.setProperty("video-rotate", std::to_string(rotate_));

    char text[32];
    _snprintf_s(text, sizeof(text), _TRUNCATE, "%.4f",
                std::log2(std::max(0.05, frameZoom_)));
    video_.setPropertyFast("video-zoom", text);
    applyPan();
}

// What a drag sends, and nothing else. Rotation and zoom have not changed
// while the mouse is moving, and asking the core to take them again on every
// WM_MOUSEMOVE is what made panning feel like it was catching on something.
// The rows' fades, handed to the mixer and to the picture. Called from the
// tick rather than from every place a fade can move, because a fade moves under
// a drag and this is one comparison of a few doubles.
void Player::applyFades(bool force) {
    double signature = timeline_.outLow() * 7.0 + timeline_.outHigh() * 13.0;
    const int rows = timeline_.rowCount();
    for (int row = 0; row < rows; ++row) {
        const vp::RowFades& f = timeline_.fades(row);
        signature += (f.in * 3.0 + f.out * 5.0 +
                      (f.curve == vp::FadeCurve::Smooth ? 0.5 : 0.0)) * (row + 1);
    }
    if (!force && std::fabs(signature - fadeSignature_) < 1e-9) return;
    fadeSignature_ = signature;

    const double lo = timeline_.outLow(), hi = timeline_.outHigh();
    for (size_t i = 0; i < audio_.tracks().size(); ++i) {
        const int row = timeline_.rowForTrack(static_cast<int>(i));
        const vp::RowFades& f = row < rows ? timeline_.fades(row) : timeline_.fades(0);
        auto& c = audio_.controls(i);
        c.fadeInAt.store(static_cast<float>(lo));
        c.fadeInLen.store(static_cast<float>(f.in));
        c.fadeOutAt.store(static_cast<float>(std::max(lo, hi - f.out)));
        c.fadeOutLen.store(static_cast<float>(f.out));
        c.fadeSmooth.store(f.curve == vp::FadeCurve::Smooth);
    }
    applyVideoFilters();
}

void Player::applyPan() {
    // What the hand dragged, plus what the piece under the playhead asks for.
    // The piece's own shift is done here rather than with a crop because a
    // picture with nothing cropped away has no room to slide inside itself:
    // this moves the frame on the canvas and leaves black behind it, which is
    // what lets a tall video be moved across a wide window.
    char text[32];
    _snprintf_s(text, sizeof(text), _TRUNCATE, "%.4f",
                std::max(-2.0, std::min(2.0,
                    (portrait_ ? 0.0 : framePanX_) + liveClipPanX_)));
    video_.setPropertyFast("video-pan-x", text);
    _snprintf_s(text, sizeof(text), _TRUNCATE, "%.4f",
                std::max(-2.0, std::min(2.0, framePanY_ + liveClipPanY_)));
    video_.setPropertyFast("video-pan-y", text);
}

void Player::bumpRotate(int degrees) {
    rotate_ = ((rotate_ + degrees) % 360 + 360) % 360;
    applyGeometry();
    wchar_t t[32];
    _snwprintf_s(t, _TRUNCATE, tr(L"Rotation %d\x00b0"), rotate_);
    say(t);
    InvalidateRect(window_, nullptr, FALSE);
}

void Player::toggleFlip(bool horizontal) {
    if (horizontal) flipH_ = !flipH_; else flipV_ = !flipV_;
    applyVideoFilters();
    say(horizontal ? (flipH_ ? tr(L"Flipped across") : tr(L"Flip across off"))
                   : (flipV_ ? tr(L"Flipped over") : tr(L"Flip over off")));
    InvalidateRect(window_, nullptr, FALSE);
}

void Player::cycleOrientation() {
    portrait_ = !portrait_;
    applyVideoFilters();
    say(portrait_ ? tr(L"Portrait") : tr(L"Landscape"));
    InvalidateRect(window_, nullptr, FALSE);
}

void Player::bumpZoom(double factor) {
    // Full screen is for watching. There is no canvas drawn there to frame
    // against and no panel to read the figure off, and a picture left zoomed by
    // a stray wheel is the sort of thing that is noticed an hour later.
    if (isFull_) return;
    zoom_ = std::max(0.25, std::min(4.0, zoom_ * factor));
    placeCanvas();
    InvalidateRect(window_, &videoStage_, TRUE);
    wchar_t t[32];
    _snwprintf_s(t, _TRUNCATE, L"Zoom %d%%", static_cast<int>(zoom_ * 100.0 + 0.5));
    say(t);
    InvalidateRect(window_, nullptr, FALSE);
}

void Player::resetView() {
    rotate_ = 0;
    flipH_ = flipV_ = portrait_ = false;
    zoom_ = 1.0;
    panX_ = panY_ = 0.0;
    placeCanvas();
    applyGeometry();
    applyVideoFilters();
    say(tr(L"View reset"));
    InvalidateRect(window_, nullptr, TRUE);
}

void Player::frameStep(bool backwards) {
    // Stepping only makes sense while stopped, so it stops first rather than
    // stepping one frame and letting playback swallow it.
    if (audio_.playing()) { audio_.pause(); video_.setPaused(true); }
    video_.frameStep(backwards);
    // The sound follows the picture here, which is the one place it does: a
    // single frame is far too short to mix.
    audio_.seek(video_.videoPosition());
    InvalidateRect(window_, nullptr, FALSE);
}

void Player::toggleKeepPitch() {
    keepPitch_ = !keepPitch_;
    audio_.setKeepPitch(keepPitch_);
    vp::settings().keepPitch = keepPitch_;
    say(keepPitch_ ? tr(L"Pitch held") : tr(L"Speed like tape"));
    InvalidateRect(window_, nullptr, FALSE);
}

void Player::bumpAvOffset(int milliseconds) {
    // The engine already accounts for the latency of the sound card. What it
    // cannot know is how long the panel, the television or the receiver holds a
    // frame before showing it, and that is what this is for.
    avOffset_ = std::max(-2.0, std::min(2.0, avOffset_ + milliseconds / 1000.0));
    vp::settings().avOffset = avOffset_;
    wchar_t t[40];
    _snwprintf_s(t, _TRUNCATE, tr(L"Sound offset %+.0f ms"), avOffset_ * 1000.0);
    say(t);
}

void Player::toggleTrack(size_t index) {
    if (index >= audio_.tracks().size()) return;
    auto& c = audio_.controls(index);
    const bool on = !c.enabled.load();
    c.enabled.store(on);
    wchar_t t[64];
    _snwprintf_s(t, _TRUNCATE, tr(L"Track %d %s"), static_cast<int>(index) + 1,
                 on ? tr(L"on") : tr(L"off"));
    say(t);
    InvalidateRect(window_, nullptr, FALSE);
}

void Player::toggleTrackNorm(size_t index) {
    if (index >= audio_.tracks().size()) return;
    auto& c = audio_.controls(index);
    // The button answers what it shows, which is whether this track is being
    // normalised right now - its own switch AND the master one. A track whose
    // own switch was already on while the master was off used to swallow the
    // first press turning that switch off again, so nothing changed on screen
    // and it took two presses to get anywhere.
    const bool live = c.normalise.load() && audio_.normalising();
    const bool on = !live;
    c.normalise.store(on);
    if (on && !audio_.normalising()) {
        audio_.setNormalise(true);
        vp::settings().normalise = true;
    }
    wchar_t t[120];
    if (on) {
        _snwprintf_s(t, _TRUNCATE,
                     tr(L"Track %d \x2014 normalization on, listening for a second first"),
                     static_cast<int>(index) + 1);
    } else {
        _snwprintf_s(t, _TRUNCATE, tr(L"Track %d \x2014 normalization off"),
                     static_cast<int>(index) + 1);
    }
    say(t);
    InvalidateRect(window_, nullptr, FALSE);
}

void Player::refreshStats() {
    if (!statsOn_ || path_.empty()) return;
    const DWORD now = GetTickCount();
    if (now - statsRefreshed_ < 500) return;
    statsRefreshed_ = now;

    // Refreshed rather than left standing: mpv's own text expires on its own,
    // and a figure that has stopped moving is worse than no figure at all.
    // Built as lines rather than one format string, because half of these are
    // std::string and passing those through varargs is how that goes wrong.
    std::wstring text;
    wchar_t line[240];

    _snwprintf_s(line, _TRUNCATE, L"%d \x00d7 %d   %.4g fps   %s   hwdec: %s",
                 video_.width(), video_.height(), video_.fps(),
                 formatTime(audio_.duration()).c_str(),
                 video_.hwdec().empty() ? L"-" : widen(video_.hwdec()).c_str());
    text += line;

    {
        const std::string shader = video_.property("glsl-shaders");
        // Whether mpv took the shader, not which file it is: an option mpv
        // refuses keeps its old value silently, and this is the line that says
        // so without printing a path nobody can read at this size.
        _snwprintf_s(line, _TRUNCATE, L"sharpen: %.2f %s",
                     adjust_[5].value / 100.0,
                     shader.empty() ? L"(off)" : L"(shader running)");
        text += L"\n";
        text += line;
    }

    _snwprintf_s(line, _TRUNCATE, tr(L"sound: %s"),
                 widen(audio_.outputDeviceName()).c_str());
    text += L"\n";
    text += line;

    _snwprintf_s(line, _TRUNCATE, tr(L"tracks: %d   latency: %.0f ms   dropouts: %llu"),
                 static_cast<int>(audio_.tracks().size()),
                 audio_.outputLatency() * 1000.0,
                 static_cast<unsigned long long>(audio_.dropouts()));
    text += L"\n";
    text += line;

    _snwprintf_s(line, _TRUNCATE,
                 tr(L"speed: %.3g\x00d7 %s   sound offset: %+.0f ms"),
                 kSpeeds[speedIndex_], keepPitch_ ? tr(L"(pitch held)") : tr(L"(like tape)"),
                 avOffset_ * 1000.0);
    text += L"\n";
    text += line;

    // The one number that says whether the two halves are actually together.
    _snwprintf_s(line, _TRUNCATE, tr(L"sound vs picture: %+.0f ms"),
                 (audio_.position() - video_.videoPosition()) * 1000.0);
    text += L"\n";
    text += line;

    video_.showText(vp::toUtf8(text), 900);
}

void Player::toggleEditor() {
    // Deliberately not written to the settings: whether the editor is open
    // now and whether it should be open at start-up are two different
    // questions, and the second one has its own switch.
    editorOpen_ = !editorOpen_;
    if (editorOpen_) buildPreviews(false);
    layout();
    InvalidateRect(window_, nullptr, TRUE);
}

void Player::buildPreviews(bool force) {
    if (path_.empty() || vp::ffmpegPath().empty()) return;
    const vp::Settings& s = vp::settings();

    // Two costs, two switches. Waveforms read the whole sound track; thumbnails
    // are a few dozen fast seeks. Neither happens behind anyone's back when the
    // setting says to wait to be asked.
    const bool wantWaves = s.waveMode != "off" && (force || s.waveMode == "editor");
    const bool wantThumbs = s.thumbMode != "off" && (force || s.thumbMode == "editor");

    if (wantWaves && (force || !wavesWanted_)) {
        wavesWanted_ = true;
        timeline_.setWaveforms({});
        const int width = std::max(1, static_cast<int>(timeline_.bounds().right -
                                                      timeline_.bounds().left));
        std::vector<int> streams;
        for (const vp::TrackInfo& t : audio_.tracks()) streams.push_back(t.audioIndex);
        waveJob_.start(path_, streams, audio_.duration(), std::max(600, width * 2));
    }
    if (wantThumbs && (force || !thumbsWanted_)) {
        thumbsWanted_ = true;
        timeline_.setThumbs({});
        thumbJob_.start(path_, audio_.duration(), 40);
    }
}

// ------------------------------------------------------- clips and captions

// Everything the panel down the right-hand side holds, in one window, reading
// and writing the very same values - so a slider moved here is heard at once
// and the panel shows it too. The panel stays as the quick way to the few of
// them that are reached most often.
void Player::openGlobalProperties() {
    if (path_.empty()) return;

    vp::PlayerSheet sheet;
    sheet.subtitle = fileName_;
    sheet.fromPlayer = [this] { return fromPlayer_; };
    sheet.setFromPlayer = [this](bool on) {
        if (on == fromPlayer_) return;
        if (!on) seedOverride();
        fromPlayer_ = on;
    };
    sheet.band = [this](int which) { return sheetBand(which); };
    sheet.setBand = [this](int which, double v) { setSheetBand(which, v); };
    sheet.resetPicture = [this] { resetAdjust(); };
    sheet.view = [this](int which) {
        return which == 0 ? frameZoom_ : (which == 1 ? framePanX_ : framePanY_);
    };
    sheet.setView = [this](int which, double value) {
        if (which == 0) frameZoom_ = std::max(0.25, std::min(4.0, value));
        else if (which == 1) framePanX_ = std::max(-1.0, std::min(1.0, value));
        else framePanY_ = std::max(-1.0, std::min(1.0, value));
        for (Adjust& a : adjust_) {
            if (a.kind == AdjustKind::Zoom) a.value = static_cast<int>(frameZoom_ * 100.0 + 0.5);
            else if (a.kind == AdjustKind::PanX) a.value = static_cast<int>(framePanX_ * 100.0);
            else if (a.kind == AdjustKind::PanY) a.value = static_cast<int>(framePanY_ * 100.0);
        }
        applyGeometry();
        InvalidateRect(window_, nullptr, FALSE);
    };
    sheet.normaliseAll = [this] {
        return fromPlayer_ ? audio_.normalising() : overrideNormalise_;
    };
    sheet.setNormaliseAll = [this](bool on) {
        if (fromPlayer_) {
            audio_.setNormalise(on);
            vp::settings().normalise = on;
        } else {
            overrideNormalise_ = on;
        }
        InvalidateRect(window_, nullptr, FALSE);
    };
    sheet.target = [this] {
        return static_cast<double>(fromPlayer_ ? audio_.normaliseTarget()
                                               : overrideTarget_);
    };
    sheet.setTarget = [this](double db) {
        if (fromPlayer_) {
            audio_.setNormaliseTarget(static_cast<float>(db));
            vp::settings().normaliseTarget = static_cast<float>(db);
        } else {
            overrideTarget_ = static_cast<float>(db);
        }
        InvalidateRect(window_, nullptr, FALSE);
    };

    if (overrideTracks_.size() != audio_.tracks().size()) seedOverride();

    for (size_t i = 0; i < audio_.tracks().size(); ++i) {
        vp::SheetTrack t;
        t.name = widen(audio_.tracks()[i].name);
        wchar_t note[96];
        const auto& c = audio_.controls(i);
        const float lift = c.normalisationDb.load();
        if (std::fabs(lift) > 0.05f) {
            _snwprintf_s(note, _TRUNCATE, L"%.4g kHz \x00b7 %d ch. \x00b7 %+.1f dB",
                         audio_.tracks()[i].sourceRate / 1000.0,
                         audio_.tracks()[i].sourceChannels, lift);
        } else {
            _snwprintf_s(note, _TRUNCATE, L"%.4g kHz \x00b7 %d ch.",
                         audio_.tracks()[i].sourceRate / 1000.0,
                         audio_.tracks()[i].sourceChannels);
        }
        t.note = note;

        auto over = [this, i]() -> OverrideTrack& { return overrideTracks_[i]; };
        const bool* live = &fromPlayer_;

        t.playing = [this, i, live] {
            return *live ? audio_.controls(i).enabled.load()
                         : overrideTracks_[i].include;
        };
        t.setPlaying = [this, i, live](bool on) {
            if (*live) audio_.controls(i).enabled.store(on);
            else overrideTracks_[i].include = on;
            InvalidateRect(window_, nullptr, FALSE);
        };
        t.normalise = [this, i, live] {
            return *live ? (audio_.normalising() &&
                            audio_.controls(i).normalise.load())
                         : overrideTracks_[i].normalise;
        };
        t.setNormalise = [this, i, live](bool on) {
            if (*live) {
                audio_.controls(i).normalise.store(on);
                if (on && !audio_.normalising()) {
                    audio_.setNormalise(true);
                    vp::settings().normalise = true;
                }
            } else {
                overrideTracks_[i].normalise = on;
            }
            InvalidateRect(window_, nullptr, FALSE);
        };

        auto band = [this, i, live](std::atomic<float> vp::TrackControls::*field,
                                    float OverrideTrack::*spare) {
            return std::make_pair(
                [this, i, live, field, spare]() -> double {
                    return *live ? (audio_.controls(i).*field).load()
                                 : overrideTracks_[i].*spare;
                },
                [this, i, live, field, spare](double v) {
                    if (*live) (audio_.controls(i).*field).store(static_cast<float>(v));
                    else overrideTracks_[i].*spare = static_cast<float>(v);
                    InvalidateRect(window_, nullptr, FALSE);
                });
        };
        auto gainPair = band(&vp::TrackControls::gain, &OverrideTrack::gain);
        t.gain = gainPair.first;
        t.setGain = gainPair.second;
        auto lowPair = band(&vp::TrackControls::eqLow, &OverrideTrack::low);
        t.low = lowPair.first;
        t.setLow = lowPair.second;
        auto midPair = band(&vp::TrackControls::eqMid, &OverrideTrack::mid);
        t.mid = midPair.first;
        t.setMid = midPair.second;
        auto highPair = band(&vp::TrackControls::eqHigh, &OverrideTrack::high);
        t.high = highPair.first;
        t.setHigh = highPair.second;
        auto pitchPair = band(&vp::TrackControls::pitch, &OverrideTrack::pitch);
        t.pitch = pitchPair.first;
        t.setPitch = pitchPair.second;
        t.stream = [this, i] {
            wchar_t label[24];
            _snwprintf_s(label, _TRUNCATE, L"a:%d", audio_.tracks()[i].audioIndex);
            return std::wstring(label);
        };
        t.nextStream = [this, i] {
            const size_t count = audio_.tracks().size();
            if (count < 2) return;
            const int want =
                (audio_.tracks()[i].audioIndex + 1) % static_cast<int>(count);
            for (size_t j = 0; j < count; ++j) {
                if (audio_.tracks()[j].audioIndex == want) {
                    audio_.swapAudioIndex(i, j);
                    break;
                }
            }
            rereadStreams();
        };
        t.reset = [this, i, live] {
            if (*live) {
                resetEq(i);
            } else {
                overrideTracks_[i].low = overrideTracks_[i].mid =
                    overrideTracks_[i].high = overrideTracks_[i].pitch = 0.0f;
            }
            InvalidateRect(window_, nullptr, FALSE);
        };
        (void)over;
        sheet.tracks.push_back(std::move(t));
    }

    vp::runPlayerWindow(window_, instance_, sheet);
    InvalidateRect(window_, nullptr, TRUE);
}

// The row that was picked with Alt, over the piece that was picked with it.
// Whatever is set here sits on top of the window above, for that row alone.
void Player::openLocalProperties() {
    const int piece = timeline_.selected() >= 0
                          ? timeline_.selected()
                          : timeline_.segmentAt(audio_.position());
    openClipProperties(piece, timeline_.selectedRow());
}

void Player::seedOverride() {
    overrideTracks_.assign(audio_.tracks().size(), OverrideTrack{});
    for (size_t i = 0; i < audio_.tracks().size(); ++i) {
        const auto& c = audio_.controls(i);
        OverrideTrack& o = overrideTracks_[i];
        o.include = c.enabled.load();
        o.normalise = audio_.normalising() && c.normalise.load();
        o.gain = c.gain.load();
        o.low = c.eqLow.load();
        o.mid = c.eqMid.load();
        o.high = c.eqHigh.load();
        o.pitch = c.pitch.load();
    }
    for (size_t i = 0; i < adjust_.size() && i < 7; ++i) {
        overrideBands_[i] = adjust_[i].value;
    }
    overrideNormalise_ = audio_.normalising();
    overrideTarget_ = audio_.normaliseTarget();
}

double Player::sheetBand(int which) const {
    if (which < 0 || which >= static_cast<int>(adjust_.size())) return 0.0;
    return fromPlayer_ ? adjust_[static_cast<size_t>(which)].value
                       : overrideBands_[which];
}

void Player::setSheetBand(int which, double value) {
    if (which < 0 || which >= static_cast<int>(adjust_.size())) return;
    const int rounded = static_cast<int>(value + (value < 0 ? -0.5 : 0.5));
    if (fromPlayer_) {
        adjust_[static_cast<size_t>(which)].value = rounded;
        applyAdjust(static_cast<size_t>(which));
    } else {
        overrideBands_[which] = rounded;
    }
    InvalidateRect(window_, nullptr, FALSE);
}

void Player::openClipProperties(int segment, int row) {
    if (segment < 0 || segment >= static_cast<int>(timeline_.segments().size())) return;

    const vp::Segment& piece = timeline_.segments()[static_cast<size_t>(segment)];
    wchar_t when[96];
    _snwprintf_s(when, _TRUNCATE, L"%s \x2013 %s", formatTime(piece.start, true).c_str(),
                 formatTime(piece.end, true).c_str());

    const bool wasPlaying = audio_.playing();
    if (wasPlaying) { audio_.pause(); video_.setPaused(true); }

    if (row <= 0) {
        // Row 0 is the picture, and so is "no particular row": a piece with no
        // row chosen is the whole column, and the picture is what that looks
        // like on screen.
        const vp::ClipVideo before = timeline_.clipVideo(segment);
        vp::ClipVideo copy = before;
        // Written into the piece as it is set, so the picture on screen is the
        // answer to the slider rather than a surprise after Save.
        auto show = [this, segment](const vp::ClipVideo& live) {
            timeline_.clipVideo(segment) = live;
            applyClipSettings(true);
        };
        if (vp::runClipVideoWindow(window_, instance_, copy, when, show)) {
            timeline_.clipVideo(segment) = copy;
            say(tr(L"Clip updated"));
        } else {
            timeline_.clipVideo(segment) = before;   // Cancel puts it back
        }
        applyClipSettings(true);
    } else {
        const int track = timeline_.trackForRow(row);
        if (track < 0 || track >= static_cast<int>(audio_.tracks().size())) return;
        const vp::ClipAudio before = timeline_.clipAudio(segment, track);
        vp::ClipAudio copy = before;
        // The per-piece mute the strip already had and the switch in the window
        // are the same thing, so they are shown as the same thing.
        copy.silent = piece.muted.count(track) > 0 || copy.silent;
        const std::wstring name = widen(audio_.tracks()[static_cast<size_t>(track)].name);
        auto show = [this, segment, track](const vp::ClipAudio& live) {
            timeline_.clipAudio(segment, track) = live;
            applyClipSettings(true);
        };
        if (vp::runClipAudioWindow(window_, instance_, copy, name, when, show)) {
            timeline_.clipAudio(segment, track) = copy;
            auto& muted = timeline_.segments()[static_cast<size_t>(segment)].muted;
            if (copy.silent) muted.insert(track); else muted.erase(track);
            say(tr(L"Clip updated"));
        } else {
            timeline_.clipAudio(segment, track) = before;
        }
        applyClipSettings(true);
    }

    if (wasPlaying) { audio_.play(); video_.setPaused(false); }
    liveSegment_ = -2;                 // push the new settings on the next tick
    layout();
    InvalidateRect(window_, nullptr, TRUE);
}

void Player::openTextProperties(int index) {
    if (index < 0 || index >= static_cast<int>(timeline_.texts().size())) return;

    const bool wasPlaying = audio_.playing();
    if (wasPlaying) { audio_.pause(); video_.setPaused(true); }

    vp::TextItem copy = timeline_.texts()[static_cast<size_t>(index)];
    bool removed = false;
    if (vp::runTextWindow(window_, instance_, copy, audio_.duration(),
                          audio_.position(), &removed)) {
        if (removed) {
            timeline_.deleteText(index);
            say(tr(L"Caption deleted"));
        } else {
            timeline_.texts()[static_cast<size_t>(index)] = copy;
        }
        refreshCaptions();
    }

    if (wasPlaying) { audio_.play(); video_.setPaused(false); }
    InvalidateRect(window_, nullptr, TRUE);
}

void Player::addCaption() {
    if (audio_.duration() <= 0.0) return;
    const int index = timeline_.addText(audio_.position());
    openTextProperties(index);
    InvalidateRect(window_, nullptr, TRUE);
}

// The captions, as a subtitle file mpv renders over the picture. The export
// burns in the identical file, so what is on screen here is what lands in the
// output - down to the outline width and the fades.
void Player::refreshCaptions() {
    if (!video_.ready()) return;

    if (timeline_.texts().empty()) {
        if (captionsShown_) {
            const char* args[] = {"sub-remove", nullptr};
            video_.command(args);
            captionsShown_ = false;
        }
        return;
    }

    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir.resize(slash + 1);

    captionSlot_ = 1 - captionSlot_;
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%s_captions%d.ass", dir.c_str(), captionSlot_);

    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out << timeline_.assDocument(std::max(16, video_.width()),
                                     std::max(16, video_.height()));
    }

    if (captionsShown_) {
        const char* drop[] = {"sub-remove", nullptr};
        video_.command(drop);
    }
    const std::string utf8 = vp::toUtf8(path);
    const char* add[] = {"sub-add", utf8.c_str(), "select", nullptr};
    video_.command(add);
    video_.setProperty("sub-visibility", "yes");
    captionsShown_ = true;
}

// What the piece under the playhead does, pushed to the mixer and to mpv. Only
// on a change of piece: these are property writes and a shader reload, and
// doing them twenty times a second would cost more than the edit does.
void Player::applyClipSettings(bool force) {
    const int at = timeline_.segmentAt(audio_.position());
    if (!force && at == liveSegment_) return;
    liveSegment_ = at;

    // A piece with a speed of its own plays at it, on top of the speed chip.
    // Leaving the piece puts the chip's own figure back.
    const double piece =
        at >= 0 && at < static_cast<int>(timeline_.segments().size())
            ? vp::clipSpeed(timeline_.segments()[static_cast<size_t>(at)].video)
            : 1.0;
    double shiftX = 0.0, shiftY = 0.0;
    if (at >= 0 && at < static_cast<int>(timeline_.segments().size())) {
        vp::clipShift(timeline_.segments()[static_cast<size_t>(at)].video, &shiftX,
                      &shiftY);
    }
    if (std::fabs(shiftX - liveClipPanX_) > 1e-6 ||
        std::fabs(shiftY - liveClipPanY_) > 1e-6) {
        liveClipPanX_ = shiftX;
        liveClipPanY_ = shiftY;
        applyPan();
    }

    const double want = kSpeeds[speedIndex_] * piece;
    if (std::fabs(want - livePlaySpeed_) > 1e-6) {
        livePlaySpeed_ = want;
        audio_.setSpeed(want);
        video_.setBaseSpeed(want);
    }

    for (size_t i = 0; i < audio_.tracks().size(); ++i) {
        auto& c = audio_.controls(i);
        const vp::ClipAudio* clip =
            timeline_.findClipAudio(at, static_cast<int>(i));
        if (clip && clip->set) {
            c.clipGain.store(clip->gain);
            c.clipEqLow.store(clip->eqLow);
            c.clipEqMid.store(clip->eqMid);
            c.clipEqHigh.store(clip->eqHigh);
            c.clipPitch.store(clip->pitch);
            c.clipSilent.store(clip->silent);
        } else {
            c.clipGain.store(1.0f);
            c.clipEqLow.store(0.0f);
            c.clipEqMid.store(0.0f);
            c.clipEqHigh.store(0.0f);
            c.clipPitch.store(0.0f);
            c.clipSilent.store(false);
        }
    }

    pictureDueAt_ = GetTickCount() + 60;
}

// ---------------------------------------------------------------------------
// The work, written down. Everything the editor and the panels hold, and the
// film it was all done to - so opening the file again brings the cuts, the
// captions, the mix and the framing back exactly as they were left.

void Player::saveProject() {
    if (path_.empty()) return;

    std::wstring suggestion = path_;
    const size_t dot = suggestion.find_last_of(L'.');
    const size_t slash = suggestion.find_last_of(L"\\/");
    if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash)) {
        suggestion.resize(dot);
    }
    suggestion += L".vpwork";

    wchar_t file[MAX_PATH]{};
    wcsncpy_s(file, suggestion.c_str(), _TRUNCATE);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = window_;
    ofn.lpstrFilter = L"Video Player work\0*.vpwork\0";
    ofn.lpstrDefExt = L"vpwork";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = tr(L"Where the work goes");
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return;

    vp::json::Value root;
    root.set("version", 1);
    root.set("source", vp::toUtf8(path_));
    root.set("position", audio_.position());

    // ---- the picture
    vp::json::Value picture;
    static const char* const kBandKeys[] = {"contrast", "brightness", "saturation",
                                            "gamma",    "hue",        "sharpen",
                                            "denoise"};
    for (size_t i = 0; i < adjust_.size() && i < 7; ++i) {
        picture.set(kBandKeys[i], sheetBand(static_cast<int>(i)));
    }
    picture.set("rotate", rotate_);
    picture.set("flipH", flipH_);
    picture.set("flipV", flipV_);
    picture.set("portrait", portrait_);
    picture.set("fit", fitIndex_);
    picture.set("frameZoom", frameZoom_);
    picture.set("framePanX", framePanX_);
    picture.set("framePanY", framePanY_);
    picture.set("speedIndex", speedIndex_);
    picture.set("keepPitch", keepPitch_);
    root.set("picture", picture);

    // ---- the mix
    vp::json::Value mix;
    mix.set("normalise", audio_.normalising());
    mix.set("target", audio_.normaliseTarget());
    mix.set("volume", volume_level_);
    vp::json::Value tracks;
    for (size_t i = 0; i < audio_.tracks().size(); ++i) {
        const auto& c = audio_.controls(i);
        vp::json::Value t;
        t.set("stream", audio_.tracks()[i].audioIndex);
        t.set("on", c.enabled.load());
        t.set("solo", c.solo.load());
        t.set("normalise", c.normalise.load());
        t.set("gain", c.gain.load());
        t.set("low", c.eqLow.load());
        t.set("mid", c.eqMid.load());
        t.set("high", c.eqHigh.load());
        t.set("pitch", c.pitch.load());
        tracks.array().push_back(t);
    }
    mix.set("tracks", tracks);
    root.set("mix", mix);

    root.set("edit", timeline_.toJson());

    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
        say(tr(L"the work could not be written"));
        return;
    }
    out << root.dump(2);
    out.close();
    projectPath_ = file;
    say(tr(L"Work saved"));
}

void Player::openProject() {
    wchar_t file[MAX_PATH]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = window_;
    ofn.lpstrFilter = L"Video Player work\0*.vpwork\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = tr(L"Open a saved piece of work");
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return;

    std::ifstream in(file, std::ios::binary);
    if (!in) { say(tr(L"the work could not be read")); return; }
    std::stringstream buffer;
    buffer << in.rdbuf();
    const vp::json::Value root = vp::json::parse(buffer.str());

    const std::wstring source = vp::fromUtf8(root["source"].asString());
    if (source.empty()) { say(tr(L"that file holds no film")); return; }
    if (GetFileAttributesW(source.c_str()) == INVALID_FILE_ATTRIBUTES) {
        say(tr(L"the film this work was done to is not where it was"));
        return;
    }

    // The film first: opening it clears the strip and rebuilds the tracks, so
    // everything below has to be laid on top of a file that is already open.
    open(source);

    const vp::json::Value& picture = root["picture"];
    static const char* const kBandKeys[] = {"contrast", "brightness", "saturation",
                                            "gamma",    "hue",        "sharpen",
                                            "denoise"};
    for (size_t i = 0; i < adjust_.size() && i < 7; ++i) {
        adjust_[i].value = picture[kBandKeys[i]].asInt(adjust_[i].value);
        applyAdjust(i);
    }
    rotate_ = picture["rotate"].asInt(0);
    flipH_ = picture["flipH"].asBool(false);
    flipV_ = picture["flipV"].asBool(false);
    portrait_ = picture["portrait"].asBool(false);
    fitIndex_ = std::max(0, std::min(2, picture["fit"].asInt(0)));
    frameZoom_ = picture["frameZoom"].asNumber(1.0);
    framePanX_ = picture["framePanX"].asNumber(0.0);
    framePanY_ = picture["framePanY"].asNumber(0.0);
    keepPitch_ = picture["keepPitch"].asBool(true);
    audio_.setKeepPitch(keepPitch_);
    setSpeedIndex(picture["speedIndex"].asInt(speedIndex_));

    const vp::json::Value& mix = root["mix"];
    audio_.setNormalise(mix["normalise"].asBool(false));
    audio_.setNormaliseTarget(static_cast<float>(mix["target"].asNumber(-14.0)));
    setVolume(static_cast<float>(mix["volume"].asNumber(volume_level_)));
    size_t at = 0;
    for (const vp::json::Value& t : mix["tracks"].asArray()) {
        if (at >= audio_.tracks().size()) break;
        auto& c = audio_.controls(at);
        audio_.setAudioIndex(at, t["stream"].asInt(static_cast<int>(at)));
        c.enabled.store(t["on"].asBool(true));
        c.solo.store(t["solo"].asBool(false));
        c.normalise.store(t["normalise"].asBool(false));
        c.gain.store(static_cast<float>(t["gain"].asNumber(1.0)));
        c.eqLow.store(static_cast<float>(t["low"].asNumber(0.0)));
        c.eqMid.store(static_cast<float>(t["mid"].asNumber(0.0)));
        c.eqHigh.store(static_cast<float>(t["high"].asNumber(0.0)));
        c.pitch.store(static_cast<float>(t["pitch"].asNumber(0.0)));
        ++at;
    }

    timeline_.fromJson(root["edit"]);
    rereadStreams();
    refreshCaptions();
    applyFades(true);
    applyClipSettings(true);
    applyVideoFilters();
    applyGeometry();
    layout();
    seekTo(root["position"].asNumber(0.0));
    projectPath_ = file;
    say(tr(L"Work opened"));
    InvalidateRect(window_, nullptr, TRUE);
}

// The picture side of a clip change: a filter chain and a shader, both of which
// cost far too much to do per mouse move.
void Player::applyClipPicture() {
    applyVideoFilters();
    for (size_t i = 0; i < adjust_.size(); ++i) {
        if (adjust_[i].kind == AdjustKind::Property) applyAdjust(i);
    }
    applySharpen();
}

void Player::openRender() {
    if (path_.empty()) return;
    if (vp::ffmpegPath().empty()) {
        MessageBoxW(window_, tr(L"ffmpeg.exe was not found. Put it next to the player."),
                    L"Video Player", MB_ICONWARNING);
        return;
    }

    vp::RenderJob job;
    job.source = path_;
    job.duration = audio_.duration();
    job.segments = timeline_.keptSegments();
    job.fades.push_back(timeline_.fades(0));           // the picture
    for (size_t i = 0; i < audio_.tracks().size(); ++i) {
        // The strip counts its sound rows from the bottom up; the export counts
        // tracks the way the file does.
        job.fades.push_back(timeline_.fades(timeline_.rowForTrack(static_cast<int>(i))));
    }
    for (size_t i = 0; i < audio_.tracks().size(); ++i) {
        vp::RenderTrack t;
        t.name = audio_.tracks()[i].name;
        // Either what is playing, or what the combined window was told to
        // keep for the export alone.
        const bool live = fromPlayer_ || i >= overrideTracks_.size();
        const OverrideTrack& o =
            live ? OverrideTrack{} : overrideTracks_[i];
        t.include = live ? audio_.controls(i).enabled.load() : o.include;
        t.target = live ? audio_.normaliseTarget() : overrideTarget_;
        t.volume = live ? audio_.controls(i).gain.load() : o.gain;
        t.eqLow = live ? audio_.controls(i).eqLow.load() : o.low;
        t.eqMid = live ? audio_.controls(i).eqMid.load() : o.mid;
        t.eqHigh = live ? audio_.controls(i).eqHigh.load() : o.high;
        t.pitch = live ? audio_.controls(i).pitch.load() : o.pitch;
        t.normalise = live ? (audio_.normalising() &&
                              audio_.controls(i).normalise.load())
                           : (overrideNormalise_ && o.normalise);
        t.sourceRate = audio_.tracks()[i].sourceRate;
        t.sourceIndex = audio_.tracks()[i].audioIndex;
        t.editIndex = static_cast<int>(i);
        // The measured figure, when the pass that reads it has finished. That
        // turns normalising into one fixed gain in the export instead of a
        // second analysis pass over the whole file.
        if (loudnessJob_.done() && i < loudnessJob_.result().size()) {
            t.measured = loudnessJob_.result()[i];
        }
        job.tracks.push_back(t);
    }
    job.contrast = static_cast<int>(sheetBand(0));
    job.brightness = static_cast<int>(sheetBand(1));
    job.saturation = static_cast<int>(sheetBand(2));
    job.gamma = static_cast<int>(sheetBand(3));
    job.hue = static_cast<int>(sheetBand(4));
    job.sharpen = static_cast<int>(sheetBand(5));
    job.denoise = static_cast<int>(sheetBand(6));

    // How the picture was being watched, so the export can be the same film.
    job.rotate = rotate_;
    job.flipH = flipH_;
    job.flipV = flipV_;
    job.portrait = portrait_;
    job.sourceWidth = video_.width();
    job.sourceHeight = video_.height();
    job.sourceFps = video_.fps();
    job.zoom = frameZoom_;
    job.panX = framePanX_;
    job.panY = framePanY_;
    job.speed = kSpeeds[speedIndex_];
    job.keepPitch = keepPitch_;

    // The captions, written out fresh so the file the export burns in is the
    // one the strip is showing rather than whatever was last handed to mpv.
    if (!timeline_.texts().empty()) {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring dir = exe;
        const size_t slash = dir.find_last_of(L"\\/");
        if (slash != std::wstring::npos) dir.resize(slash + 1);
        const std::wstring path = dir + L"_render_captions.ass";
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (out) {
            out << timeline_.assDocument(std::max(16, video_.width()),
                                         std::max(16, video_.height()));
            out.close();
            job.captions = path;
        }
    }

    const bool wasPlaying = audio_.playing();
    if (wasPlaying) { audio_.pause(); video_.setPaused(true); }
    if (const char* dump = std::getenv("VP_CMD_LOG")) {
        vp::RenderPreset probe;
        std::ofstream log(dump, std::ios::trunc);
        log << vp::toUtf8(vp::buildRenderCommand(job, probe, L"TEST.mp4"));
    }
    if (vp::runRenderWindow(window_, instance_, job, &fromPlayer_)) {
        say(tr(L"Render finished"));
    }
    if (wasPlaying) { audio_.play(); video_.setPaused(false); }
    InvalidateRect(window_, nullptr, TRUE);
}

void Player::openSettings() {
    if (vp::runSettingsWindow(window_, instance_)) {
        vp::Settings& s = vp::settings();
        s.save();

        keepPitch_ = s.keepPitch;
        audio_.setKeepPitch(keepPitch_);
        startClock();
        SetWindowPos(window_, s.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE);

        // Turning previews off should stop what is running, not just stop the
        // next one from starting.
        if (s.waveMode == "off") {
            waveJob_.cancel();
            wavesWanted_ = false;
            timeline_.setWaveforms({});
        }
        if (s.thumbMode == "off") {
            thumbJob_.cancel();
            thumbJob_.clear();
            thumbsWanted_ = false;
            timeline_.setThumbs({});
        }
    }
    layout();
    InvalidateRect(window_, nullptr, TRUE);
}

void Player::screenshot() {
    if (path_.empty()) return;
    wchar_t stamp[64];
    SYSTEMTIME now{};
    GetLocalTime(&now);
    _snwprintf_s(stamp, _TRUNCATE, L"_%04d%02d%02d_%02d%02d%02d.png", now.wYear,
                 now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);

    std::wstring out = path_;
    const size_t dot = out.find_last_of(L'.');
    if (dot != std::wstring::npos) out.resize(dot);
    out += stamp;

    const std::string target = vp::toUtf8(out);
    const char* args[] = {"screenshot-to-file", target.c_str(), "video", nullptr};
    video_.command(args);
    say(tr(L"Screenshot saved"));
}

// -------------------------------------------------------------------- input

void Player::toggleFullscreen() {
    isFull_ = !isFull_;
    // Full screen shows the film, not the workbench: whatever the picture had
    // been dragged or zoomed to for a closer look is put back, and the wheel
    // stops zooming while it lasts. The framing set in the panel stays - that
    // is part of the film.
    zoom_ = 1.0;
    panX_ = panY_ = 0.0;
    if (isFull_) {
        GetWindowRect(window_, &beforeFull_);
        SetWindowLongPtrW(window_, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        HMONITOR mon = MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(mon, &mi);
        SetWindowPos(window_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED);
    } else {
        SetWindowLongPtrW(window_, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPos(window_, nullptr, beforeFull_.left, beforeFull_.top,
                     beforeFull_.right - beforeFull_.left,
                     beforeFull_.bottom - beforeFull_.top,
                     SWP_FRAMECHANGED | SWP_NOZORDER);
    }
    layout();
    InvalidateRect(window_, nullptr, TRUE);
}

void Player::onKey(WPARAM key) {
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const float step = vp::settings().volumeStep / 100.0f;

    if (ctrl) {
        switch (key) {
            case 'I':
                if (shift) {
                    timeline_.setMarkOut(audio_.position());
                    say(tr(L"End ") + formatTime(audio_.position(), true));
                } else {
                    timeline_.setMarkIn(audio_.position());
                    say(tr(L"Start ") + formatTime(audio_.position(), true));
                }
                InvalidateRect(window_, nullptr, TRUE);
                return;
            case 'S': screenshot(); return;
            case 'R': openRender(); return;
            case 'O': choose(); return;
            case VK_OEM_COMMA: openSettings(); return;
            // Ctrl+Shift+arrows nudge the sound against the picture, the way
            // every other player spells it.
            case VK_RIGHT: if (shift) bumpAvOffset(25); return;
            case VK_LEFT: if (shift) bumpAvOffset(-25); return;
            case VK_DOWN:
                if (shift) {
                    avOffset_ = 0.0;
                    vp::settings().avOffset = 0.0;
                    say(tr(L"Sound offset 0 ms"));
                }
                return;
            default: break;
        }
    }

    switch (key) {
        case VK_SPACE: case 'K': togglePlay(); break;
        case VK_LEFT:
            seekBy(-(shift ? vp::settings().seekLarge : vp::settings().seekSmall));
            break;
        case VK_RIGHT:
            seekBy(shift ? vp::settings().seekLarge : vp::settings().seekSmall);
            break;
        case VK_HOME: seekTo(0.0); break;
        case VK_END: seekTo(audio_.duration()); break;
        case VK_UP: setVolume(volume_level_ + step); break;
        case VK_DOWN: setVolume(volume_level_ - step); break;
        case VK_OEM_COMMA: frameStep(true); break;
        case VK_OEM_PERIOD: frameStep(false); break;
        case 'F': toggleFullscreen(); break;
        case 'E': toggleEditor(); break;
        case 'R': bumpRotate(shift ? -90 : 90); break;
        case 'H': toggleFlip(true); break;
        case 'V': toggleFlip(false); break;
        case 'P': toggleKeepPitch(); break;
        case 'I':
            statsOn_ = !statsOn_;
            statsRefreshed_ = 0;
            if (!statsOn_) video_.showText("", 1);
            InvalidateRect(window_, nullptr, FALSE);
            break;
        case 'T': {
            // The track list over the picture. In full screen the panel is not
            // there, and the digits that switch tracks are no use without
            // something that says which digit is which.
            std::wstring list = tr(L"Audio tracks");
            for (size_t i = 0; i < audio_.tracks().size(); ++i) {
                wchar_t line[160];
                _snwprintf_s(line, _TRUNCATE, L"%d  %s   %s", static_cast<int>(i) + 1,
                             audio_.controls(i).enabled.load() ? tr(L"on") : tr(L"off"),
                             widen(audio_.tracks()[i].name).c_str());
                list += L"\n";
                list += line;
            }
            video_.showText(vp::toUtf8(list), 2500);
            break;
        }
        case 'Z':
            zoom_ = 1.0;
            panX_ = panY_ = 0.0;
            applyGeometry();
            say(L"Zoom 100%");
            break;
        case 'L':
            if (shift) {
                timeline_.clearMarks();
                looping_ = false;
                say(tr(L"Loop cleared"));
            } else {
                // One key walks the whole loop: start here, end here, run it,
                // off again - which is the order it is reached in practice.
                if (timeline_.markIn() < 0.0) {
                    timeline_.setMarkIn(audio_.position());
                    say(tr(L"Loop from ") + formatTime(audio_.position(), true));
                } else if (timeline_.markOut() < 0.0) {
                    timeline_.setMarkOut(audio_.position());
                    looping_ = true;
                    say(tr(L"Loop to ") + formatTime(audio_.position(), true));
                } else {
                    looping_ = !looping_;
                    say(looping_ ? tr(L"Loop on") : tr(L"Loop off"));
                }
            }
            InvalidateRect(window_, nullptr, TRUE);
            break;
        case 'S':
            if (timeline_.splitAt(audio_.position())) {
                say(tr(L"Split at ") + formatTime(audio_.position(), true));
                InvalidateRect(window_, nullptr, TRUE);
            }
            break;
        case VK_DELETE:
            timeline_.deleteSelected();
            InvalidateRect(window_, nullptr, TRUE);
            break;
        case VK_INSERT:
            timeline_.restoreSelected();
            InvalidateRect(window_, nullptr, TRUE);
            break;
        case 'N':
            audio_.setNormalise(!audio_.normalising());
            vp::settings().normalise = audio_.normalising();
            say(audio_.normalising() ? tr(L"Normalization on")
                                     : tr(L"Normalization off"));
            break;
        case 'M':
            if (volume_level_ > 0.0f) {
                volumeBeforeMute_ = volume_level_;
                setVolume(0.0f);
            } else {
                setVolume(volumeBeforeMute_);
            }
            break;
        case VK_OEM_PLUS: case VK_ADD: bumpZoom(1.1); break;
        case VK_OEM_MINUS: case VK_SUBTRACT: bumpZoom(1.0 / 1.1); break;
        case VK_ESCAPE: if (isFull_) toggleFullscreen(); break;
        case '0': resetView(); break;
        default:
            // Digits switch tracks on and off, as they did before. Seeking by
            // tenths is what the bar is for, and putting both on the same keys
            // means one of them is always the wrong guess.
            if (key >= '1' && key <= '9') toggleTrack(static_cast<size_t>(key - '1'));
            break;
    }
}

// --------------------------------------------------------------------- tick

void Player::tick() {
    if (audio_.playing() && !videoAsleep_) {
        video_.followAudioClock(audio_.position() + avOffset_, true);
    }
    video_.drainEvents();
    applyFades();

    // ---- background work that has finished
    if (waveJob_.done() && !timeline_.hasWaveforms()) {
        timeline_.setWaveforms(waveJob_.result());
        InvalidateRect(window_, nullptr, TRUE);
    }
    if (thumbJob_.running() || (thumbJob_.done() && !timeline_.hasThumbs())) {
        timeline_.setThumbs(thumbJob_.taken());
        if (editorOpen_) InvalidateRect(window_, nullptr, TRUE);
    }

    // ---- the cuts, honoured during playback
    const double at = audio_.position();
    if (audio_.playing() && timeline_.duration() > 0.0) {
        const double lo = timeline_.outLow(), hi = timeline_.outHigh();
        if (looping_ && at >= hi - 0.05) {
            seekTo(lo);
        } else if (at < lo - 0.5) {
            seekTo(lo);
        } else {
            for (const vp::Segment& s : timeline_.segments()) {
                if (s.kept || at < s.start || at >= s.end) continue;
                // Skipped rather than silenced: a cut stretch is not in the
                // output, so playing it back would preview a file that will not
                // exist.
                seekTo(s.end + 0.02);
                break;
            }
        }

    }

    // What the piece under the playhead does to the sound and the picture. This
    // replaced an older trick that held the listener's own fader down to
    // silence a track over a stretch, which meant an edit and a fader could not
    // both be true at once - and left the fader wherever the edit had put it if
    // anything went wrong in between.
    applyClipSettings(false);

    // What each sound row is really doing, so the strip draws it that way.
    // The wave is drawn at the loudness the track is actually played at,
    // measured against the levelling target. A normalised track therefore sits
    // at the same middling height whatever the recording was, and one that is
    // not normalised is as big or as small as it really is.
    const double reference = audio_.normaliseTarget();
    for (size_t i = 0; i < audio_.tracks().size(); ++i) {
        const auto& c = audio_.controls(i);
        double measured = reference;
        if (loudnessJob_.done() && i < loudnessJob_.result().size() &&
            loudnessJob_.result()[i] > -69.0f) {
            measured = loudnessJob_.result()[i];
        } else if (c.levelReady.load()) {
            measured = c.measuredDb.load();
        }
        double played = measured;
        if (audio_.normalising() && c.normalise.load()) {
            played += c.normalisationDb.load();
        }
        // On a scale of decibels, not of amplitude: the difference between a
        // track at -14 and one at -41 is four hundred times in amplitude, which
        // draws the quiet one as a flat line. Sixty decibels of range across the
        // row puts the levelling target at about three quarters of it and still
        // leaves a quiet track visible.
        played += 20.0 * std::log10(std::max(0.0001f, c.gain.load()));
        // Decibels, with a floor under them. Loudness on its own spans far too
        // wide a range to draw straight: a track twenty-five decibels below the
        // levelling target is a hundredth of the amplitude, which is a flat
        // line. This keeps a quiet track plainly visible and still clearly
        // smaller than a loud one, with the target at about four fifths.
        // Thirty-five decibels across the row rather than sixty: the fader and
        // the levelling move a track by ten or twelve, and across sixty that
        // was a change of a few pixels nobody could see. The levelling target
        // fills the row, and a quarter of it is kept as a floor so a track that
        // is genuinely quiet is still a wave and not a line.
        const double range = std::max(0.0, std::min(1.0, (played + 49.0) / 35.0));
        float loud = static_cast<float>(0.25 + 0.75 * range);
        if (!c.enabled.load()) loud = 0.0f;
        timeline_.setRowGain(timeline_.rowForTrack(static_cast<int>(i)), loud);
    }


    {
        RECT actual{};
        GetWindowRect(videoHost_, &actual);
        MapWindowPoints(HWND_DESKTOP, window_, reinterpret_cast<POINT*>(&actual), 2);
        if (actual.left != videoCanvas_.left || actual.top != videoCanvas_.top ||
            actual.right != videoCanvas_.right || actual.bottom != videoCanvas_.bottom) {
            placeCanvas();
            InvalidateRect(window_, &videoStage_, FALSE);
        }
    }

    if (video_.width() != stageW_ || video_.height() != stageH_) {
        stageW_ = video_.width();
        stageH_ = video_.height();
        layout();
        InvalidateRect(window_, nullptr, TRUE);
    }

    if (pictureDueAt_ != 0 && GetTickCount() >= pictureDueAt_) {
        pictureDueAt_ = 0;
        applyClipPicture();
    }
    if (captionsDueAt_ != 0 && GetTickCount() >= captionsDueAt_) {
        captionsDueAt_ = 0;
        refreshCaptions();
    }
    if (sharpenDueAt_ != 0 && GetTickCount() >= sharpenDueAt_) {
        sharpenDueAt_ = 0;
        applySharpen();
    }

    refreshStats();

    // ---- where the file was left
    if (std::fabs(at - lastPositionSaved_) > 15.0) {
        lastPositionSaved_ = at;
        rememberPosition();
    }

    // Rebuilt until the picture answers: hwdec settles a moment after the file
    // loads, so asking once and keeping the answer leaves it blank.
    if (video_.width() > 0) {
        const std::string hw = video_.hwdec();
        if (infoStale_ || !hw.empty()) {
            if (!hw.empty()) infoStale_ = false;
            wchar_t top[220];
            _snwprintf_s(top, _TRUNCATE,
                         L"%d \x00d7 %d  \x00b7  %.4g fps  \x00b7  %s  \x00b7  hwdec: %s",
                         video_.width(), video_.height(), video_.fps(),
                         formatTime(audio_.duration()).c_str(),
                         hw.empty() ? L"-" : widen(hw).c_str());
            infoTop_ = top;

            // The dropout count earns its place only when it is not zero, and
            // when it is not zero it is the first thing anyone needs to see.
            wchar_t bottom[220];
            const unsigned long long lost = audio_.dropouts();
            if (lost > 0) {
                _snwprintf_s(bottom, _TRUNCATE,
                             tr(L"%d tracks  \x00b7  %.0f ms  \x00b7  %llu dropouts"),
                             static_cast<int>(audio_.tracks().size()),
                             audio_.outputLatency() * 1000.0, lost);
            } else {
                _snwprintf_s(bottom, _TRUNCATE, tr(L"%s  \x00b7  %d tracks  \x00b7  %.0f ms"),
                             widen(audio_.outputDeviceName()).c_str(),
                             static_cast<int>(audio_.tracks().size()),
                             audio_.outputLatency() * 1000.0);
            }
            infoBottom_ = bottom;
        }
    }

    RECT client{};
    GetClientRect(window_, &client);
    if (isFull_) {
        if (!osd_.empty() && GetTickCount() <= osdUntil_ + 200) {
            InvalidateRect(window_, nullptr, FALSE);
        }
        return;
    }
    const RECT video = videoArea();
    RECT transport{0, video.bottom, client.right, client.bottom};
    InvalidateRect(window_, &transport, FALSE);
    RECT panel{client.right - kPanelWidth, 44, client.right, client.bottom};
    InvalidateRect(window_, &panel, FALSE);
    if (!osd_.empty() && GetTickCount() <= osdUntil_ + 200) {
        RECT top{0, video.top, client.right - kPanelWidth, video.top + 100};
        InvalidateRect(window_, &top, FALSE);
    }
}

// ---------------------------------------------------------------- plumbing

LRESULT CALLBACK Player::proc(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    auto* self = reinterpret_cast<Player*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message) {
        case WM_NCCREATE: {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(l);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            break;
        }
        case WM_SIZE:
            if (self) {
                // Minimised means nobody is watching: the picture is switched
                // off altogether so the card stops decoding it, and the sound
                // plays on. It comes back where the sound has got to.
                self->sleepVideo(w == SIZE_MINIMIZED);
                self->layout();
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            if (self) {
                // Drawn off screen first. The transport and panel repaint many
                // times a second, and flicker is the entire cost of not.
                RECT client{};
                GetClientRect(hwnd, &client);
                // The back buffer is kept between frames. Building a screen
                // sized bitmap again for every mouse move was most of what
                // made the panels feel heavy under the pointer.
                if (!self->bufferDc_ || self->bufferSize_.cx != client.right ||
                    self->bufferSize_.cy != client.bottom) {
                    self->releaseBuffer();
                    self->bufferDc_ = CreateCompatibleDC(dc);
                    self->buffer_ = CreateCompatibleBitmap(dc, std::max(1L, client.right),
                                                           std::max(1L, client.bottom));
                    self->bufferOld_ = SelectObject(self->bufferDc_, self->buffer_);
                    self->bufferSize_ = {client.right, client.bottom};
                }
                HDC mem = self->bufferDc_;
                self->paintClip_ = ps.rcPaint;
                fill(mem, ps.rcPaint, kBack);
                self->paint(mem);
                BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top,
                       ps.rcPaint.right - ps.rcPaint.left,
                       ps.rcPaint.bottom - ps.rcPaint.top, mem, ps.rcPaint.left,
                       ps.rcPaint.top, SRCCOPY);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN:
            if (self) {
                SetCapture(hwnd);
                self->onDown({GET_X_LPARAM(l), GET_Y_LPARAM(l)});
                // Every click repaints. Several switches - levelling, solo, a
                // track on or off - only changed a flag and left the panel
                // showing the old state until something else happened to
                // invalidate the window.
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONDBLCLK:
            if (self) {
                POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                RECT picture{};
                GetClientRect(self->videoHost_, &picture);
                MapWindowPoints(self->videoHost_, hwnd,
                                reinterpret_cast<POINT*>(&picture), 2);
                if (PtInRect(&picture, p)) {
                    // The first click of the pair already toggled play, so this
                    // puts it back before going full screen.
                    self->togglePlay();
                    self->toggleFullscreen();
                } else if (self->editorOpen_ && self->timeline_.onDoubleClick(p)) {
                    if (self->timeline_.inTextRow(p)) {
                        self->openTextProperties(self->timeline_.selectedText());
                    } else {
                        // A double click on a piece opens that piece. Which row
                        // it landed on decides whether that means the picture or
                        // one of the sound tracks.
                        // The strip has just selected the piece and the row the
                        // click landed on, which is exactly what to open.
                        self->openClipProperties(self->timeline_.selected(),
                                                 self->timeline_.selectedRow());
                    }
                    InvalidateRect(hwnd, nullptr, TRUE);
                }
            }
            return 0;
        case WM_MOUSEMOVE:
            if (self) self->onMove({GET_X_LPARAM(l), GET_Y_LPARAM(l)});
            return 0;
        case WM_LBUTTONUP:
            if (self) { self->onUp(); ReleaseCapture(); }
            return 0;
        case WM_RBUTTONDOWN:
            if (self && self->editorOpen_) {
                POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                RECT strip = self->timeline_.bounds();
                const int row = self->timeline_.rowAt(p);
                if (row >= 0 && PtInRect(&strip, p)) {
                    HMENU menu = CreatePopupMenu();
                    const bool smooth =
                        self->timeline_.fades(row).curve == vp::FadeCurve::Smooth;
                    AppendMenuW(menu, MF_STRING | (smooth ? 0u : MF_CHECKED), 1,
                                tr(L"Linear fade"));
                    AppendMenuW(menu, MF_STRING | (smooth ? MF_CHECKED : 0u), 2,
                                tr(L"Smooth fade"));
                    POINT screen = p;
                    ClientToScreen(hwnd, &screen);
                    const int chosen =
                        TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x,
                                       screen.y, 0, hwnd, nullptr);
                    DestroyMenu(menu);
                    if (chosen == 1) {
                        self->timeline_.fades(row).curve = vp::FadeCurve::Linear;
                    } else if (chosen == 2) {
                        self->timeline_.fades(row).curve = vp::FadeCurve::Smooth;
                    }
                    InvalidateRect(hwnd, nullptr, TRUE);
                }
            }
            return 0;
        case WM_MOUSEWHEEL:
            if (self) {
                // The wheel reports in screen coordinates, unlike every other
                // mouse message here.
                POINT at{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                ScreenToClient(hwnd, &at);
                self->onWheel(GET_WHEEL_DELTA_WPARAM(w), at);
            }
            return 0;
        case WM_KEYDOWN:
            if (self) self->onKey(w);
            return 0;
        case WM_TIMER:
            if (self) self->tick();
            return 0;
        case WM_DROPFILES: {
            wchar_t file[MAX_PATH]{};
            if (DragQueryFileW(reinterpret_cast<HDROP>(w), 0, file, MAX_PATH) && self) {
                self->open(file);
            }
            DragFinish(reinterpret_cast<HDROP>(w));
            return 0;
        }
        case WM_CLOSE:
            if (self) {
                self->rememberPosition();
                vp::settings().save();
            }
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (self) self->releaseBuffer();
            PostQuitMessage(0);
            return 0;
        default: break;
    }
    return DefWindowProcW(hwnd, message, w, l);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetProcessDPIAware();

    std::wstring openThis;
    int count = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &count)) {
        if (count > 1) openThis = argv[1];
        LocalFree(argv);
    }

    auto player = std::make_unique<Player>();
    if (!player->create(instance, openThis)) return 1;

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    CoUninitialize();
    return 0;
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Created by FajkPes with assistance from ChatGPT Codex and Claude Code.
