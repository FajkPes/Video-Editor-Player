#include "render.h"

#include <windowsx.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

#include "lang.h"
#include "media_tools.h"
#include "theme.h"

using namespace vp::theme;

namespace vp {

namespace {

struct Encoder {
    const wchar_t* label;
    const char* name;
    bool gpu;
};

const Encoder kEncoders[] = {
    {L"HEVC \x2014 NVENC (GPU)", "hevc_nvenc", true},
    {L"H.264 \x2014 NVENC (GPU)", "h264_nvenc", true},
    {L"AV1 \x2014 NVENC (GPU)", "av1_nvenc", true},
    {L"HEVC \x2014 x265 (CPU)", "libx265", false},
    {L"H.264 \x2014 x264 (CPU)", "libx264", false},
};

const wchar_t* const kGpuPresets[] = {L"p1", L"p2", L"p3", L"p4", L"p5", L"p6", L"p7"};
const wchar_t* const kCpuPresets[] = {L"ultrafast", L"superfast", L"veryfast",
                                      L"faster",    L"fast",      L"medium",
                                      L"slow",      L"slower",    L"veryslow"};
const int kFpsChoices[] = {0, 24, 25, 30, 48, 50, 60, 120};
// The shape of the file. "As it looks" is whatever the player was showing.
const wchar_t* const kShapeNames[] = {L"as it looks", L"landscape", L"portrait"};
struct SizeChoice { const wchar_t* label; int w, h; };
const SizeChoice kSizeChoices[] = {
    {L"as the source", 0, 0},   {L"3840 \x00d7 2160", 3840, 2160},
    {L"2560 \x00d7 1440", 2560, 1440}, {L"1920 \x00d7 1080", 1920, 1080},
    {L"1280 \x00d7 720", 1280, 720},   {L"1080 \x00d7 1920", 1080, 1920},
    {L"720 \x00d7 1280", 720, 1280},
};
const int kAudioRates[] = {128, 192, 256, 320};
const float kLufsChoices[] = {-23.0f, -18.0f, -16.0f, -14.0f, -12.0f, -9.0f};

// The equaliser frequencies the mixer uses, so what is heard is what is
// written.
const int kEqBands[] = {120, 1200, 8000};

std::string number(double v, int decimals = 3) {
    char buf[48];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%.*f", decimals, v);
    return buf;
}

// atempo holds pitch but only stretches between 0.5x and 2x per pass, so an
// extreme speed becomes a chain of them.
std::string atempoChain(double speed) {
    std::string out;
    double left = speed;
    while (left > 2.0) { out += "atempo=2.0,"; left /= 2.0; }
    while (left < 0.5) { out += "atempo=0.5,"; left /= 0.5; }
    if (std::fabs(left - 1.0) > 1e-6) out += "atempo=" + number(left, 6) + ",";
    return out;
}

std::wstring widenAscii(const std::string& s) { return fromUtf8(s); }

// A path inside a filter argument. ffmpeg's parser takes a colon as the end of
// an option and a backslash as an escape, so a Windows path has to be handed
// over with the slashes turned round and the drive's colon escaped. The whole
// filter graph already sits inside double quotes on the command line, where a
// backslash means itself, so this is the only escaping the path needs.
std::string filterPath(const std::wstring& path) {
    std::string out;
    for (char c : toUtf8(path)) {
        if (c == '\\') { out += '/'; continue; }
        if (c == ':') { out += "\\:"; continue; }
        if (c == '\'') { out += "\\\'"; continue; }
        out += c;
    }
    return out;
}

// The whole picture chain for one piece: the panel's own settings with that
// piece's additions on top, in the same order and by the same arithmetic the
// player uses, so a clip looks the same in the file as it did on screen.
std::string shiftChain(double x, double y) {
    if (std::fabs(x) < 0.001 && std::fabs(y) < 0.001) return {};
    // The frame is laid on a black canvas three times its size and the middle
    // is taken back out, offset. What slid past the edge is cut off by the
    // canvas and black takes its place - which is what moving a picture out of
    // frame looks like, zoomed in or out.
    std::ostringstream c;
    c << ",pad=w=3*in_w:h=3*in_h:x=in_w:y=in_h:color=black"
      << ",crop=w=in_w/3:h=in_h/3:x=in_w/3*(1-" << number(x, 4)
      << "):y=in_h/3*(1-" << number(y, 4) << ")";
    return c.str();
}

// The zoom, as a scale and a crop back to the size that came in - so the canvas
// stays the canvas and the picture grows or shrinks inside it.
std::string zoomChain(double z) {
    if (std::fabs(z - 1.0) < 1e-6) return {};
    std::ostringstream c;
    c << ",scale=iw*" << number(z, 4) << ":ih*" << number(z, 4);
    if (z > 1.0) {
        c << ",crop=w=in_w/" << number(z, 4) << ":h=in_h/" << number(z, 4);
    } else {
        c << ",pad=w=in_w/" << number(z, 4) << ":h=in_h/" << number(z, 4)
          << ":x=(ow-iw)/2:y=(oh-ih)/2:color=black";
    }
    return c.str();
}

std::string pictureChain(const RenderJob& job, const Segment& piece) {
    const ClipVideo& v = piece.video;
    const bool has = v.set;
    const int contrast = job.contrast + (has ? v.contrast : 0);
    const int brightness = job.brightness + (has ? v.brightness : 0);
    const int saturation = job.saturation + (has ? v.saturation : 0);
    const int gamma = job.gamma + (has ? v.gamma : 0);
    const int hue = job.hue + (has ? v.hue : 0);
    const int sharpen = std::max(0, std::min(200, job.sharpen + (has ? v.sharpen : 0)));
    const int denoise = std::max(0, std::min(100, job.denoise + (has ? v.denoise : 0)));

    std::ostringstream c;
    // The portrait canvas first, because a piece's own framing is cut by it -
    // which is the order the player shows: the canvas, then what is inside it.
    if (job.portrait) {
        // Which part of a wide frame the tall canvas shows. Sliding it across
        // the source is what "move the picture in its portrait frame" means -
        // taking the middle and then shifting it only pushed it into black.
        const double t = std::max(0.0, std::min(1.0, 0.5 - job.panX * 0.5));
        c << ",crop=w=ih*9/16:h=ih:x=(iw-ih*9/16)*" << number(t, 4) << ":y=0";
    }
    // The crop next, because it decides what the rest of the chain works on.
    double cx = 0.0, cy = 0.0, cw = 1.0, ch = 1.0;
    if (clipWindow(v, &cx, &cy, &cw, &ch)) {
        c << ",crop=iw*" << number(cw, 4) << ":ih*" << number(ch, 4)
          << ":iw*" << number(cx, 4) << ":ih*" << number(cy, 4);
    }
    double sx = 0.0, sy = 0.0;
    clipShift(v, &sx, &sy);
    c << shiftChain(sx, sy);
    c << ",eq=contrast=" << number(1.0 + contrast / 100.0)
      << ":brightness=" << number(brightness / 100.0)
      << ":saturation=" << number(1.0 + saturation / 100.0)
      << ":gamma=" << number(1.0 + gamma / 100.0);
    if (hue != 0) c << ",hue=h=" << number(hue * 1.8, 1);
    if (denoise > 0) {
        const double d = denoise / 100.0;
        c << ",hqdn3d=" << number(2.0 * d, 2) << ":" << number(1.5 * d, 2) << ":"
          << number(4.0 * d, 2) << ":" << number(4.0 * d, 2);
    }
    if (sharpen > 0) {
        // The player sharpens with a contrast-adaptive shader, which ffmpeg has
        // no equivalent of. unsharp at a little over half the weight is the
        // closest match that does not ring on edges.
        c << ",unsharp=3:3:" << number(sharpen / 100.0 * 0.95, 2) << ":3:3:0";
    }
    if (has && v.opacity < 0.999f) {
        c << ",colorchannelmixer=rr=" << number(v.opacity, 3) << ":gg="
          << number(v.opacity, 3) << ":bb=" << number(v.opacity, 3);
    }
    return c.str();
}

// One sound track over one piece, again on top of what the mixer was doing.
std::string clipSoundChain(const Segment& piece, int track, int sourceRate) {
    const auto found = piece.audio.find(track);
    if (found == piece.audio.end() || !found->second.set) return {};
    const ClipAudio& a = found->second;

    std::ostringstream c;
    if (std::fabs(a.gain - 1.0f) > 0.001f) {
        c << ",volume=" << number(a.gain, 4);
    }
    const float bands[] = {a.eqLow, a.eqMid, a.eqHigh};
    for (int b = 0; b < 3; ++b) {
        if (std::fabs(bands[b]) <= 0.05f) continue;
        c << ",equalizer=f=" << kEqBands[b] << ":t=q:w=1.2:g=" << number(bands[b], 1);
    }
    if (std::fabs(a.pitch) > 0.01f) {
        const double ratio = std::pow(2.0, a.pitch / 12.0);
        const int rate = sourceRate > 0 ? sourceRate : 48000;
        std::string tempo = atempoChain(1.0 / ratio);
        if (!tempo.empty()) tempo.pop_back();        // the trailing comma
        c << ",asetrate=" << static_cast<int>(rate * ratio + 0.5);
        if (!tempo.empty()) c << "," << tempo;
        c << ",aresample=" << rate;
    }
    return c.str();
}



}  // namespace

// ------------------------------------------------------------ command line

std::wstring buildRenderCommand(const RenderJob& job, const RenderPreset& preset,
                                const std::wstring& output) {
    const Encoder& enc = kEncoders[std::max(
        0, std::min<int>(preset.encoder, _countof(kEncoders) - 1))];

    std::vector<Segment> segs = job.segments;
    if (segs.empty()) segs.push_back(Segment{0.0, job.duration, true, {}});

    std::vector<const RenderTrack*> tracks;
    for (const RenderTrack& t : job.tracks) {
        if (t.include) tracks.push_back(&t);
    }
    if (tracks.empty()) {
        for (const RenderTrack& t : job.tracks) tracks.push_back(&t);
    }

    const double speed = std::max(0.05, preset.speed);
    double total = 0.0;
    for (const Segment& s : segs) total += (s.end - s.start) / clipSpeed(s.video);
    const double outTotal = total / speed;

    std::ostringstream g;

    // ---- picture
    //
    // The captions are burned in before anything is cut, so a caption is timed
    // against the source exactly as it was while editing and a caption lying
    // across a dropped stretch loses the part that was dropped. It is the same
    // file libass was already rendering on screen.
    const char* source = "0:v";
    if (!job.captions.empty()) {
        g << "[0:v]ass=filename='" << filterPath(job.captions) << "'[vsub];";
        source = "vsub";
    }

    // Each piece is adjusted on its own before they are joined, because each
    // piece may have been given settings of its own.
    for (size_t i = 0; i < segs.size(); ++i) {
        const double piece = clipSpeed(segs[i].video);
        g << "[" << source << "]trim=start=" << number(segs[i].start)
          << ":end=" << number(segs[i].end) << ",setpts=PTS-STARTPTS";
        // This piece plays at its own speed. The sound of every track over the
        // same piece is stretched by the same figure below, so they still line
        // up once the pieces are joined.
        if (std::fabs(piece - 1.0) > 1e-6) {
            g << ",setpts=PTS/" << number(piece, 4);
        }
        g << pictureChain(job, segs[i]) << "[v" << i << "];";
    }
    for (size_t i = 0; i < segs.size(); ++i) g << "[v" << i << "]";
    g << "concat=n=" << segs.size() << ":v=1:a=0[vc];[vc]";

    if (std::fabs(speed - 1.0) > 1e-6) g << "setpts=PTS/" << number(speed, 6) << ",";
    g << "null";
    // The same shape the player was showing. The order matters and it is the
    // order mpv uses: crop, then the flips, then the rotation.
    if (job.flipH) g << ",hflip";
    if (job.flipV) g << ",vflip";
    if (job.rotate == 90) g << ",transpose=1";
    else if (job.rotate == 180) g << ",transpose=1,transpose=1";
    else if (job.rotate == 270) g << ",transpose=2";

    // Last of all, the way mpv does it: the zoom and where the picture was
    // dragged to, both cut off by the canvas they sit on.
    g << zoomChain(std::max(0.25, std::min(4.0, job.zoom)));
    g << shiftChain(job.portrait ? 0.0 : std::max(-1.0, std::min(1.0, job.panX)),
                    std::max(-1.0, std::min(1.0, job.panY)));

    if (preset.halfSize) g << ",scale=iw/2:ih/2";

    // The shape and the size of the file. The picture is fitted into the frame
    // rather than stretched or cut, and what is left over is black - the same
    // as the canvas in the player.
    int outW = preset.outWidth, outH = preset.outHeight;
    if (outW <= 0 || outH <= 0) {
        if (preset.orientation == 2) {          // portrait
            outW = 1080; outH = 1920;
        } else if (preset.orientation == 1) {   // landscape
            outW = 1920; outH = 1080;
        }
    }
    if (outW > 0 && outH > 0) {
        outW -= outW % 2;
        outH -= outH % 2;
        g << ",scale=" << outW << ":" << outH
          << ":force_original_aspect_ratio=decrease"
          << ",pad=" << outW << ":" << outH << ":(ow-iw)/2:(oh-ih)/2:color=black";
    }

    const RowFades videoFade = job.fades.empty() ? RowFades{} : job.fades[0];
    const char* videoCurve = videoFade.curve == FadeCurve::Smooth ? "hsin" : "tri";
    if (videoFade.in > 0.01) {
        g << ",fade=t=in:st=0:d=" << number(videoFade.in / speed)
          << ":curve=" << videoCurve;
    }
    if (videoFade.out > 0.01) {
        g << ",fade=t=out:st="
          << number(std::max(0.0, outTotal - videoFade.out / speed))
          << ":d=" << number(videoFade.out / speed) << ":curve=" << videoCurve;
    }
    g << "[vout];";

    // ---- sound: the same cuts per track, then level, tone, speed and fades
    for (size_t j = 0; j < tracks.size(); ++j) {
        const RenderTrack& t = *tracks[j];
        // The file's own numbering, not this list's: a stream the player
        // could not decode is still one of ffmpeg's, and counting positions
        // here would export the wrong track's settings onto the wrong track.
        const int streamIndex = t.sourceIndex;

        for (size_t i = 0; i < segs.size(); ++i) {
            g << "[0:a:" << streamIndex << "]atrim=start=" << number(segs[i].start)
              << ":end=" << number(segs[i].end) << ",asetpts=PTS-STARTPTS";
            if (segs[i].muted.count(t.editIndex) > 0) {
                g << ",volume=0";
            } else {
                g << clipSoundChain(segs[i], t.editIndex, t.sourceRate);
            }
            const double piece = clipSpeed(segs[i].video);
            if (std::fabs(piece - 1.0) > 1e-6) {
                std::string tempo = atempoChain(piece);
                if (!tempo.empty()) {
                    tempo.pop_back();               // the trailing comma
                    g << "," << tempo;
                }
            }
            g << "[a" << j << "_" << i << "];";
        }
        for (size_t i = 0; i < segs.size(); ++i) g << "[a" << j << "_" << i << "]";
        g << "concat=n=" << segs.size() << ":v=0:a=1[ac" << j << "];";

        std::ostringstream chain;
        if (t.normalise) {
            if (t.measured > -69.0f) {
                // Already measured, so the correction is a fixed gain: one pass,
                // and exactly the figure the mixer was applying while editing.
                const double gain =
                    std::max(0.05, std::min(4.0, std::pow(10.0, (t.target - t.measured) / 20.0)));
                chain << "volume=" << number(gain * t.volume, 4) << ",";
            } else {
                chain << "loudnorm=I=" << number(t.target, 1) << ":TP=-1.5:LRA=11,"
                      << "volume=" << number(t.volume, 4) << ",";
            }
        } else {
            chain << "volume=" << number(t.volume, 4) << ",";
        }

        const float bands[] = {t.eqLow, t.eqMid, t.eqHigh};
        for (int b = 0; b < 3; ++b) {
            if (std::fabs(bands[b]) <= 0.05f) continue;
            chain << "equalizer=f=" << kEqBands[b] << ":t=q:w=1.2:g="
                  << number(bands[b], 1) << ",";
        }

        // This track moved against the others, the way the mixer had it. The
        // player shifts with a delay line and ffmpeg with a resampler followed
        // by a stretch; the interval is the same, which is what was asked for.
        if (std::fabs(t.pitch) > 0.01f) {
            const double ratio = std::pow(2.0, t.pitch / 12.0);
            const int rate = t.sourceRate > 0 ? t.sourceRate : 48000;
            chain << "asetrate=" << static_cast<int>(rate * ratio + 0.5) << ","
                  << atempoChain(1.0 / ratio) << "aresample=" << rate << ",";
        }

        if (std::fabs(speed - 1.0) > 1e-6) {
            if (preset.keepPitch) {
                chain << atempoChain(speed);
            } else {
                const int rate = t.sourceRate > 0 ? t.sourceRate : 48000;
                chain << "asetrate=" << static_cast<int>(rate * speed) << ",aresample="
                      << rate << ",";
            }
        }

        const RowFades& f = job.fades.size() > j + 1 ? job.fades[j + 1] : videoFade;
        const char* curve = f.curve == FadeCurve::Smooth ? "hsin" : "tri";
        if (f.in > 0.01) {
            chain << "afade=t=in:st=0:d=" << number(f.in / speed) << ":curve=" << curve
                  << ",";
        }
        if (f.out > 0.01) {
            chain << "afade=t=out:st=" << number(std::max(0.0, outTotal - f.out / speed))
                  << ":d=" << number(f.out / speed) << ":curve=" << curve << ",";
        }

        std::string body = chain.str();
        if (!body.empty()) body.pop_back();          // the trailing comma
        if (body.empty()) body = "anull";
        g << "[ac" << j << "]" << body << "[af" << j << "];";
    }

    if (tracks.size() > 1) {
        for (size_t j = 0; j < tracks.size(); ++j) g << "[af" << j << "]";
        g << "amix=inputs=" << tracks.size() << ":normalize=0[aout]";
    } else {
        g << "[af0]anull[aout]";
    }

    // ---- the command itself
    std::wostringstream cmd;
    cmd << L"\"" << ffmpegPath() << L"\" -y -hide_banner -nostdin -nostats";
    cmd << L" -progress pipe:1";
    if (preset.hardwareDecode && enc.gpu) cmd << L" -hwaccel cuda";
    cmd << L" -i \"" << job.source << L"\"";
    cmd << L" -filter_complex \"" << widenAscii(g.str()) << L"\"";
    cmd << L" -map \"[vout]\" -map \"[aout]\"";
    cmd << L" -c:v " << widenAscii(enc.name);

    const wchar_t* speedPreset =
        enc.gpu ? kGpuPresets[std::max(0, std::min<int>(preset.speedPreset,
                                                        _countof(kGpuPresets) - 1))]
                : kCpuPresets[std::max(0, std::min<int>(preset.speedPreset,
                                                        _countof(kCpuPresets) - 1))];
    cmd << L" -preset " << speedPreset;

    if (preset.useQuality) {
        if (enc.gpu) {
            cmd << L" -rc vbr -cq " << preset.quality << L" -b:v 0";
        } else {
            cmd << L" -crf " << preset.quality;
        }
    } else {
        cmd << L" -b:v " << preset.bitrate << L"k -maxrate "
            << static_cast<int>(preset.bitrate * 1.5) << L"k -bufsize "
            << preset.bitrate * 2 << L"k";
    }
    if (preset.fps > 0) cmd << L" -r " << preset.fps;
    cmd << L" -pix_fmt yuv420p";
    cmd << L" -c:a aac -b:a " << preset.audioBitrate << L"k -ac 2";
    cmd << L" -movflags +faststart \"" << output << L"\"";
    return cmd.str();
}

// ================================================================== window

namespace {

struct Hit {
    RECT r{};
    bool has(POINT p) const { return PtInRect(&r, p) != 0; }
};

void drawChip(HDC dc, const Hit& h, const std::wstring& label, bool hot, HFONT font,
              bool live = true) {
    roundFill(dc, h.r, hot ? kRowHot : kRow, 6);
    text(dc, h.r, label, live ? kText : kTextDim,
         DT_CENTER | DT_SINGLELINE | DT_VCENTER, font);
}

void drawButton(HDC dc, const Hit& h, const std::wstring& label, bool hot, bool primary,
                HFONT font) {
    roundFill(dc, h.r, primary ? (hot ? kAccentLit : kAccent) : (hot ? kRowHot : kRow), 6);
    text(dc, h.r, label, primary ? RGB(255, 255, 255) : kText,
         DT_CENTER | DT_SINGLELINE | DT_VCENTER, font);
}

void drawCheck(HDC dc, const Hit& h, const std::wstring& label, bool on, HFONT font) {
    RECT box{h.r.left, h.r.top + 3, h.r.left + 16, h.r.top + 19};
    roundFill(dc, box, on ? kAccent : kLine, 4);
    if (on) {
        text(dc, box, L"\x2713", RGB(255, 255, 255),
             DT_CENTER | DT_SINGLELINE | DT_VCENTER, font);
    }
    text(dc, {h.r.left + 24, h.r.top, h.r.right, h.r.bottom}, label, kText,
         DT_LEFT | DT_SINGLELINE | DT_VCENTER, font);
}

class RenderWindow {
public:
    RenderWindow(const RenderJob& job) : job_(job) {
        tracks_ = job.tracks;
        // On by default, because the export anybody actually wants is the film
        // they were just watching. Turning it off is for the case where the
        // player was being used to inspect something rather than to set it up.
        preset_.speed = job.speed;
        preset_.keepPitch = job.keepPitch;
        preset_ = settings().renderPresets.empty() ? RenderPreset{}
                                                   : settings().renderPresets.front();
        if (!settings().renderPresets.empty()) presetIndex_ = 0;
    }

