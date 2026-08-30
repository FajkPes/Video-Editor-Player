#pragma once
// Colours, fonts and the four shapes everything in this window is built from.
//
// Kept apart from the window itself so the look can be changed in one place, and
// so the drawing code below stays about layout rather than about brushes.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <string>

namespace vp::theme {

// The palette of the version this replaces, value for value, so the two
// programs look like the same program. bg / panel / panel_hi / border /
// border_hi / text / muted / dim / accent / accent_dk / track.
const COLORREF kBack      = RGB(0x14, 0x16, 0x1b);   // bg
const COLORREF kPanel     = RGB(0x1c, 0x1f, 0x27);   // panel
const COLORREF kSection   = RGB(0x23, 0x27, 0x33);   // panel_hi
const COLORREF kRow       = RGB(0x23, 0x27, 0x33);   // a button at rest
const COLORREF kRowHot    = RGB(0x2f, 0x35, 0x43);   // a button under the mouse
const COLORREF kLine      = RGB(0x2f, 0x35, 0x43);   // border
const COLORREF kLineHot   = RGB(0x3d, 0x45, 0x57);   // border_hi
const COLORREF kTrack     = RGB(0x2a, 0x2f, 0x3b);   // a slider groove
const COLORREF kText      = RGB(0xe8, 0xea, 0xf0);
const COLORREF kTextDim   = RGB(0x8b, 0x93, 0xa7);   // muted
const COLORREF kTextFaint = RGB(0x5f, 0x67, 0x79);   // dim
const COLORREF kAccent    = RGB(0x4f, 0x8c, 0xff);
const COLORREF kAccentDk  = RGB(0x2f, 0x6f, 0xe0);
const COLORREF kAccentLit = RGB(0x6e, 0xa2, 0xff);
const COLORREF kAccentDim = RGB(0x2a, 0x3a, 0x5c);   // an accent that is not live
const COLORREF kGreen     = RGB(0x2e, 0xcc, 0x71);
const COLORREF kWarn      = RGB(0xf0, 0xa1, 0x2e);
const COLORREF kRed       = RGB(0xef, 0x4d, 0x4d);

struct Fonts {
    HFONT ui = nullptr;      // ordinary label
    // Not named `small`: windows.h defines that as a macro for char,
    // and a field by that name turns every use into a syntax error
    // several hundred lines from the cause.
    HFONT sub = nullptr;     // secondary text and values
    HFONT tiny = nullptr;    // band labels
    HFONT bold = nullptr;    // section and window titles
    HFONT semi = nullptr;    // a section heading inside the panel
    HFONT mono = nullptr;    // times, percentages, anything that must not jitter
    HFONT icon = nullptr;    // Segoe MDL2, for play and pause only
    HFONT osd = nullptr;

    void create();
    void destroy();
};

inline void fill(HDC dc, const RECT& r, COLORREF colour) {
    HBRUSH brush = CreateSolidBrush(colour);
    FillRect(dc, &r, brush);
    DeleteObject(brush);
}

inline void roundFill(HDC dc, const RECT& r, COLORREF colour, int radius) {
    HBRUSH brush = CreateSolidBrush(colour);
    HPEN pen = CreatePen(PS_NULL, 0, 0);
    HGDIOBJ ob = SelectObject(dc, brush);
    HGDIOBJ op = SelectObject(dc, pen);
    // The ellipse is twice the corner radius, so `radius` here means the same
    // thing as border-radius did in the stylesheet of the old version.
    RoundRect(dc, r.left, r.top, r.right + 1, r.bottom + 1, radius * 2, radius * 2);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(brush);
    DeleteObject(pen);
}

// A panel or a button: a fill and the one-pixel border every frame in the old
// version had. Without the border the darker shades run together and the
// interface reads as one flat sheet rather than as boxes.
// The border on its own, over whatever is already there.
inline void roundOutline(HDC dc, const RECT& r, COLORREF border, int radius) {
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    HGDIOBJ op = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius * 2, radius * 2);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pen);
}

inline void roundBox(HDC dc, const RECT& r, COLORREF inside, COLORREF border,
                     int radius) {
    HBRUSH brush = CreateSolidBrush(inside);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, brush);
    HGDIOBJ op = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius * 2, radius * 2);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(brush);
    DeleteObject(pen);
}

// The tick inside a checkbox, drawn rather than set in a font: Segoe UI's
// U+2713 sits low in the box and is thinner than the one the old version used.
inline void checkMark(HDC dc, const RECT& box, COLORREF colour) {
    const int w = box.right - box.left;
    HPEN pen = CreatePen(PS_SOLID, std::max(2, w / 7), colour);
    HGDIOBJ op = SelectObject(dc, pen);
    const POINT pts[3] = {{box.left + w * 22 / 100, box.top + w * 52 / 100},
                          {box.left + w * 42 / 100, box.top + w * 72 / 100},
                          {box.left + w * 78 / 100, box.top + w * 28 / 100}};
    Polyline(dc, pts, 3);
    SelectObject(dc, op);
    DeleteObject(pen);
}

// A checkbox exactly as the old version drew it: a rounded square that fills
// with the accent when it is on.
inline void checkBox(HDC dc, const RECT& box, bool on, bool hot) {
    roundBox(dc, box, on ? kAccent : kSection, on ? kAccent : (hot ? kAccent : kLineHot),
             4);
    if (on) checkMark(dc, box, RGB(255, 255, 255));
}

inline void text(HDC dc, RECT r, const std::wstring& s, COLORREF colour, UINT flags,
                 HFONT font) {
    HGDIOBJ old = SelectObject(dc, font);
    SetTextColor(dc, colour);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s.c_str(), -1, &r, flags);
    SelectObject(dc, old);
}

// A groove with a filled part and a knob. The seek bar, every volume and every
// equaliser band are all this, which is why it takes a "centred" flag: a band
// fills outwards from the middle because zero is its resting place, while a
// volume fills from the left because silence is.
inline void slider(HDC dc, const RECT& r, double fraction, bool live, bool centred) {
    const int mid = (r.top + r.bottom) / 2;
    RECT groove{r.left, mid - 2, r.right, mid + 2};
    roundFill(dc, groove, kTrack, 2);

    fraction = std::max(0.0, std::min(1.0, fraction));
    const int at = r.left + static_cast<int>((r.right - r.left) * fraction);
    const int from = centred ? (r.left + r.right) / 2 : r.left;
    RECT done{std::min(from, at), mid - 2, std::max(from, at), mid + 2};
    if (done.right > done.left) roundFill(dc, done, live ? kAccent : kAccentDim, 2);

    // 13 px across, the width the old version's handle was.
    RECT knob{at - 6, mid - 6, at + 7, mid + 7};
    roundFill(dc, knob, live ? kText : kTextDim, 7);
}

}  // namespace vp::theme
