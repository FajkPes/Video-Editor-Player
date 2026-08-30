#include "timeline.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

#include "settings.h"

#include "lang.h"
#include "theme.h"

#pragma comment(lib, "msimg32.lib")

using namespace vp::theme;

namespace vp {

namespace {

std::wstring shortTime(double seconds) {
    if (!(seconds >= 0.0)) seconds = 0.0;
    const int total = static_cast<int>(seconds);
    wchar_t buf[32];
    if (total >= 3600) {
        _snwprintf_s(buf, _TRUNCATE, L"%d:%02d:%02d", total / 3600, (total / 60) % 60,
                     total % 60);
    } else {
        _snwprintf_s(buf, _TRUNCATE, L"%d:%02d", total / 60, total % 60);
    }
    return buf;
}

// A tick spacing that lands on a round number of seconds and leaves the labels
// far enough apart to read.
double tickStep(double secondsPerPixel) {
    const double wanted = secondsPerPixel * 92.0;
    const double steps[] = {1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600, 7200};
    for (double s : steps) {
        if (s >= wanted) return s;
    }
    return 7200.0;
}

constexpr int kFadeHandle = 7;

// A wash of colour over whatever is already there. Painting the fade wedge
// opaquely hid the waveform underneath it, which is the very thing the wedge is
// meant to be describing; blending leaves the sound visible through it.
void blendFill(HDC dc, const RECT& r, COLORREF colour, int alpha) {
    const int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return;
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    RECT all{0, 0, w, h};
    HBRUSH brush = CreateSolidBrush(colour);
    FillRect(mem, &all, brush);
    DeleteObject(brush);
    BLENDFUNCTION blend{AC_SRC_OVER, 0, static_cast<BYTE>(alpha), 0};
    AlphaBlend(dc, r.left, r.top, w, h, mem, 0, 0, w, h, blend);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

}  // namespace

// ---------------------------------------------------------------- contents

void Timeline::setMedia(double duration, const std::vector<std::string>& trackNames) {
    duration_ = std::max(0.0, duration);
    trackNames_ = trackNames;
    segments_.clear();
    if (duration_ > 0.0) segments_.push_back(Segment{0.0, duration_, true, {}});
    fades_.assign(static_cast<size_t>(rowCount()), RowFades{});
    waves_.clear();
    thumbs_.clear();
    texts_.clear();
    markIn_ = markOut_ = -1.0;
    selected_ = -1;
    selectedRow_ = -1;
    selectedText_ = -1;
}

void Timeline::clear() {
    setMedia(0.0, {});
}

int Timeline::heightFor() const {
    return kHeadHeight + kTextRow + kGap + kVideoRow + kGap + 9 +
           static_cast<int>(trackNames_.size()) * (kAudioRow + kGap) + 8;
}

int Timeline::rowTop(int row) const {
    int y = trackStart();
    for (int i = 0; i < row; ++i) y += rowHeight(i) + gapAfter(i);
    return y;
}

// The picture is one thing and the sound is another, so there is more air
// between them than there is between two sound tracks.
int Timeline::gapAfter(int row) const { return row == 0 ? kGap + 9 : kGap; }

int Timeline::rowHeight(int row) const { return row == 0 ? kVideoRow : kAudioRow; }

double Timeline::timeAt(int x) const {
    const int span = bodyRight() - bodyLeft();
    if (span <= 0 || duration_ <= 0.0) return 0.0;
    const double f = static_cast<double>(x - bodyLeft()) / span;
    return std::max(0.0, std::min(duration_, f * duration_));
}

int Timeline::xFor(double seconds) const {
    const int span = bodyRight() - bodyLeft();
    if (duration_ <= 0.0) return bodyLeft();
    const double f = std::max(0.0, std::min(1.0, seconds / duration_));
    return bodyLeft() + static_cast<int>(span * f);
}

int Timeline::segmentAt(double seconds) const {
    for (size_t i = 0; i < segments_.size(); ++i) {
        if (seconds >= segments_[i].start && seconds < segments_[i].end) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

ClipAudio& Timeline::clipAudio(int segment, int track) {
    static ClipAudio nowhere;
    if (segment < 0 || segment >= static_cast<int>(segments_.size())) return nowhere;
    return segments_[static_cast<size_t>(segment)].audio[track];
}

const ClipAudio* Timeline::findClipAudio(int segment, int track) const {
    if (segment < 0 || segment >= static_cast<int>(segments_.size())) return nullptr;
    const auto& map = segments_[static_cast<size_t>(segment)].audio;
    const auto found = map.find(track);
    return found == map.end() ? nullptr : &found->second;
}

ClipVideo& Timeline::clipVideo(int segment) {
    static ClipVideo nowhere;
    if (segment < 0 || segment >= static_cast<int>(segments_.size())) return nowhere;
    return segments_[static_cast<size_t>(segment)].video;
}

// ------------------------------------------------------------------- text

int Timeline::addText(double at) {
    TextItem item;
    item.start = std::max(0.0, std::min(std::max(0.0, duration_ - 0.5), at));
    item.end = std::min(duration_, item.start + 3.0);
    if (item.end <= item.start) item.end = item.start + 0.5;
    texts_.push_back(item);
    selectedText_ = static_cast<int>(texts_.size()) - 1;
    return selectedText_;
}

void Timeline::deleteText(int index) {
    if (index < 0 || index >= static_cast<int>(texts_.size())) return;
    texts_.erase(texts_.begin() + index);
    selectedText_ = -1;
}

namespace {

// ASS wants &HAABBGGRR, and its alpha runs the other way round from everybody
// else: 00 is opaque.
std::string assColour(COLORREF c, int opacityPercent) {
    const int alpha = 255 - std::max(0, std::min(255, opacityPercent * 255 / 100));
    char buf[24];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "&H%02X%02X%02X%02X", alpha,
                GetBValue(c), GetGValue(c), GetRValue(c));
    return buf;
}

std::string assTime(double seconds) {
    if (!(seconds >= 0.0)) seconds = 0.0;
    const int hundredths = static_cast<int>(seconds * 100.0 + 0.5);
    char buf[32];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%d:%02d:%02d.%02d",
                hundredths / 360000, (hundredths / 6000) % 60,
                (hundredths / 100) % 60, hundredths % 100);
    return buf;
}

// A line break typed into the box has to reach ASS as its own escape, and a
// brace would otherwise start an override block.
std::string assText(const std::wstring& text) {
    std::string out;
    for (wchar_t c : text) {
        if (c == L'\n') { out += "\\N"; continue; }
        if (c == L'\r') continue;
        if (c == L'{' || c == L'}') { out += ' '; continue; }
        wchar_t one[2] = {c, 0};
        out += toUtf8(one);
    }
    return out;
}

}  // namespace

std::string Timeline::assDocument(int frameWidth, int frameHeight) const {
    // Laid out against a fixed 1920x1080 stage whatever the file is, so a
    // caption sits in the same place and at the same size on a phone clip as on
    // a 4K one. libass scales the stage to the frame for us.
    (void)frameWidth;
    (void)frameHeight;
    std::ostringstream out;
    out << "[Script Info]\n"
        << "ScriptType: v4.00+\n"
        << "PlayResX: 1920\n"
        << "PlayResY: 1080\n"
        << "WrapStyle: 0\n"
        << "ScaledBorderAndShadow: yes\n"
        << "YCbCr Matrix: None\n\n"
        << "[V4+ Styles]\n"
        << "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, "
           "OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, "
           "ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
           "Alignment, MarginL, MarginR, MarginV, Encoding\n";

    for (size_t i = 0; i < texts_.size(); ++i) {
        const TextItem& t = texts_[i];
        out << "Style: s" << i << "," << toUtf8(t.font) << "," << t.size << ","
            << assColour(t.colour, t.opacity) << ","
            << assColour(t.colour, t.opacity) << ","
            << assColour(t.outlineColour, t.opacity) << ","
            << assColour(t.shadowColour, t.opacity == 100 ? 60 : t.opacity) << ","
            << (t.bold ? -1 : 0) << "," << (t.italic ? -1 : 0) << ",0,0,"
            << "100,100,0,0,1," << t.outline << "," << t.shadow << ","
            << std::max(1, std::min(9, t.grid)) << ","
            << t.marginX << "," << t.marginX << "," << t.marginY << ",1\n";
    }

    out << "\n[Events]\n"
        << "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, "
           "Effect, Text\n";
    for (size_t i = 0; i < texts_.size(); ++i) {
        const TextItem& t = texts_[i];
        if (t.end <= t.start) continue;
        // The fades are the same ones the row shows, handed to libass in the
        // milliseconds it wants.
        const int fin = static_cast<int>(t.fadeIn * 1000.0 + 0.5);
        const int fout = static_cast<int>(t.fadeOut * 1000.0 + 0.5);
        out << "Dialogue: 0," << assTime(t.start) << "," << assTime(t.end)
            << ",s" << i << ",,0,0,0,,";
        if (fin > 0 || fout > 0) out << "{\\fad(" << fin << ',' << fout << ")}";
        out << assText(t.text) << "\n";
    }
    return out.str();
}

// ----------------------------------------------------------------- editing

bool Timeline::splitAt(double seconds) {
    if (duration_ <= 0.0) return false;
    for (size_t i = 0; i < segments_.size(); ++i) {
        Segment& s = segments_[i];

        // A split has to land inside a stretch with room either side, or it
        // would make a piece of zero length that nothing could ever select.
        if (seconds <= s.start + 0.05 || seconds >= s.end - 0.05) continue;
        Segment right = s;
        right.start = seconds;
        s.end = seconds;
        segments_.insert(segments_.begin() + static_cast<long>(i) + 1, right);
        selected_ = static_cast<int>(i) + 1;
        // The picked row is kept. Cutting while one row is picked is how a
        // single track is worked on, and clearing it here meant the very next
        // Drop piece went and took the whole column.
        return true;
    }
    return false;
}

void Timeline::deleteSelected() {
    if (selected_ < 0 || selected_ >= static_cast<int>(segments_.size())) return;
    Segment& s = segments_[static_cast<size_t>(selected_)];
    if (selectedRow_ < 0) {
        s.kept = false;                     // the whole column goes
    } else if (selectedRow_ == 0) {
        s.kept = false;                     // no picture means no stretch at all
    } else {
        s.muted.insert(trackForRow(selectedRow_));   // just this track falls silent
    }
}

void Timeline::restoreSelected() {
    if (selected_ < 0 || selected_ >= static_cast<int>(segments_.size())) return;
    Segment& s = segments_[static_cast<size_t>(selected_)];
    if (selectedRow_ > 0) {
        s.muted.erase(trackForRow(selectedRow_));
    } else {
        s.kept = true;
        s.muted.clear();
    }
}

void Timeline::resetCuts() {
    segments_.clear();
    if (duration_ > 0.0) segments_.push_back(Segment{0.0, duration_, true, {}});
    selected_ = -1;
    selectedRow_ = -1;
}

void Timeline::setMarkIn(double seconds) {
    markIn_ = std::max(0.0, std::min(duration_, seconds));
    if (markOut_ >= 0.0 && markOut_ <= markIn_) markOut_ = -1.0;
}

void Timeline::setMarkOut(double seconds) {
    markOut_ = std::max(0.0, std::min(duration_, seconds));
    if (markIn_ >= 0.0 && markIn_ >= markOut_) markIn_ = -1.0;
}

void Timeline::clearMarks() { markIn_ = markOut_ = -1.0; }

double Timeline::outLow() const { return markIn_ >= 0.0 ? markIn_ : 0.0; }
double Timeline::outHigh() const { return markOut_ >= 0.0 ? markOut_ : duration_; }

std::vector<Segment> Timeline::keptSegments() const {
    const double lo = outLow(), hi = outHigh();
    std::vector<Segment> out;
    for (const Segment& s : segments_) {
        if (!s.kept) continue;
        Segment clipped = s;
        clipped.start = std::max(s.start, lo);
        clipped.end = std::min(s.end, hi);
        if (clipped.end - clipped.start > 0.02) out.push_back(clipped);
    }
    return out;
}

double Timeline::keptDuration() const {
    double total = 0.0;
    for (const Segment& s : keptSegments()) total += s.end - s.start;
    return total;
}

std::vector<Segment> Timeline::muteRanges() const {
    std::vector<Segment> out;
    for (const Segment& s : keptSegments()) {
        if (!s.muted.empty()) out.push_back(s);
    }
    return out;
}

std::wstring Timeline::statusLine() const {
    const std::vector<Segment> kept = keptSegments();
    wchar_t buf[320];
    int muted = 0;
    for (const Segment& s : kept) muted += static_cast<int>(s.muted.size());

    std::wstring extra;
    if (muted > 0) {
        wchar_t m[64];
        _snwprintf_s(m, _TRUNCATE, tr(L"   \x00b7   tracks silenced over a piece: %d"), muted);
        extra = m;
    }
    if (selected_ >= 0 && selected_ < static_cast<int>(segments_.size())) {
        const Segment& s = segments_[static_cast<size_t>(selected_)];
        wchar_t who[40];
        wcscpy_s(who, tr(L"every row"));
        if (selectedRow_ == 0) wcscpy_s(who, tr(L"picture"));
        else if (selectedRow_ > 0) {
            _snwprintf_s(who, _TRUNCATE, tr(L"track %d"), trackForRow(selectedRow_) + 1);
        }
        wchar_t sel[160];
        _snwprintf_s(sel, _TRUNCATE, tr(L"   \x00b7   selected: %s \x2013 %s (%s)"),
                     shortTime(s.start).c_str(), shortTime(s.end).c_str(), who);
        extra += sel;
    }

    _snwprintf_s(buf, _TRUNCATE, tr(L"pieces: %d   \x00b7   output length: %s%s"),
                 static_cast<int>(kept.size()), shortTime(keptDuration()).c_str(),
                 extra.c_str());
    return buf;
}

// ------------------------------------------------------------------ drawing

void Timeline::paint(HDC dc, double playhead, POINT mouse, const theme::Fonts* fonts) const {
    fill(dc, bounds_, kBack);
    if (duration_ <= 0.0) {
        text(dc, bounds_, tr(L"open a file and its tracks appear here"), kTextDim,
             DT_CENTER | DT_SINGLELINE | DT_VCENTER, fonts->sub);
        return;
    }

    paintRuler(dc, fonts);
    paintTextRow(dc, mouse, fonts);
    for (int row = 0; row < rowCount(); ++row) paintRow(dc, row, mouse, fonts);

    // The part that is not going out at all, greyed over every row at once.
    const int lo = xFor(outLow()), hi = xFor(outHigh());
    // Dimmed rather than hidden: the rows underneath still show what is there,
    // they just read as not going out. A single pixel stretched over the area
    // is the cheapest way to get a real wash instead of a pattern of lines.
    auto shade = [&](int left, int right) {
        if (right <= left) return;
        HDC layer = CreateCompatibleDC(dc);
        HBITMAP dot = CreateCompatibleBitmap(dc, 1, 1);
        HGDIOBJ old = SelectObject(layer, dot);
        RECT one{0, 0, 1, 1};
        fill(layer, one, kBack);
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 190, 0};
        AlphaBlend(dc, left, textTop(), right - left,
                   bounds_.bottom - 8 - textTop(), layer, 0, 0, 1, 1, blend);
        SelectObject(layer, old);
        DeleteObject(dot);
        DeleteDC(layer);
    };
    shade(bodyLeft(), lo);
    shade(hi, bodyRight());