    bool run(HWND owner, HINSTANCE instance);

private:
    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
    void layout();
    void paint(HDC dc);
    void onDown(POINT p);
    void onDrag(POINT p);
    void start();
    void stop();
    void showCommand();
    void savePreset();
    void deletePreset();
    void tick();
    double outputSeconds() const;
    std::wstring summary() const;

    RenderJob job_;
    std::vector<RenderTrack> tracks_;
    RenderPreset preset_;
    int presetIndex_ = -1;
    bool usePlayer_ = true;
public:
    void setUsePlayer(bool on) { usePlayer_ = on; }
    bool usePlayer() const { return usePlayer_; }
private:

    // The job as it will actually be rendered: the picture geometry and the
    // playback speed either come from the player or are left out.
    RenderJob effective() const;

    HWND window_ = nullptr;
    theme::Fonts fonts_;
    POINT mouse_{};

    Hit presetChip_, presetSave_, presetDelete_;
    Hit encoderChip_, qualityCheck_, fpsChip_, speedPresetChip_, audioChip_;
    Hit bitrateSlider_, bitrateDown_, bitrateUp_;
    Hit qualitySlider_, qualityDown_, qualityUp_;
    Hit speedSlider_, pitchCheck_, hwdecCheck_, halfCheck_, playerCheck_;
    Hit shapeChip_, sizeChip_;
    Hit allTracks_, noTracks_;
    std::vector<Hit> trackCheck_, trackNorm_, trackTarget_, trackVolume_;
    Hit showCommand_, startButton_, closeButton_;
    Hit openFile_, openFolder_;
    Hit chooseFile_;
    std::wstring safeOutput(const std::wstring& wanted) const;
    bool chooseOutput();

    enum class Drag { None, Bitrate, Quality, Speed, TrackVolume };
    Drag drag_ = Drag::None;
    int dragIndex_ = -1;

    std::thread worker_;
    std::atomic<bool> running_{false}, cancel_{false};
    std::atomic<int> percent_{0};
    std::atomic<bool> succeeded_{false};
    std::wstring status_, outputPath_;
    int progressTop_ = 0, bottom_ = 0;
    bool wroteFile_ = false;
    bool wasRunning_ = false;
};

constexpr int kWindowWidth = 760;
constexpr int kRowHeight = 34;

double RenderWindow::outputSeconds() const {
    double total = 0.0;
    for (const Segment& s : job_.segments) total += s.end - s.start;
    if (total <= 0.0) total = job_.duration;
    return total / std::max(0.05, preset_.speed);
}

std::wstring RenderWindow::summary() const {
    const double out = outputSeconds();
    int included = 0;
    for (const RenderTrack& t : tracks_) included += t.include ? 1 : 0;

    wchar_t buf[320];
    const double megabytes =
        preset_.useQuality ? 0.0 : out * (preset_.bitrate + preset_.audioBitrate) / 8192.0;
    if (megabytes > 0.0) {
        _snwprintf_s(buf, _TRUNCATE,
                     tr(L"%d pieces  \x00b7  output length %d:%02d  \x00b7  "
                        L"%d sound tracks  \x00b7  about %.0f MB"),
                     static_cast<int>(job_.segments.size()), static_cast<int>(out) / 60,
                     static_cast<int>(out) % 60, included, megabytes);
    } else {
        _snwprintf_s(buf, _TRUNCATE,
                     tr(L"%d pieces  \x00b7  output length %d:%02d  \x00b7  "
                        L"%d sound tracks  \x00b7  size follows the quality"),
                     static_cast<int>(job_.segments.size()), static_cast<int>(out) / 60,
                     static_cast<int>(out) % 60, included);
    }
    return buf;
}

void RenderWindow::layout() {
    const int left = 24;
    const int right = kWindowWidth - 24;
    const int labelWidth = 150;

    presetDelete_.r = {right - 30, 26, right, 54};
    presetSave_.r = {right - 64, 26, right - 34, 54};
    presetChip_.r = {right - 250, 26, right - 68, 54};

    int y = 104;
    playerCheck_.r = {left, y, right, y + 26};
    y += kRowHeight + 8;

    auto row = [&](Hit& h, int width) {
        h.r = {left + labelWidth, y, left + labelWidth + width, y + 26};
        y += kRowHeight;
    };

    row(encoderChip_, 240);
    // Bitrate and quality share a line each: the slider is for reaching a
    // figure quickly, the two buttons for landing on an exact one.
    bitrateSlider_.r = {left + labelWidth, y + 5, right - 150, y + 21};
    bitrateDown_.r = {right - 138, y, right - 110, y + 26};
    bitrateUp_.r = {right - 106, y, right - 78, y + 26};
    y += kRowHeight;
    qualityCheck_.r = {left + labelWidth, y, left + labelWidth + 300, y + 26};
    y += kRowHeight;
    qualitySlider_.r = {left + labelWidth, y + 5, right - 150, y + 21};
    qualityDown_.r = {right - 138, y, right - 110, y + 26};
    qualityUp_.r = {right - 106, y, right - 78, y + 26};
    y += kRowHeight;
    row(fpsChip_, 130);
    row(speedPresetChip_, 160);
    row(audioChip_, 130);

    speedSlider_.r = {left + labelWidth, y + 5, left + labelWidth + 220, y + 21};
    pitchCheck_.r = {left + labelWidth + 300, y, right, y + 26};
    y += kRowHeight;
    hwdecCheck_.r = {left + labelWidth, y, left + labelWidth + 250, y + 26};
    halfCheck_.r = {left + labelWidth + 270, y, right, y + 26};
    y += kRowHeight;
    shapeChip_.r = {left + labelWidth, y, left + labelWidth + 150, y + 26};
    sizeChip_.r = {left + labelWidth + 166, y, left + labelWidth + 336, y + 26};
    y += kRowHeight + 22;

    // ---- tracks
    noTracks_.r = {right - 58, y, right, y + 24};
    allTracks_.r = {right - 118, y, right - 62, y + 24};
    y += 34;

    trackCheck_.assign(tracks_.size(), Hit{});
    trackNorm_.assign(tracks_.size(), Hit{});
    trackTarget_.assign(tracks_.size(), Hit{});
    trackVolume_.assign(tracks_.size(), Hit{});
    for (size_t i = 0; i < tracks_.size(); ++i) {
        trackCheck_[i].r = {left + 4, y, left + 220, y + 26};
        trackNorm_[i].r = {left + 236, y, left + 340, y + 26};
        trackTarget_[i].r = {left + 348, y, left + 448, y + 26};
        trackVolume_[i].r = {left + 470, y + 5, right - 56, y + 21};
        y += 32;
    }

    y += 24;
    progressTop_ = y;
    y += 34;

    closeButton_.r = {right - 110, y, right, y + 34};
    startButton_.r = {right - 250, y, right - 120, y + 34};
    openFile_.r = {left, y, left + 150, y + 34};
    openFolder_.r = {left + 158, y, left + 320, y + 34};
    chooseFile_.r = {right - 400, progressTop_ + 34, right - 260, progressTop_ + 58};
    showCommand_.r = {left, y, left + 170, y + 34};
    bottom_ = y + 34 + 24;
}

void RenderWindow::paint(HDC dc) {
    RECT client{};
    GetClientRect(window_, &client);
    fill(dc, client, kBack);

    const int left = 24;
    const int right = kWindowWidth - 24;
    const int labelWidth = 150;

    text(dc, {left, 24, left + 300, 56}, tr(L"Export"), kText,
         DT_LEFT | DT_SINGLELINE | DT_VCENTER, fonts_.bold);

    const std::wstring presetName =
        presetIndex_ >= 0 && presetIndex_ < static_cast<int>(settings().renderPresets.size())
            ? fromUtf8(settings().renderPresets[static_cast<size_t>(presetIndex_)].name)
            : L"\x2014 preset \x2014";
    drawChip(dc, presetChip_, presetName, presetChip_.has(mouse_), fonts_.sub);
    drawChip(dc, presetSave_, L"+", presetSave_.has(mouse_), fonts_.ui);
    drawChip(dc, presetDelete_, L"\x2715", presetDelete_.has(mouse_), fonts_.ui);

    text(dc, {left, 62, right, 84}, summary(), kTextDim,
         DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS, fonts_.sub);

    auto label = [&](const Hit& h, const wchar_t* s) {
        text(dc, {left, h.r.top, left + labelWidth - 12, h.r.bottom}, s, kTextDim,
             DT_LEFT | DT_SINGLELINE | DT_VCENTER, fonts_.sub);
    };

    drawCheck(dc, playerCheck_, tr(L"Take everything from the player"), usePlayer_,
              fonts_.sub);
    text(dc, {playerCheck_.r.left + 24, playerCheck_.r.bottom, right,
              playerCheck_.r.bottom + 18},
         usePlayer_
             ? tr(L"picture adjustments, rotation, flips, crop, per-track pitch, "
                  L"normalization, faders and speed \x2014 exactly as they are set now")
             : tr(L"only the cuts and the mixer; the picture is exported untouched"),
         kTextDim, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS, fonts_.tiny);

    const Encoder& enc = kEncoders[std::max(
        0, std::min<int>(preset_.encoder, _countof(kEncoders) - 1))];
    label(encoderChip_, tr(L"Codec"));
    drawChip(dc, encoderChip_, enc.label, encoderChip_.has(mouse_), fonts_.sub);

    label(bitrateDown_, tr(L"Bitrate"));
    slider(dc, bitrateSlider_.r,
           (preset_.bitrate - 500.0) / (150000.0 - 500.0), !preset_.useQuality, false);
    drawChip(dc, bitrateDown_, L"\x2212", bitrateDown_.has(mouse_), fonts_.ui,
             !preset_.useQuality);
    drawChip(dc, bitrateUp_, L"+", bitrateUp_.has(mouse_), fonts_.ui, !preset_.useQuality);
    wchar_t br[40];
    _snwprintf_s(br, _TRUNCATE, tr(L"%d kbps"), preset_.bitrate);
    text(dc, {right - 74, bitrateDown_.r.top, right, bitrateDown_.r.bottom}, br,
         preset_.useQuality ? kTextDim : kText, DT_RIGHT | DT_SINGLELINE | DT_VCENTER,
         fonts_.sub);

    drawCheck(dc, qualityCheck_, tr(L"Use quality (CQ / CRF) instead of a bitrate"),
              preset_.useQuality, fonts_.sub);

    label(qualityDown_, tr(L"CQ / CRF"));
    slider(dc, qualitySlider_.r, preset_.quality / 51.0, preset_.useQuality, false);
    drawChip(dc, qualityDown_, L"\x2212", qualityDown_.has(mouse_), fonts_.ui,
             preset_.useQuality);
    drawChip(dc, qualityUp_, L"+", qualityUp_.has(mouse_), fonts_.ui, preset_.useQuality);
    wchar_t cq[16];
    _snwprintf_s(cq, _TRUNCATE, L"%d", preset_.quality);
    text(dc, {right - 74, qualityDown_.r.top, right, qualityDown_.r.bottom}, cq,
         preset_.useQuality ? kText : kTextDim, DT_RIGHT | DT_SINGLELINE | DT_VCENTER,
         fonts_.sub);

    label(fpsChip_, tr(L"FPS"));
    drawChip(dc, fpsChip_, preset_.fps == 0 ? tr(L"as the source") : std::to_wstring(preset_.fps),
             fpsChip_.has(mouse_), fonts_.sub);

    label(speedPresetChip_, tr(L"Encoder preset"));
    const wchar_t* sp =
        enc.gpu ? kGpuPresets[std::max(0, std::min<int>(preset_.speedPreset,
                                                        _countof(kGpuPresets) - 1))]
                : kCpuPresets[std::max(0, std::min<int>(preset_.speedPreset,
                                                        _countof(kCpuPresets) - 1))];
    drawChip(dc, speedPresetChip_, sp, speedPresetChip_.has(mouse_), fonts_.sub);

    label(audioChip_, tr(L"Sound"));
    drawChip(dc, audioChip_, std::to_wstring(preset_.audioBitrate) + L" kbps",
             audioChip_.has(mouse_), fonts_.sub);

    label(speedSlider_, tr(L"Speed"));
    slider(dc, speedSlider_.r, (preset_.speed - 0.25) / 3.75,
           std::fabs(preset_.speed - 1.0) > 1e-6, false);
    wchar_t sv[24];
    _snwprintf_s(sv, _TRUNCATE, L"%.2g\x00d7", preset_.speed);
    text(dc, {speedSlider_.r.right + 10, speedSlider_.r.top - 5,
              speedSlider_.r.right + 70, speedSlider_.r.bottom + 5},
         sv, kText, DT_LEFT | DT_SINGLELINE | DT_VCENTER, fonts_.sub);
    drawCheck(dc, pitchCheck_, tr(L"Hold the pitch"), preset_.keepPitch, fonts_.sub);

    drawCheck(dc, hwdecCheck_, tr(L"Decode on the GPU"), preset_.hardwareDecode, fonts_.sub);
    drawCheck(dc, halfCheck_, tr(L"Half resolution"), preset_.halfSize, fonts_.sub);

    text(dc, {left, shapeChip_.r.top, left + labelWidth - 10, shapeChip_.r.bottom},
         tr(L"Shape and size"), kTextDim, DT_LEFT | DT_SINGLELINE | DT_VCENTER,
         fonts_.sub);
    drawChip(dc, shapeChip_, tr(kShapeNames[std::max(0, std::min(2, preset_.orientation))]),
             shapeChip_.has(mouse_), fonts_.sub);
    std::wstring sizeText = tr(L"as the source");
    for (const SizeChoice& c : kSizeChoices) {
        if (c.w == preset_.outWidth && c.h == preset_.outHeight) sizeText = tr(c.label);
    }
    drawChip(dc, sizeChip_, sizeText, sizeChip_.has(mouse_), fonts_.sub);

    // What will actually be written, worked out the same way the graph works it
    // out - so the line is an answer and not a wish.
    {
        const RenderJob out = effective();
        int w = out.sourceWidth, h = out.sourceHeight;
        if (out.rotate == 90 || out.rotate == 270) std::swap(w, h);
        if (out.portrait && h > 0) w = h * 9 / 16;
        if (preset_.halfSize) { w /= 2; h /= 2; }
        int forcedW = preset_.outWidth, forcedH = preset_.outHeight;
        if (forcedW <= 0 || forcedH <= 0) {
            if (preset_.orientation == 2) { forcedW = 1080; forcedH = 1920; }
            else if (preset_.orientation == 1) { forcedW = 1920; forcedH = 1080; }
        }
        if (forcedW > 0 && forcedH > 0) { w = forcedW; h = forcedH; }
        w -= w % 2;
        h -= h % 2;
        const double fps = preset_.fps > 0 ? preset_.fps : out.sourceFps;
        wchar_t line[220];
        _snwprintf_s(line, _TRUNCATE,
                     tr(L"Written as %d \x00d7 %d  \x00b7  %s  \x00b7  %.4g fps  \x00b7  %s"),
                     std::max(0, w), std::max(0, h),
                     h > w ? tr(L"portrait") : tr(L"landscape"), fps,
                     kEncoders[std::max(0, std::min<int>(preset_.encoder,
                                                         _countof(kEncoders) - 1))].label);
        text(dc, {left, shapeChip_.r.bottom + 6, right, shapeChip_.r.bottom + 28}, line,
             kAccentLit, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS, fonts_.sub);
    }

    // ---- tracks
    const int tracksTop = allTracks_.r.top;
    RECT rule{left, tracksTop - 14, right, tracksTop - 13};
    fill(dc, rule, kLine);
    text(dc, {left, tracksTop, left + 300, tracksTop + 24}, tr(L"Sound tracks in the export"),
         kText, DT_LEFT | DT_SINGLELINE | DT_VCENTER, fonts_.ui);
    drawChip(dc, allTracks_, tr(L"All"), allTracks_.has(mouse_), fonts_.tiny);
    drawChip(dc, noTracks_, tr(L"None"), noTracks_.has(mouse_), fonts_.tiny);

    for (size_t i = 0; i < tracks_.size(); ++i) {
        RenderTrack& t = tracks_[i];
        wchar_t name[96];
        _snwprintf_s(name, _TRUNCATE, L"#%d  %s", static_cast<int>(i) + 1,
                     fromUtf8(t.name).c_str());
        drawCheck(dc, trackCheck_[i], name, t.include, fonts_.sub);

        drawCheck(dc, trackNorm_[i], tr(L"normalize"), t.normalise, fonts_.sub);

        wchar_t target[32];
        _snwprintf_s(target, _TRUNCATE, L"%g LUFS", t.target);
        drawChip(dc, trackTarget_[i], target, trackTarget_[i].has(mouse_), fonts_.tiny,
                 t.normalise);

        slider(dc, trackVolume_[i].r, t.volume, t.include, false);
        wchar_t pct[16];
        _snwprintf_s(pct, _TRUNCATE, L"%d%%", static_cast<int>(t.volume * 100.0f + 0.5f));
        text(dc, {right - 50, trackVolume_[i].r.top - 5, right, trackVolume_[i].r.bottom + 5},
             pct, kTextDim, DT_RIGHT | DT_SINGLELINE | DT_VCENTER, fonts_.sub);
    }

    // ---- progress and buttons
    if (running_.load() || percent_.load() > 0) {
        RECT groove{left, progressTop_ + 6, right, progressTop_ + 14};
        roundFill(dc, groove, kLine, 5);
        RECT done = groove;
        done.right = groove.left + (groove.right - groove.left) * percent_.load() / 100;
        if (done.right > done.left) {
            roundFill(dc, done, succeeded_.load() ? kAccentLit : kAccent, 5);
        }
    }
    text(dc, {left, progressTop_ + 16, right, progressTop_ + 38}, status_, kTextDim,
         DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS, fonts_.sub);
    // Where it is going, and a way to say where it should go, before any of
    // it happens rather than in a dialog that appears once Render is pressed.
    text(dc, {left, progressTop_ + 36, chooseFile_.r.left - 12, progressTop_ + 56},
         outputPath_.empty() ? tr(L"nowhere chosen yet \x2013 pick a folder")
                             : tr(L"to: ") + outputPath_,
         kTextFaint, DT_LEFT | DT_SINGLELINE | DT_PATH_ELLIPSIS, fonts_.tiny);
    if (!running_.load()) {
        drawButton(dc, chooseFile_, tr(L"Where\x2026"), chooseFile_.has(mouse_), false,
                   fonts_.sub);
    }

    // Once something has been written, the two things anybody wants next.
    if (wroteFile_ && !running_.load()) {
        drawButton(dc, openFile_, tr(L"Play it"), openFile_.has(mouse_), false, fonts_.ui);
        drawButton(dc, openFolder_, tr(L"Show the folder"), openFolder_.has(mouse_), false,
                   fonts_.ui);
    } else {
        drawButton(dc, showCommand_, tr(L"Copy the command"), showCommand_.has(mouse_),
                   false, fonts_.ui);
    }
    drawButton(dc, startButton_, running_.load() ? tr(L"Cancel") : L"Render\x2026",
               startButton_.has(mouse_), true, fonts_.ui);
    drawButton(dc, closeButton_, tr(L"Close"), closeButton_.has(mouse_), false, fonts_.ui);
}

void RenderWindow::onDown(POINT p) {
    mouse_ = p;
    auto redraw = [&] { InvalidateRect(window_, nullptr, FALSE); };

    if (wroteFile_ && !running_.load() && openFile_.has(p)) {
        ShellExecuteW(window_, L"open", outputPath_.c_str(), nullptr, nullptr, SW_SHOW);
        return;
    }
    if (wroteFile_ && !running_.load() && openFolder_.has(p)) {
        // The folder, with the file already picked out in it.
        const std::wstring pick = L"/select,\"" + outputPath_ + L"\"";
        ShellExecuteW(window_, L"open", L"explorer.exe", pick.c_str(), nullptr, SW_SHOW);
        return;
    }
    if (!running_.load() && chooseFile_.has(p)) {
        chooseOutput();
        return;
    }
    if (closeButton_.has(p)) {
        if (running_.load()) stop();
        DestroyWindow(window_);
        return;
    }
    if (startButton_.has(p)) {
        if (running_.load()) stop(); else start();
        redraw();
        return;
    }
    if (showCommand_.has(p)) { showCommand(); return; }
    if (running_.load()) return;   // nothing else may change mid-render

    if (presetChip_.has(p)) {
        const int count = static_cast<int>(settings().renderPresets.size());
        if (count > 0) {
            presetIndex_ = (presetIndex_ + 2) % (count + 1) - 1;
            if (presetIndex_ >= 0) {
                preset_ = settings().renderPresets[static_cast<size_t>(presetIndex_)];
            }
        }
        redraw();
        return;
    }
    if (presetSave_.has(p)) { savePreset(); redraw(); return; }
    if (presetDelete_.has(p)) { deletePreset(); redraw(); return; }

    if (encoderChip_.has(p)) {
        preset_.encoder = (preset_.encoder + 1) % static_cast<int>(_countof(kEncoders));
        // The two preset tables are different lengths, so a switch between CPU
        // and GPU has to land somewhere that exists.
        const int limit = kEncoders[preset_.encoder].gpu
                              ? static_cast<int>(_countof(kGpuPresets))
                              : static_cast<int>(_countof(kCpuPresets));
        preset_.speedPreset = std::min(preset_.speedPreset, limit - 1);
        redraw();
        return;
    }
    if (qualityCheck_.has(p)) { preset_.useQuality = !preset_.useQuality; redraw(); return; }
    if (fpsChip_.has(p)) {
        int at = 0;
        for (int i = 0; i < static_cast<int>(_countof(kFpsChoices)); ++i) {
            if (kFpsChoices[i] == preset_.fps) at = i;
        }
        preset_.fps = kFpsChoices[(at + 1) % static_cast<int>(_countof(kFpsChoices))];
        redraw();
        return;
    }
    if (speedPresetChip_.has(p)) {
        const int limit = kEncoders[preset_.encoder].gpu
                              ? static_cast<int>(_countof(kGpuPresets))
                              : static_cast<int>(_countof(kCpuPresets));
        preset_.speedPreset = (preset_.speedPreset + 1) % limit;
        redraw();
        return;
    }
    if (audioChip_.has(p)) {
        int at = 0;
        for (int i = 0; i < static_cast<int>(_countof(kAudioRates)); ++i) {
            if (kAudioRates[i] == preset_.audioBitrate) at = i;
        }
        preset_.audioBitrate = kAudioRates[(at + 1) % static_cast<int>(_countof(kAudioRates))];
        redraw();
        return;
    }
    if (playerCheck_.has(p)) {
        usePlayer_ = !usePlayer_;
        if (usePlayer_) {
            preset_.speed = job_.speed;
            preset_.keepPitch = job_.keepPitch;
        }
        redraw();
        return;
    }
    if (pitchCheck_.has(p)) { preset_.keepPitch = !preset_.keepPitch; redraw(); return; }
    if (hwdecCheck_.has(p)) { preset_.hardwareDecode = !preset_.hardwareDecode; redraw(); return; }
    if (halfCheck_.has(p)) { preset_.halfSize = !preset_.halfSize; redraw(); return; }
    if (shapeChip_.has(p)) {
        preset_.orientation = (preset_.orientation + 1) % 3;
        redraw();
        return;
    }
    if (sizeChip_.has(p)) {
        int at = 0;
        for (int i = 0; i < static_cast<int>(_countof(kSizeChoices)); ++i) {
            if (kSizeChoices[i].w == preset_.outWidth &&
                kSizeChoices[i].h == preset_.outHeight) {
                at = i;
            }
        }
        at = (at + 1) % static_cast<int>(_countof(kSizeChoices));
        preset_.outWidth = kSizeChoices[at].w;
        preset_.outHeight = kSizeChoices[at].h;
        redraw();
        return;
    }

    if (bitrateDown_.has(p)) { preset_.bitrate = std::max(500, preset_.bitrate - 1000); redraw(); return; }
    if (bitrateUp_.has(p)) { preset_.bitrate = std::min(300000, preset_.bitrate + 1000); redraw(); return; }
    if (qualityDown_.has(p)) { preset_.quality = std::max(0, preset_.quality - 1); redraw(); return; }
    if (qualityUp_.has(p)) { preset_.quality = std::min(51, preset_.quality + 1); redraw(); return; }

    if (allTracks_.has(p)) { for (RenderTrack& t : tracks_) t.include = true; redraw(); return; }
    if (noTracks_.has(p)) { for (RenderTrack& t : tracks_) t.include = false; redraw(); return; }

    auto fraction = [&](const Hit& h) {
        const double span = h.r.right - h.r.left;
        return span <= 0 ? 0.0 : std::max(0.0, std::min(1.0, (p.x - h.r.left) / span));
    };

    if (bitrateSlider_.has(p)) {
        drag_ = Drag::Bitrate;
        preset_.bitrate = 500 + static_cast<int>(fraction(bitrateSlider_) * 149500 / 500) * 500;
        redraw();
        return;
    }
    if (qualitySlider_.has(p)) {
        drag_ = Drag::Quality;
        preset_.quality = static_cast<int>(fraction(qualitySlider_) * 51 + 0.5);
        redraw();
        return;
    }
    if (speedSlider_.has(p)) {
        drag_ = Drag::Speed;
        preset_.speed = 0.25 + std::floor(fraction(speedSlider_) * 3.75 / 0.05 + 0.5) * 0.05;
        redraw();
        return;
    }

    for (size_t i = 0; i < tracks_.size(); ++i) {
        if (trackCheck_[i].has(p)) { tracks_[i].include = !tracks_[i].include; redraw(); return; }
        if (trackNorm_[i].has(p)) { tracks_[i].normalise = !tracks_[i].normalise; redraw(); return; }
        if (trackTarget_[i].has(p)) {
            int at = 0;
            for (int k = 0; k < static_cast<int>(_countof(kLufsChoices)); ++k) {
                if (std::fabs(kLufsChoices[k] - tracks_[i].target) < 0.01f) at = k;
            }
            tracks_[i].target = kLufsChoices[(at + 1) % static_cast<int>(_countof(kLufsChoices))];
            redraw();
            return;
        }
        if (trackVolume_[i].has(p)) {
            drag_ = Drag::TrackVolume;
            dragIndex_ = static_cast<int>(i);
            tracks_[i].volume = static_cast<float>(fraction(trackVolume_[i]));
            redraw();
            return;
        }
    }
}

void RenderWindow::onDrag(POINT p) {
    mouse_ = p;
    auto fraction = [&](const Hit& h) {
        const double span = h.r.right - h.r.left;
        return span <= 0 ? 0.0 : std::max(0.0, std::min(1.0, (p.x - h.r.left) / span));
    };
    switch (drag_) {
        case Drag::Bitrate:
            preset_.bitrate = 500 + static_cast<int>(fraction(bitrateSlider_) * 149500 / 500) * 500;
            break;
        case Drag::Quality:
            preset_.quality = static_cast<int>(fraction(qualitySlider_) * 51 + 0.5);
            break;
        case Drag::Speed:
            preset_.speed = 0.25 + std::floor(fraction(speedSlider_) * 3.75 / 0.05 + 0.5) * 0.05;
            break;
        case Drag::TrackVolume:
            if (dragIndex_ >= 0) {
                tracks_[static_cast<size_t>(dragIndex_)].volume =
                    static_cast<float>(fraction(trackVolume_[static_cast<size_t>(dragIndex_)]));
            }
            break;
        default: break;
    }
    InvalidateRect(window_, nullptr, FALSE);
}

RenderJob RenderWindow::effective() const {
    RenderJob copy = job_;
    copy.tracks = tracks_;
    if (usePlayer_) return copy;

    // Off: the cuts and the mixer still apply, because those were made in this
    // window's own list and on the time axis. Everything the picture panel and
    // the transport did is dropped.
    copy.contrast = copy.brightness = copy.saturation = copy.gamma = copy.hue = 0;
    copy.sharpen = copy.denoise = 0;
    copy.rotate = 0;
    copy.flipH = copy.flipV = copy.portrait = false;
    copy.zoom = 1.0;
    copy.panX = copy.panY = 0.0;
    for (RenderTrack& t : copy.tracks) t.pitch = 0.0f;
    return copy;
}

void RenderWindow::showCommand() {
    const RenderJob copy = effective();
    const std::wstring cmd = buildRenderCommand(copy, preset_, L"VYSTUP.mp4");

    // Onto the clipboard rather than into a window of its own: the reason to
    // want it is to run it or to send it to someone, and both start with a copy.
    if (OpenClipboard(window_)) {
        EmptyClipboard();
        HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE, (cmd.size() + 1) * sizeof(wchar_t));
        if (block) {
            memcpy(GlobalLock(block), cmd.c_str(), (cmd.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(block);
            SetClipboardData(CF_UNICODETEXT, block);
        }
        CloseClipboard();
    }
    status_ = tr(L"the ffmpeg command is on the clipboard");
    InvalidateRect(window_, nullptr, FALSE);
}

void RenderWindow::savePreset() {
    RenderPreset p = preset_;
    wchar_t stamp[64];
    SYSTEMTIME now{};
    GetLocalTime(&now);
    _snwprintf_s(stamp, _TRUNCATE, L"%s %02d:%02d",
                 kEncoders[preset_.encoder].gpu ? L"GPU" : L"CPU", now.wHour, now.wMinute);
    p.name = toUtf8(stamp);
    settings().renderPresets.push_back(p);
    presetIndex_ = static_cast<int>(settings().renderPresets.size()) - 1;
    settings().save();
    status_ = tr(L"preset saved");
}

void RenderWindow::deletePreset() {
    if (presetIndex_ < 0 ||
        presetIndex_ >= static_cast<int>(settings().renderPresets.size())) {
        return;
    }
    settings().renderPresets.erase(settings().renderPresets.begin() + presetIndex_);
    presetIndex_ = -1;
    settings().save();
    status_ = tr(L"preset deleted");
}

// Two files are the same file when their full paths match, whatever case
// they were typed in - Windows does not care and neither does this.
namespace {
bool samePath(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const wchar_t x = a[i] == L'/' ? L'\\' : towlower(a[i]);
        const wchar_t y = b[i] == L'/' ? L'\\' : towlower(b[i]);
        if (x != y) return false;
    }
    return true;
}
}  // namespace

// The name the export is offered under, and the guarantee that goes with it:
// it carries this program's mark and it is never the file that was opened.
// Overwriting the recording somebody is editing is not a mistake anyone should
// be able to make by clicking through a dialog.
std::wstring RenderWindow::safeOutput(const std::wstring& wanted) const {
    static const std::wstring mark = L" - Video Player";
    std::wstring stem = wanted, ext = L".mp4";
    const size_t dot = stem.find_last_of(L'.');
    const size_t slash = stem.find_last_of(L"\\/");
    if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash)) {
        ext = stem.substr(dot);
        stem.resize(dot);
    }
    if (stem.size() < mark.size() ||
        stem.compare(stem.size() - mark.size(), mark.size(), mark) != 0) {
        stem += mark;
    }
    std::wstring out = stem + ext;
    for (int copy = 2; samePath(out, job_.source) && copy < 100; ++copy) {
        out = stem + L" (" + std::to_wstring(copy) + L")" + ext;
    }
    return out;
}