    // The playhead, over everything.
    const int head = xFor(playhead);
    RECT line{head, bounds_.top + 4, head + 2, bounds_.bottom - 8};
    fill(dc, line, RGB(0xf0, 0x54, 0x54));
    RECT grip{head - 5, bounds_.top + 2, head + 7, bounds_.top + 12};
    roundFill(dc, grip, RGB(0xf0, 0x54, 0x54), 3);
}

void Timeline::paintRuler(HDC dc, const theme::Fonts* fonts) const {
    RECT head{bounds_.left, bounds_.top, bounds_.right, bounds_.top + kHeadHeight};
    fill(dc, head, kPanel);

    const int span = bodyRight() - bodyLeft();
    if (span <= 0) return;
    const double step = tickStep(duration_ / span);
    for (double t = 0.0; t <= duration_; t += step) {
        const int x = xFor(t);
        RECT tick{x, bounds_.top + kHeadHeight - 6, x + 1, bounds_.top + kHeadHeight};
        fill(dc, tick, kLine);
        text(dc, {x + 4, bounds_.top + 2, x + 74, bounds_.top + kHeadHeight - 4},
             shortTime(t), kTextDim, DT_LEFT | DT_SINGLELINE | DT_VCENTER, fonts->tiny);
    }

    // The marks, drawn as brackets so which side they hold is obvious.
    auto mark = [&](double at, bool opening) {
        if (at < 0.0) return;
        const int x = xFor(at);
        RECT stem{opening ? x : x - 2, bounds_.top, opening ? x + 2 : x,
                  bounds_.bottom - 8};
        fill(dc, stem, kAccentLit);
        RECT flag{opening ? x : x - 16, bounds_.top + 2, opening ? x + 16 : x,
                  bounds_.top + 12};
        roundFill(dc, flag, kAccentLit, 3);
    };
    mark(markIn_, true);
    mark(markOut_, false);
}

void Timeline::paintTextRow(HDC dc, POINT mouse, const theme::Fonts* fonts) const {
    const int top = textTop();

    RECT name{bounds_.left + 8, top, bounds_.left + kGutter - 8, top + kTextRow};
    text(dc, name, tr(L"text"), kTextDim, DT_LEFT | DT_SINGLELINE | DT_VCENTER,
         fonts->sub);

    RECT body{bodyLeft(), top, bodyRight(), top + kTextRow};
    roundFill(dc, body, kSection, 4);

    if (texts_.empty()) {
        text(dc, body, tr(L"double click here to add a caption"), kTextFaint,
             DT_CENTER | DT_SINGLELINE | DT_VCENTER, fonts->tiny);
        return;
    }

    for (size_t i = 0; i < texts_.size(); ++i) {
        const TextItem& t = texts_[i];
        const int left = xFor(t.start), right = xFor(t.end);
        if (right - left < 2) continue;
        RECT block{left, top + 3, right, top + kTextRow - 3};
        const bool chosen = static_cast<int>(i) == selectedText_;
        const bool hot = PtInRect(&block, mouse) != 0;
        roundBox(dc, block, chosen ? kAccentDim : (hot ? kRowHot : kRow),
                 chosen ? kAccent : (hot ? kLineHot : kLine), 4);

        // The fades, as the same wedges the sound rows use, so the two mean the
        // same thing in the same place.
        const int span = right - left;
        const double length = std::max(0.01, t.end - t.start);
        auto wedge = [&](double seconds, bool rising) {
            if (seconds <= 0.01) return;
            const int width = std::max(2, static_cast<int>(span * seconds / length));
            const int from = rising ? left : right;
            const int to = rising ? left + width : right - width;
            POINT pts[3] = {{from, block.bottom - 1},
                            {to, rising ? block.top + 1 : block.bottom - 1},
                            {rising ? to : from, block.top + 1}};
            HBRUSH brush = CreateSolidBrush(kAccentDim);
            HPEN pen = CreatePen(PS_SOLID, 1, kAccentLit);
            HGDIOBJ ob = SelectObject(dc, brush);
            HGDIOBJ op = SelectObject(dc, pen);
            Polygon(dc, pts, 3);
            SelectObject(dc, ob);
            SelectObject(dc, op);
            DeleteObject(brush);
            DeleteObject(pen);
        };
        wedge(t.fadeIn, true);
        wedge(t.fadeOut, false);

        // The ends are grips: dragging one moves when the caption starts or
        // stops, dragging the middle moves the whole thing.
        RECT startGrip{block.left, block.top, block.left + 4, block.bottom};
        RECT endGrip{block.right - 4, block.top, block.right, block.bottom};
        fill(dc, startGrip, chosen ? kAccentLit : kLineHot);
        fill(dc, endGrip, chosen ? kAccentLit : kLineHot);

        RECT label{block.left + 7, block.top, block.right - 7, block.bottom};
        text(dc, label, t.text, chosen ? RGB(255, 255, 255) : kText,
             DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS, fonts->tiny);
    }
}

void Timeline::paintRow(HDC dc, int row, POINT mouse, const theme::Fonts* fonts) const {
    const int top = rowTop(row);
    const int height = rowHeight(row);

    RECT name{bounds_.left + 8, top, bounds_.left + kGutter - 8, top + height};
    std::wstring label;
    if (row == 0) {
        label = L"V1";
    } else if (trackForRow(row) >= 0 &&
               static_cast<size_t>(trackForRow(row)) < trackNames_.size() &&
               !trackNames_[static_cast<size_t>(trackForRow(row))].empty()) {
        // The name the file gives it, so a row and its panel row agree even
        // when they are not handed over in the same order.
        const std::string& given = trackNames_[static_cast<size_t>(trackForRow(row))];
        label.assign(given.begin(), given.end());
    } else {
        wchar_t buf[64];
        _snwprintf_s(buf, _TRUNCATE, L"A%d", row);
        label = buf;
    }
    // No row in particular means the whole column: every row of the file is
    // picked, and every one of them says so.
    const bool rowPicked =
        selected_ >= 0 && (selectedRow_ < 0 || selectedRow_ == row);
    const bool rowHot = mouse.y >= top && mouse.y < top + height &&
                        mouse.x >= bounds_.left && mouse.x < bounds_.right;
    text(dc, name, label,
         rowPicked ? kAccentLit : (rowHot ? kText : kTextDim),
         DT_LEFT | DT_SINGLELINE | DT_VCENTER, fonts->sub);

    RECT body{bodyLeft(), top, bodyRight(), top + height};
    roundFill(dc, body, rowHot && !rowPicked ? kRow : kSection, 4);
    // A picked row is outlined end to end. Which row a command is about was
    // otherwise only legible from the one piece that had changed colour.
    if (rowHot && !rowPicked) {
        RECT frame{bodyLeft() - 1, top - 1, bodyRight() + 1, top + height + 1};
        roundOutline(dc, frame, kLineHot, 5);
    }
    if (rowPicked) {
        // The row is marked in its margin; the piece itself is marked below,
        // where the piece is drawn. Which is the point: on a row that has been
        // cut, only one of its pieces is what the next command is about.
        RECT tab{bounds_.left + 2, top + 2, bounds_.left + 7, top + height - 2};
        roundFill(dc, tab, kAccentLit, 2);
    }

    // The rule that separates the picture from the sound below it.
    if (row == 0) {
        RECT rule{bounds_.left + 2, top + height + 5, bounds_.right - 2,
                  top + height + 6};
        fill(dc, rule, kLine);
    }

    for (size_t i = 0; i < segments_.size(); ++i) {
        const Segment& s = segments_[i];
        const int left = xFor(s.start), right = xFor(s.end);
        if (right - left < 1) continue;
        RECT piece{left + 1, top, right - 1, top + height};

        const int track = trackForRow(row);
        const bool trackMuted = track >= 0 && s.muted.count(track) > 0;
        const bool gone = !s.kept || trackMuted;
        const bool chosen = static_cast<int>(i) == selected_ &&
                            (selectedRow_ < 0 || selectedRow_ == row);

        COLORREF colour = gone ? RGB(0x2a, 0x1c, 0x1c) : kRow;
        if (chosen) colour = gone ? RGB(0x4a, 0x24, 0x24) : kAccentDim;
        else if (PtInRect(&piece, mouse)) colour = gone ? RGB(0x36, 0x22, 0x22) : kRowHot;
        roundFill(dc, piece, colour, 4);

        if (row == 0) {
            // The film strip, clipped to this piece so a dropped stretch does
            // not carry pictures that will not be in the output.
            HRGN clip = CreateRectRgn(piece.left, piece.top + 1, piece.right,
                                      piece.bottom - 1);
            SelectClipRgn(dc, clip);
            for (const Thumb& thumb : thumbs_) {
                if (thumb.at < s.start || thumb.at > s.end || !thumb.bitmap) continue;
                const int x = xFor(thumb.at) - kThumbWidth / 2;
                HDC mem = CreateCompatibleDC(dc);
                HGDIOBJ old = SelectObject(mem, thumb.bitmap);
                const int drawn = height - 4;
                SetStretchBltMode(dc, HALFTONE);
                StretchBlt(dc, x, top + 2, drawn * kThumbWidth / kThumbHeight, drawn, mem,
                           0, 0, kThumbWidth, kThumbHeight, SRCCOPY);
                SelectObject(mem, old);
                DeleteDC(mem);
            }
            SelectClipRgn(dc, nullptr);
            DeleteObject(clip);
        } else if (track >= 0 && static_cast<size_t>(track) < waves_.size() &&
                   !waves_[static_cast<size_t>(track)].peaks.empty()) {
            const Waveform& wave = waves_[static_cast<size_t>(track)];
            const int mid = top + height / 2;

            // The wave is drawn at the size the mixer is playing it: the row's
            // fader and levelling, times whatever this piece adds on top of
            // them. A piece set silent draws as a line, which is what it is.
            float loud = rowGain(row);
            const auto found = s.audio.find(track);
            if (found != s.audio.end() && found->second.set) {
                loud *= found->second.silent ? 0.0f : found->second.gain;
            }
            if (trackMuted || !s.kept) loud = 0.0f;
            loud = std::max(0.0f, std::min(1.0f, loud));
            const int reach = static_cast<int>((height / 2 - 3) * loud);
            HPEN peakPen = CreatePen(PS_SOLID, 1, gone ? kTextDim : kAccentLit);
            HGDIOBJ oldPen = SelectObject(dc, peakPen);
            for (int x = piece.left; x < piece.right; ++x) {
                const size_t bucket = static_cast<size_t>(
                    static_cast<double>(x - bodyLeft()) / (bodyRight() - bodyLeft()) *
                    wave.peaks.size());
                if (bucket >= wave.peaks.size()) break;
                const int h = static_cast<int>(wave.peaks[bucket] * reach);
                MoveToEx(dc, x, mid - h, nullptr);
                LineTo(dc, x, mid + h + 1);
            }
            SelectObject(dc, oldPen);
            DeleteObject(peakPen);

            // The loud core drawn over the outline, so speech reads apart from
            // room noise at a glance.
            HPEN bodyPen = CreatePen(PS_SOLID, 1, gone ? kLine : kAccent);
            oldPen = SelectObject(dc, bodyPen);
            for (int x = piece.left; x < piece.right; ++x) {
                const size_t bucket = static_cast<size_t>(
                    static_cast<double>(x - bodyLeft()) / (bodyRight() - bodyLeft()) *
                    wave.rms.size());
                if (bucket >= wave.rms.size()) break;
                const int h = static_cast<int>(wave.rms[bucket] * reach);
                MoveToEx(dc, x, mid - h, nullptr);
                LineTo(dc, x, mid + h + 1);
            }
            SelectObject(dc, oldPen);
            DeleteObject(bodyPen);
        }

        if (gone) {
            text(dc, piece, trackMuted ? tr(L"silent") : tr(L"dropped"), RGB(0xd8, 0x8a, 0x8a),
                 DT_CENTER | DT_SINGLELINE | DT_VCENTER, fonts->tiny);
        }

        // Where this piece begins, when it is not the beginning of the film:
        // a clean break with one hairline in it. The bar with caps at both ends
        // that stood here read as damage to the picture rather than as a cut.
        if (i > 0) {
            // A break the width of a gap, one hairline down the middle of it,
            // and a short cap at the top so the cut is found at a glance and
            // the cuts on the rows below line up under it. Caps at both ends
            // were too loud; the bare hairline was too quiet.
            RECT cut{left - 3, top, left + 1, top + height};
            fill(dc, cut, kBack);
            RECT line{left - 1, top + 2, left, top + height - 2};
            fill(dc, line, kLineHot);
            RECT cap{left - 3, top, left + 2, top + 4};
            fill(dc, cap, kAccentLit);
        }

        // The piece everything picked is about. Two rings and a bar along
        // the top of it: one thin outline was lost against a waveform.
        if (chosen) {
            RECT frame{piece.left, top, piece.right, top + height};
            roundOutline(dc, frame, kAccentLit, 4);
            RECT inner{frame.left + 1, frame.top + 1, frame.right - 1,
                       frame.bottom - 1};
            roundOutline(dc, inner, kAccent, 4);
            RECT cap{piece.left + 2, top + 1, piece.right - 2, top + 4};
            fill(dc, cap, kAccentLit);
            RECT foot{piece.left + 2, top + height - 4, piece.right - 2,
                      top + height - 1};
            fill(dc, foot, kAccentLit);
        }
    }

    paintFadeHandles(dc, row, rowHot, fonts);
}

void Timeline::paintFadeHandles(HDC dc, int row, bool rowHot,
                                const theme::Fonts* fonts) const {
    const RowFades& f = fades_[static_cast<size_t>(row)];
    const int top = rowTop(row);
    const int height = rowHeight(row);
    const int lo = xFor(outLow()), hi = xFor(outHigh());
    const bool smooth = f.curve == FadeCurve::Smooth;

    // The ramp is drawn as what it does to the sound rather than as a plain
    // triangle: the part it takes away is washed dark, the part it leaves is
    // tinted with the accent, and the boundary between the two is the curve the
    // export will actually apply. That reads at any length - the wash is the
    // whole wedge on a long fade, and on a short one the full-height marker at
    // the end of the ramp and the figure beside it still say it is there.
    auto ramp = [&](int fromX, int toX, bool rising, double seconds) {
        const int width = std::abs(toX - fromX);
        const int bottom = top + height - 1;
        const int roof = top + 1;
        const int left = std::min(fromX, toX), right = std::max(fromX, toX);

        if (width >= 2) {
            const int steps = std::max(2, std::min(96, width));
            std::vector<POINT> curve;
            curve.reserve(static_cast<size_t>(steps) + 3);
            for (int i = 0; i <= steps; ++i) {
                const double t = static_cast<double>(i) / steps;
                const double along = rising ? t : 1.0 - t;
                const double gain = smooth ? (1.0 - std::cos(along * 3.14159265358979)) / 2.0
                                           : along;
                curve.push_back({fromX + (toX - fromX) * i / steps,
                                 bottom - static_cast<int>((bottom - roof) * gain)});
            }

            const RECT band{left, roof, right + 1, bottom + 1};

            // What the fade removes, clipped to the area above the curve.
            std::vector<POINT> above = curve;
            above.push_back({toX, roof});
            above.push_back({fromX, roof});
            if (HRGN rgn = CreatePolygonRgn(above.data(), static_cast<int>(above.size()),
                                            WINDING)) {
                SelectClipRgn(dc, rgn);
                blendFill(dc, band, RGB(0x08, 0x09, 0x0c), 170);
                SelectClipRgn(dc, nullptr);
                DeleteObject(rgn);
            }

            // What it leaves, clipped to the area below it.
            std::vector<POINT> below = curve;
            below.push_back({toX, bottom});
            below.push_back({fromX, bottom});
            if (HRGN rgn = CreatePolygonRgn(below.data(), static_cast<int>(below.size()),
                                            WINDING)) {
                SelectClipRgn(dc, rgn);
                blendFill(dc, band, kAccent, 46);
                SelectClipRgn(dc, nullptr);
                DeleteObject(rgn);
            }

            // The curve itself, over both, thick enough to read over a waveform.
            HPEN line = CreatePen(PS_SOLID, 2, kAccentLit);
            HGDIOBJ oldLine = SelectObject(dc, line);
            Polyline(dc, curve.data(), steps + 1);
            SelectObject(dc, oldLine);
            DeleteObject(line);
        }

        // The marker at the end of the ramp: the one part that is the same size
        // whether the fade lasts a second or a minute.
        RECT edge{toX - 1, roof, toX + 1, bottom + 1};
        fill(dc, edge, kAccentLit);
        RECT foot{left, bottom - 1, right + 1, bottom + 1};
        if (foot.right - foot.left >= 2) fill(dc, foot, kAccentLit);

        // The length, over the middle of the ramp when it is wide enough to
        // hold it and just past the marker when it is not.
        if (fonts == nullptr) return;

        // The length as a badge rather than a whisper: while a fade is being
        // dragged the figure is the only thing being aimed at, and faint text
        // over a waveform is not a figure anybody can aim with.
        const bool dragging = (drag_ == Drag::FadeIn || drag_ == Drag::FadeOut) &&
                              dragRow_ == row;
        wchar_t label[40];
        _snwprintf_s(label, _TRUNCATE, L"%.2f s%s", seconds,
                     smooth ? L"  \x223f" : L"");
        SIZE size{};
        HGDIOBJ oldFont = SelectObject(dc, dragging ? fonts->semi : fonts->tiny);
        GetTextExtentPoint32W(dc, label, static_cast<int>(wcslen(label)), &size);
        SelectObject(dc, oldFont);

        const int padding = dragging ? 8 : 6;
        const int boxW = size.cx + padding * 2;
        const int boxH = dragging ? 22 : 16;
        // Beside the end of the ramp, on whichever side there is room.
        int boxLeft = rising ? toX + 6 : toX - 6 - boxW;
        boxLeft = std::max(bodyLeft() + 2, std::min(bodyRight() - boxW - 2, boxLeft));
        RECT badge{boxLeft, roof + 1, boxLeft + boxW, roof + 1 + boxH};
        roundFill(dc, badge, dragging ? kAccent : kAccentDim, 4);
        text(dc, badge, label, dragging ? RGB(255, 255, 255) : kAccentLit,
             DT_CENTER | DT_SINGLELINE | DT_VCENTER,
             dragging ? fonts->semi : fonts->tiny);
    };
    if (f.in > 0.01) ramp(lo, xFor(outLow() + f.in), true, f.in);
    if (f.out > 0.01) ramp(xFor(outHigh() - f.out), hi, false, f.out);

    // A grip standing up the row rather than a disc lying on top of it: the
    // disc sat over the picture at the very start of the strip and read as a
    // smudge on the content. A row with no fade shows nothing at all until the
    // pointer is over it, so an untouched strip stays clean.
    auto handle = [&](int x, double seconds) {
        const bool set = seconds > 0.01;
        // A tab sitting on the shoulder of the row: half of it in the gap above
        // the row, half over its very top edge, so it is always visible without
        // standing over the picture or the waveform.
        RECT grip{x - 13, top - 4, x + 13, top + 6};
        roundFill(dc, grip, set ? kAccentLit : (rowHot ? kLineHot : kLine), 4);
        // Two rules across it, one shorter than the other: a ramp, small.
        const int mid = top + 1;
        RECT a{x - 7, mid, x + 7, mid + 1};
        fill(dc, a, set ? kAccentDk : kSection);
        RECT b{x - 7, mid + 3, x + (seconds > 0.01 ? 3 : 0), mid + 4};
        fill(dc, b, set ? kAccentDk : kSection);
    };
    handle(f.in > 0.01 ? xFor(outLow() + f.in) : lo + kFadeHandle, f.in);
    handle(f.out > 0.01 ? xFor(outHigh() - f.out) : hi - kFadeHandle, f.out);
}

// ------------------------------------------------------------------- storing

json::Value Timeline::toJson() const {
    json::Value out;
    out.set("duration", duration_);
    out.set("markIn", markIn_);
    out.set("markOut", markOut_);

    json::Value pieces;
    for (const Segment& s : segments_) {
        json::Value piece;
        piece.set("start", s.start);
        piece.set("end", s.end);
        piece.set("kept", s.kept);
        json::Value muted;
        for (int t : s.muted) muted.array().push_back(json::Value(t));
        piece.set("muted", muted);

        if (s.video.set) {
            json::Value v;
            v.set("opacity", s.video.opacity);
            v.set("contrast", s.video.contrast);
            v.set("brightness", s.video.brightness);
            v.set("saturation", s.video.saturation);
            v.set("gamma", s.video.gamma);
            v.set("hue", s.video.hue);
            v.set("sharpen", s.video.sharpen);
            v.set("denoise", s.video.denoise);
            v.set("cropX", s.video.cropX);
            v.set("cropY", s.video.cropY);
            v.set("cropW", s.video.cropW);
            v.set("cropH", s.video.cropH);
            v.set("panX", s.video.panX);
            v.set("panY", s.video.panY);
            v.set("zoom", s.video.zoom);
            v.set("speed", s.video.speed);
            piece.set("video", v);
        }

        json::Value sounds;
        for (const auto& pair : s.audio) {
            if (!pair.second.set) continue;
            json::Value a;
            a.set("track", pair.first);
            a.set("gain", pair.second.gain);
            a.set("low", pair.second.eqLow);
            a.set("mid", pair.second.eqMid);
            a.set("high", pair.second.eqHigh);
            a.set("pitch", pair.second.pitch);
            a.set("silent", pair.second.silent);
            sounds.array().push_back(a);
        }
        piece.set("audio", sounds);
        pieces.array().push_back(piece);
    }
    out.set("pieces", pieces);

    json::Value fades;
    for (const RowFades& f : fades_) {
        json::Value one;
        one.set("in", f.in);
        one.set("out", f.out);
        one.set("smooth", f.curve == FadeCurve::Smooth);
        fades.array().push_back(one);
    }
    out.set("fades", fades);

    json::Value texts;
    for (const TextItem& t : texts_) {
        json::Value one;
        one.set("start", t.start);
        one.set("end", t.end);
        one.set("text", toUtf8(t.text));
        one.set("font", toUtf8(t.font));
        one.set("size", t.size);
        one.set("bold", t.bold);
        one.set("italic", t.italic);
        one.set("colour", static_cast<double>(t.colour));
        one.set("outlineColour", static_cast<double>(t.outlineColour));
        one.set("shadowColour", static_cast<double>(t.shadowColour));
        one.set("outline", t.outline);
        one.set("shadow", t.shadow);
        one.set("opacity", t.opacity);
        one.set("grid", t.grid);
        one.set("marginX", t.marginX);
        one.set("marginY", t.marginY);
        one.set("fadeIn", t.fadeIn);
        one.set("fadeOut", t.fadeOut);
        texts.array().push_back(one);
    }
    out.set("texts", texts);
    return out;
}

void Timeline::fromJson(const json::Value& v) {
    if (!v.isObject()) return;
    markIn_ = v["markIn"].asNumber(-1.0);
    markOut_ = v["markOut"].asNumber(-1.0);

    const auto& pieces = v["pieces"].asArray();
    if (!pieces.empty()) {
        segments_.clear();
        for (const json::Value& p : pieces) {
            Segment s;
            s.start = p["start"].asNumber();
            s.end = p["end"].asNumber();
            s.kept = p["kept"].asBool(true);
            for (const json::Value& m : p["muted"].asArray()) {
                s.muted.insert(m.asInt());
            }
            const json::Value& vv = p["video"];
            if (vv.isObject()) {
                s.video.set = true;
                s.video.opacity = static_cast<float>(vv["opacity"].asNumber(1.0));
                s.video.contrast = vv["contrast"].asInt();
                s.video.brightness = vv["brightness"].asInt();
                s.video.saturation = vv["saturation"].asInt();
                s.video.gamma = vv["gamma"].asInt();
                s.video.hue = vv["hue"].asInt();
                s.video.sharpen = vv["sharpen"].asInt();
                s.video.denoise = vv["denoise"].asInt();
                s.video.cropX = vv["cropX"].asNumber(0.0);
                s.video.cropY = vv["cropY"].asNumber(0.0);
                s.video.cropW = vv["cropW"].asNumber(1.0);
                s.video.cropH = vv["cropH"].asNumber(1.0);
                s.video.panX = vv["panX"].asNumber(0.0);
                s.video.panY = vv["panY"].asNumber(0.0);
                s.video.zoom = vv["zoom"].asNumber(1.0);
                s.video.speed = vv["speed"].asNumber(1.0);
            }
            for (const json::Value& a : p["audio"].asArray()) {
                ClipAudio clip;
                clip.set = true;
                clip.gain = static_cast<float>(a["gain"].asNumber(1.0));
                clip.eqLow = static_cast<float>(a["low"].asNumber());
                clip.eqMid = static_cast<float>(a["mid"].asNumber());
                clip.eqHigh = static_cast<float>(a["high"].asNumber());
                clip.pitch = static_cast<float>(a["pitch"].asNumber());
                clip.silent = a["silent"].asBool(false);
                s.audio[a["track"].asInt()] = clip;
            }
            segments_.push_back(s);
        }
    }

    const auto& fades = v["fades"].asArray();
    for (size_t i = 0; i < fades.size() && i < fades_.size(); ++i) {
        fades_[i].in = fades[i]["in"].asNumber();
        fades_[i].out = fades[i]["out"].asNumber();
        fades_[i].curve =
            fades[i]["smooth"].asBool() ? FadeCurve::Smooth : FadeCurve::Linear;
    }

    texts_.clear();
    for (const json::Value& t : v["texts"].asArray()) {
        TextItem item;
        item.start = t["start"].asNumber();
        item.end = t["end"].asNumber(item.start + 3.0);
        item.text = fromUtf8(t["text"].asString());
        item.font = fromUtf8(t["font"].asString("Segoe UI"));
        item.size = t["size"].asInt(48);
        item.bold = t["bold"].asBool(true);
        item.italic = t["italic"].asBool(false);
        item.colour = static_cast<COLORREF>(t["colour"].asNumber(0xFFFFFF));
        item.outlineColour = static_cast<COLORREF>(t["outlineColour"].asNumber(0));
        item.shadowColour = static_cast<COLORREF>(t["shadowColour"].asNumber(0));
        item.outline = t["outline"].asInt(2);
        item.shadow = t["shadow"].asInt(0);
        item.opacity = t["opacity"].asInt(100);
        item.grid = t["grid"].asInt(2);
        item.marginX = t["marginX"].asInt(40);
        item.marginY = t["marginY"].asInt(40);
        item.fadeIn = t["fadeIn"].asNumber(0.25);
        item.fadeOut = t["fadeOut"].asNumber(0.25);
        texts_.push_back(item);
    }

    selected_ = -1;
    selectedRow_ = -1;
    selectedText_ = -1;
}

// -------------------------------------------------------------- interaction

bool Timeline::inTextRow(POINT p) const {
    return p.y >= textTop() && p.y < textTop() + kTextRow;
}

int Timeline::textAt(POINT p) const {
    if (!inTextRow(p)) return -1;
    // Backwards, so the one drawn last - the one on top - is the one picked.
    for (int i = static_cast<int>(texts_.size()) - 1; i >= 0; --i) {
        const TextItem& t = texts_[static_cast<size_t>(i)];
        if (p.x >= xFor(t.start) - 2 && p.x <= xFor(t.end) + 2) return i;
    }
    return -1;
}

int Timeline::rowAt(POINT p) const {
    for (int row = 0; row < rowCount(); ++row) {
        const int top = rowTop(row);
        if (p.y >= top && p.y < top + rowHeight(row)) return row;
    }
    return -1;
}

bool Timeline::onDown(POINT p, bool alt, bool* seeked, double* seekTo) {
    *seeked = false;
    if (!PtInRect(&bounds_, p) || duration_ <= 0.0) return false;

    // The ruler is for scrubbing and nothing else, so a drag there never has to
    // guess whether a segment was meant. It ends where the caption row starts:
    // before that line was drawn here, this test claimed the caption row too.
    if (p.y < textTop()) {
        drag_ = Drag::Head;
        *seeked = true;
        *seekTo = timeAt(p.x);
        return true;
    }

    if (inTextRow(p)) {
        const int which = textAt(p);
        selectedText_ = which;
        // A caption is not a track: picking one lets go of whatever row and
        // piece were picked, so the row's own window stops offering itself.
        selected_ = -1;
        selectedRow_ = -1;
        if (which >= 0) {
            const TextItem& t = texts_[static_cast<size_t>(which)];
            const int left = xFor(t.start), right = xFor(t.end);
            dragText_ = which;
            if (p.x <= left + 5) drag_ = Drag::TextStart;
            else if (p.x >= right - 5) drag_ = Drag::TextEnd;
            else { drag_ = Drag::TextMove; dragGrab_ = timeAt(p.x) - t.start; }
        }
        return true;
    }

    const int row = rowAt(p);
    if (row < 0) return true;

    // Fade handles first: they sit on top of a segment and are the smaller
    // target, so they have to win the pixel they share.
    const RowFades& f = fades_[static_cast<size_t>(row)];
    const int top = rowTop(row), height = rowHeight(row);
    const int lo = xFor(outLow()), hi = xFor(outHigh());
    const int inX = f.in > 0.01 ? xFor(outLow() + f.in) : lo + kFadeHandle;
    const int outX = f.out > 0.01 ? xFor(outHigh() - f.out) : hi - kFadeHandle;
    // The grip lies along the top edge, so that is where it answers.
    if (p.y >= top && p.y <= top + 10) {
        if (std::abs(p.x - inX) <= kFadeHandle + 2) {
            drag_ = Drag::FadeIn;
            dragRow_ = row;
            return true;
        }
        if (std::abs(p.x - outX) <= kFadeHandle + 2) {
            drag_ = Drag::FadeOut;
            dragRow_ = row;
            return true;
        }
    }

    const double at = timeAt(p.x);
    for (size_t i = 0; i < segments_.size(); ++i) {
        if (at < segments_[i].start || at > segments_[i].end) continue;
        selected_ = static_cast<int>(i);
        selectedRow_ = alt ? row : -1;
        break;
    }
    // Selecting and moving the playhead at once: in a strip this small, having
    // to aim at the ruler to move the picture is the thing that would annoy.
    drag_ = Drag::Head;
    *seeked = true;
    *seekTo = at;
    return true;
}

bool Timeline::onDrag(POINT p, bool* seeked, double* seekTo) {
    *seeked = false;
    if (drag_ == Drag::None) return false;

    if (drag_ == Drag::Head) {
        *seeked = true;
        *seekTo = timeAt(p.x);
        return true;
    }

    if (drag_ == Drag::TextMove || drag_ == Drag::TextStart || drag_ == Drag::TextEnd) {
        if (dragText_ < 0 || dragText_ >= static_cast<int>(texts_.size())) return true;
        TextItem& t = texts_[static_cast<size_t>(dragText_)];
        const double at = timeAt(p.x);
        if (drag_ == Drag::TextMove) {
            const double length = t.end - t.start;
            t.start = std::max(0.0, std::min(duration_ - length, at - dragGrab_));
            t.end = t.start + length;
        } else if (drag_ == Drag::TextStart) {
            t.start = std::max(0.0, std::min(t.end - 0.1, at));
        } else {
            t.end = std::min(duration_, std::max(t.start + 0.1, at));
        }
        return true;
    }

    if (dragRow_ < 0 || dragRow_ >= static_cast<int>(fades_.size())) return true;
    RowFades& f = fades_[static_cast<size_t>(dragRow_)];
    const double span = std::max(0.5, outHigh() - outLow());
    if (drag_ == Drag::FadeIn) {
        f.in = std::max(0.0, std::min(span / 2.0, timeAt(p.x) - outLow()));
    } else {
        f.out = std::max(0.0, std::min(span / 2.0, outHigh() - timeAt(p.x)));
    }
    return true;
}

void Timeline::onUp() {
    drag_ = Drag::None;
    dragRow_ = -1;
    dragText_ = -1;
}

bool Timeline::onDoubleClick(POINT p) {
    if (inTextRow(p)) {
        // Handled by the window: it is the one that can open a dialog. All this
        // has to do is make sure something is selected to open.
        const int which = textAt(p);
        selectedText_ = which >= 0 ? which : addText(timeAt(p.x));
        return true;
    }

    const int row = rowAt(p);
    if (row < 0 || p.y < trackStart()) return false;
    RowFades& f = fades_[static_cast<size_t>(row)];
    const int top = rowTop(row), height = rowHeight(row);

    // A fade handle first, and only when the pointer is near one in both
    // directions. Testing the height alone reserved the whole middle band of
    // every row for two handles at its ends, which is where a double click on
    // the body of a piece lands.
    const int inX = f.in > 0.01 ? xFor(outLow() + f.in) : xFor(outLow()) + kFadeHandle;
    const int outX = f.out > 0.01 ? xFor(outHigh() - f.out) : xFor(outHigh()) - kFadeHandle;
    if (p.y >= top && p.y <= top + 10) {
        if (std::abs(p.x - inX) <= kFadeHandle + 2) { f.in = 0.0; return true; }
        if (std::abs(p.x - outX) <= kFadeHandle + 2) { f.out = 0.0; return true; }
    }

    // Anywhere else on a piece means the piece itself, whose properties the
    // window then opens. Selecting it here is what tells the window which piece
    // and which row were meant.
    const int piece = segmentAt(timeAt(p.x));
    if (piece < 0) return false;
    selected_ = piece;
    selectedRow_ = row;
    selectedText_ = -1;
    return true;
}

}  // namespace vp