bool RenderWindow::chooseOutput() {
    std::wstring suggestion = outputPath_.empty() ? job_.source : outputPath_;
    suggestion = safeOutput(suggestion);

    wchar_t file[MAX_PATH]{};
    wcsncpy_s(file, suggestion.c_str(), _TRUNCATE);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = window_;
    ofn.lpstrFilter = L"MP4\0*.mp4\0Matroska\0*.mkv\0";
    ofn.lpstrDefExt = L"mp4";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = tr(L"Where the export goes");
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return false;

    // Whatever was typed, it comes back marked and it is not the source.
    outputPath_ = safeOutput(file);
    wroteFile_ = false;
    InvalidateRect(window_, nullptr, FALSE);
    return true;
}

void RenderWindow::start() {
    if (ffmpegPath().empty()) {
        status_ = tr(L"ffmpeg.exe was not found \x2013 put it next to the player");
        return;
    }

    if (outputPath_.empty() && !chooseOutput()) return;
    const std::wstring cmd = buildRenderCommand(effective(), preset_, outputPath_);
    const double total = outputSeconds();

    cancel_.store(false);
    succeeded_.store(false);
    percent_.store(0);
    running_.store(true);
    status_ = tr(L"rendering\x2026");

    if (worker_.joinable()) worker_.join();
    worker_ = std::thread([this, cmd, total] {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE readEnd = nullptr, writeEnd = nullptr;
        if (!CreatePipe(&readEnd, &writeEnd, &sa, 1 << 16)) {
            running_.store(false);
            return;
        }
        SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        si.hStdOutput = writeEnd;
        si.hStdError = writeEnd;

        std::wstring line = cmd;
        PROCESS_INFORMATION pi{};
        const BOOL started =
            CreateProcessW(nullptr, &line[0], nullptr, nullptr, TRUE,
                           CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr,
                           nullptr, &si, &pi);
        CloseHandle(writeEnd);
        if (!started) {
            CloseHandle(readEnd);
            running_.store(false);
            return;
        }

        // -progress writes key=value lines; out_time_us against the known
        // output length is an honest percentage, where parsing the human log
        // would break the first time ffmpeg reworded it.
        std::string pending;
        char buffer[4096];
        DWORD got = 0;
        while (ReadFile(readEnd, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
            if (cancel_.load()) {
                TerminateProcess(pi.hProcess, 1);
                break;
            }
            pending.append(buffer, got);
            size_t at;
            while ((at = pending.find('\n')) != std::string::npos) {
                const std::string entry = pending.substr(0, at);
                pending.erase(0, at + 1);
                if (entry.compare(0, 12, "out_time_us=") == 0 && total > 0.0) {
                    const double seconds = atof(entry.c_str() + 12) / 1e6;
                    percent_.store(std::max(0, std::min(100, static_cast<int>(
                                                                seconds / total * 100.0))));
                }
            }
        }
        CloseHandle(readEnd);

        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);

        succeeded_.store(code == 0 && !cancel_.load());
        if (succeeded_.load()) percent_.store(100);
        running_.store(false);
    });
}

void RenderWindow::stop() {
    cancel_.store(true);
    status_ = tr(L"cancelling\x2026");
}

void RenderWindow::tick() {
    // A member rather than a static: a second export in the same session would
    // otherwise inherit the first one's idea of whether it had finished.
    const bool now = running_.load();
    if (wasRunning_ && !now) {
        if (worker_.joinable()) worker_.join();
        if (succeeded_.load()) {
            wroteFile_ = true;
            status_ = tr(L"done: ") + outputPath_;
        } else if (cancel_.load()) {
            status_ = tr(L"cancelled");
        } else {
            status_ = tr(L"the render failed \x2013 try another codec, or copy the command and run it");
        }
    }
    wasRunning_ = now;
    if (now) {
        wchar_t s[96];
        _snwprintf_s(s, _TRUNCATE, tr(L"rendering\x2026  %d %%"), percent_.load());
        status_ = s;
    }
    InvalidateRect(window_, nullptr, FALSE);
}

LRESULT CALLBACK RenderWindow::proc(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    auto* self = reinterpret_cast<RenderWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message) {
        case WM_NCCREATE: {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(l);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            break;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            if (self) {
                RECT client{};
                GetClientRect(hwnd, &client);
                HDC mem = CreateCompatibleDC(dc);
                HBITMAP bmp = CreateCompatibleBitmap(dc, client.right, client.bottom);
                HGDIOBJ old = SelectObject(mem, bmp);
                self->paint(mem);
                BitBlt(dc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
                SelectObject(mem, old);
                DeleteObject(bmp);
                DeleteDC(mem);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN:
            if (self) { SetCapture(hwnd); self->onDown({GET_X_LPARAM(l), GET_Y_LPARAM(l)}); }
            return 0;
        case WM_MOUSEMOVE:
            if (self) {
                self->mouse_ = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                if (self->drag_ != Drag::None) self->onDrag(self->mouse_);
                else InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONUP:
            if (self) { self->drag_ = Drag::None; self->dragIndex_ = -1; ReleaseCapture(); }
            return 0;
        case WM_TIMER:
            if (self) self->tick();
            return 0;
        case WM_KEYDOWN:
            if (w == VK_ESCAPE) DestroyWindow(hwnd);
            return 0;
        case WM_CLOSE:
            if (self && self->running_.load()) { self->stop(); return 0; }
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default: break;
    }
    return DefWindowProcW(hwnd, message, w, l);
}

bool RenderWindow::run(HWND owner, HINSTANCE instance) {
    fonts_.create();
    layout();

    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.style = CS_HREDRAW | CS_VREDRAW;
    cls.lpfnWndProc = &RenderWindow::proc;
    cls.hInstance = instance;
    cls.hCursor = LoadCursor(nullptr, IDC_ARROW);
    cls.hbrBackground = CreateSolidBrush(kBack);
    cls.lpszClassName = L"VideoPlayerCppRender";
    RegisterClassExW(&cls);

    RECT frame{0, 0, kWindowWidth, bottom_};
    AdjustWindowRect(&frame, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);

    RECT ownerRect{};
    GetWindowRect(owner, &ownerRect);
    const int width = frame.right - frame.left;
    const int height = frame.bottom - frame.top;

    window_ = CreateWindowExW(WS_EX_DLGMODALFRAME, cls.lpszClassName, L"Render",
                              WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                              (ownerRect.left + ownerRect.right - width) / 2,
                              std::max<LONG>(40, (ownerRect.top + ownerRect.bottom - height) / 2),
                              width, height, owner, nullptr, instance, this);
    if (!window_) { fonts_.destroy(); return false; }

    EnableWindow(owner, FALSE);
    ShowWindow(window_, SW_SHOW);
    SetTimer(window_, 1, 120, nullptr);

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window_, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    cancel_.store(true);
    if (worker_.joinable()) worker_.join();
    fonts_.destroy();
    return wroteFile_;
}

}  // namespace

bool runRenderWindow(HWND owner, HINSTANCE instance, const RenderJob& job,
                     bool* fromPlayer) {
    RenderWindow window(job);
    // One switch, two windows: whatever it was set to in the combined clip
    // window is what the export opens with, and changing it here changes it
    // there too.
    if (fromPlayer) window.setUsePlayer(*fromPlayer);
    const bool made = window.run(owner, instance);
    if (fromPlayer) *fromPlayer = window.usePlayer();
    return made;
}

}  // namespace vp
