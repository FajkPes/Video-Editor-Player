// The clip and caption property sheets.
//
// One small modal framework at the top - a list of rows, each a label and one
// control - and then three windows built out of it. The framework exists
// because the alternative was three copies of the same message loop, and
// because a property sheet is genuinely just a list of rows.

#include "clip_window.h"

#include <windowsx.h>
#include <commdlg.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

#include "lang.h"
#include "settings.h"
#include "theme.h"

#pragma comment(lib, "comdlg32.lib")

using namespace vp::theme;

namespace vp {
namespace {

struct Hit {
    RECT r{};
    bool has(POINT p) const { return PtInRect(&r, p) != 0; }
};

// A row is a label on the left and one control on the right. Which control it
// is decides how it draws and what a click on it does.
enum class Kind { Slider, Toggle, Chip, Button, Grid, Gap, Heading };

struct Row {
    Kind kind = Kind::Slider;
    std::wstring label;
    std::wstring note;

    // Slider: the value lives behind these, in whatever unit the caller likes.
    std::function<double()> get;
    std::function<void(double)> set;
    double lo = 0.0, hi = 1.0, step = 0.0;
    int decimals = 0;
    std::wstring suffix;
    bool centred = false;

    // Toggle, Chip and Button.
    std::function<bool()> on;
    std::function<void()> press;
    std::function<std::wstring()> caption;
    COLORREF swatch = 0;
    bool isSwatch = false;

    Hit hit;
    Hit extra;          // the second target on a grid row, or a chip's reset
};

constexpr int kRowHeight = 30;
constexpr int kLabelWidth = 168;

class Sheet {
public:
    Sheet(const std::wstring& title, const std::wstring& subtitle, int width)
        : title_(title), subtitle_(subtitle), width_(width) {}

    std::vector<Row>& rows() { return rows_; }
    void setExtraButton(const std::wstring& label) { extraLabel_ = label; }
    bool extraPressed() const { return extraPressed_; }
    // A sheet whose edits are live has nothing to save or to cancel, so it says
    // something else on its buttons.
    void setButtons(const std::wstring& ok, const std::wstring& back) {
        okLabel_ = ok;
        backLabel_ = back;
    }

    // A child edit box, for the one thing that cannot sensibly be hand drawn.
    void wantTextBox(std::wstring* target) { textTarget_ = target; }

    bool run(HWND owner, HINSTANCE instance);

private:
    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
    void layout();
    void paint(HDC dc);
    void onDown(POINT p);
    void onMove(POINT p);
    double fractionOf(const Row& row, POINT p) const;

    std::wstring title_, subtitle_, extraLabel_;
    int width_;
    std::vector<Row> rows_;

    HWND window_ = nullptr, edit_ = nullptr;
    std::wstring* textTarget_ = nullptr;
    Fonts fonts_;
    POINT mouse_{};
    int dragRow_ = -1;
    bool saved_ = false, extraPressed_ = false, closed_ = false;

    Hit save_, cancel_, extra_;
    int bottom_ = 0, textTop_ = 0, textHeight_ = 0;
    std::wstring okLabel_, backLabel_;

    // Taller than the screen is normal once a film's every track is listed, so
    // the rows scroll under a bar of buttons that stays put.
    int scroll_ = 0;        // how far down the rows have been pushed
    int content_ = 0;       // how tall they are altogether
    int viewHeight_ = 0;    // how much of that fits, 0 before the window exists
    bool draggingBar_ = false;
    int barGrab_ = 0;       // where in the thumb it was taken hold of
    Hit bar_;               // the groove, so it can be hit like anything else
    void scrollBy(int by);
    void scrollTo(int top);
};

// ------------------------------------------------------------------ layout

void Sheet::layout() {
    const int left = 24;
    const int right = width_ - 24;

    int y = (subtitle_.empty() ? 72 : 92) - scroll_;
    if (textTarget_) {
        textTop_ = y;
        textHeight_ = 84;
        y += textHeight_ + 14;
    }

    for (Row& row : rows_) {
        if (row.kind == Kind::Gap) { y += 14; continue; }
        if (row.kind == Kind::Heading) {
            row.hit.r = {left, y, right, y + 24};
            y += 30;
            continue;
        }
        if (row.kind == Kind::Grid) {
            // Nine cells, three across, in the space one row would take three
            // times over.
            row.hit.r = {left + kLabelWidth, y, left + kLabelWidth + 96, y + 96};
            y += 104;
            continue;
        }
        if (row.kind == Kind::Slider) {
            row.hit.r = {left + kLabelWidth, y + 7, right - 76, y + 23};
        } else if (row.kind == Kind::Toggle) {
            row.hit.r = {left + kLabelWidth, y + 4, right, y + 26};
        } else {
            row.hit.r = {left + kLabelWidth, y + 2, left + kLabelWidth + 200, y + 28};
        }
        y += kRowHeight;
    }

    content_ = y + scroll_ + 18 + 32 + 24;
    bottom_ = content_;

    // The buttons sit on the floor of the window rather than under the last
    // row, so they are reachable however far the rows have been scrolled.
    const int bar = (viewHeight_ > 0 ? viewHeight_ : content_) - 56;
    cancel_.r = {right - 104, bar, right, bar + 32};
    save_.r = {right - 216, bar, right - 112, bar + 32};
    extra_.r = {left, bar, left + 150, bar + 32};
}

void Sheet::scrollBy(int by) { scrollTo(scroll_ + by); }

void Sheet::scrollTo(int top) {
    const int most = std::max(0, content_ - std::max(1, viewHeight_));
    const int want = std::max(0, std::min(most, top));
    if (want == scroll_) return;
    scroll_ = want;
    InvalidateRect(window_, nullptr, FALSE);
}

// ----------------------------------------------------------------- drawing

void Sheet::paint(HDC dc) {
    RECT client{};
    GetClientRect(window_, &client);
    fill(dc, client, kBack);

    const int left = 24;
    const int right = width_ - 24;

    text(dc, {left, 20, right, 50}, title_, kText,
         DT_LEFT | DT_SINGLELINE | DT_VCENTER, fonts_.bold);
    if (!subtitle_.empty()) {
        text(dc, {left, 50, right, 72}, subtitle_, kTextDim,
             DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS, fonts_.sub);
    }

    for (const Row& row : rows_) {
        if (row.kind == Kind::Gap) continue;
        if (row.kind == Kind::Heading) {
            RECT rule{left, row.hit.r.top + 2, right, row.hit.r.top + 3};
            fill(dc, rule, kLine);
            text(dc, {left, row.hit.r.top + 6, right, row.hit.r.bottom + 6}, row.label,
                 kText, DT_LEFT | DT_SINGLELINE | DT_VCENTER, fonts_.semi);
            continue;
        }

        const RECT& r = row.hit.r;
        text(dc, {left, r.top - 4, left + kLabelWidth - 12, r.bottom + 4}, row.label,
             kTextDim, DT_LEFT | DT_SINGLELINE | DT_VCENTER, fonts_.ui);

        switch (row.kind) {
            case Kind::Slider: {
                const double value = row.get();
                const double span = std::max(1e-9, row.hi - row.lo);
                slider(dc, r, (value - row.lo) / span,
                       std::fabs(value) > 1e-6 || !row.centred, row.centred);
                wchar_t shown[48];
                _snwprintf_s(shown, _TRUNCATE, L"%.*f%s", row.decimals, value,
                             row.suffix.c_str());
                text(dc, {r.right + 8, r.top - 4, right, r.bottom + 4}, shown, kTextDim,
                     DT_RIGHT | DT_SINGLELINE | DT_VCENTER, fonts_.mono);
                break;
            }
            case Kind::Toggle: {
                RECT box{r.left, (r.top + r.bottom) / 2 - 8, r.left + 16,
                         (r.top + r.bottom) / 2 + 8};
                checkBox(dc, box, row.on(), row.hit.has(mouse_));
                // A switch that can describe its own state says that instead of
                // a fixed line, which otherwise describes the state it is not
                // in and reads as a contradiction.
                const std::wstring said = row.caption ? row.caption() : row.note;
                text(dc, {box.right + 10, r.top, right, r.bottom}, said,
                     row.on() ? kText : kTextDim,
                     DT_LEFT | DT_SINGLELINE | DT_VCENTER, fonts_.ui);
                break;
            }
            case Kind::Chip: {
                roundBox(dc, r, row.hit.has(mouse_) ? kRowHot : kSection,
                         row.hit.has(mouse_) ? kLineHot : kLine, 6);
                if (row.isSwatch) {
                    // A colour is shown rather than named: nobody reads a colour
                    // out of six hex digits.
                    RECT patch{r.left + 6, r.top + 5, r.left + 40, r.bottom - 5};
                    roundBox(dc, patch, row.swatch, kLineHot, 4);
                    text(dc, {patch.right + 10, r.top, r.right - 6, r.bottom},
                         row.caption(), kText,
                         DT_LEFT | DT_SINGLELINE | DT_VCENTER, fonts_.ui);
                } else {
                    text(dc, r, row.caption(), kText,
                         DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS,
                         fonts_.ui);
                }
                if (!row.note.empty()) {
                    text(dc, {r.right + 12, r.top, right, r.bottom}, row.note, kTextFaint,
                         DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS, fonts_.tiny);
                }
                break;
            }
            case Kind::Button: {
                roundBox(dc, r, row.hit.has(mouse_) ? kRowHot : kSection,
                         row.hit.has(mouse_) ? kLineHot : kLine, 6);
                text(dc, r, row.caption(), kText,
                     DT_CENTER | DT_SINGLELINE | DT_VCENTER, fonts_.ui);
                break;
            }
            case Kind::Grid: {
                // The nine places a caption can sit, laid out the way they sit
                // on the frame, so choosing one is pointing at it.
                const int chosen = static_cast<int>(row.get());
                for (int cell = 1; cell <= 9; ++cell) {
                    const int column = (cell - 1) % 3;
                    const int band = (cell - 1) / 3;          // 0 bottom, 2 top
                    RECT box{r.left + column * 32, r.bottom - (band + 1) * 32,
                             r.left + column * 32 + 28, r.bottom - band * 32 - 4};
                    const bool live = cell == chosen;
                    const bool hot = PtInRect(&box, mouse_) != 0;
                    roundBox(dc, box, live ? kAccent : (hot ? kRowHot : kSection),
                             live ? kAccentLit : (hot ? kLineHot : kLine), 4);
                    RECT dot{(box.left + box.right) / 2 - 3,
                             (box.top + box.bottom) / 2 - 2,
                             (box.left + box.right) / 2 + 3,
                             (box.top + box.bottom) / 2 + 2};
                    fill(dot.left < dot.right ? dc : dc, dot,
                         live ? RGB(255, 255, 255) : kTextDim);
                }
                text(dc, {r.right + 16, r.top, right, r.bottom}, row.note, kTextFaint,
                     DT_LEFT | DT_WORDBREAK, fonts_.tiny);
                break;
            }
            default: break;
        }
    }

    auto button = [&](const Hit& h, const std::wstring& label, bool primary) {
        roundBox(dc, h.r,
                 primary ? (h.has(mouse_) ? kAccentDk : kAccent)
                         : (h.has(mouse_) ? kRowHot : kSection),
                 primary ? kAccentDk : (h.has(mouse_) ? kLineHot : kLine), 6);
        text(dc, h.r, label, primary ? RGB(255, 255, 255) : kText,
             DT_CENTER | DT_SINGLELINE | DT_VCENTER, primary ? fonts_.semi : fonts_.ui);
    };
    RECT bar{0, save_.r.top - 14, client.right, client.bottom};
    fill(dc, bar, kBack);
    RECT rule{0, bar.top, client.right, bar.top + 1};
    fill(dc, rule, kLine);
    if (!extraLabel_.empty()) button(extra_, extraLabel_, false);
    button(save_, okLabel_.empty() ? tr(L"Save") : okLabel_, true);
    button(cancel_, backLabel_.empty() ? tr(L"Cancel") : backLabel_, false);

    // A hint of how far down the rows we are, when there are more than fit.
    const int most = std::max(0, content_ - std::max(1, viewHeight_));
    if (most > 0) {
        // Wide enough to be aimed at, and it remembers where it was grabbed.
        const int track = bar.top - 70;
        const int knob = std::max(36, track * std::max(1, viewHeight_) / content_);
        const int at = 70 + (track - knob) * scroll_ / std::max(1, most);
        bar_.r = {client.right - 16, 70, client.right - 4, bar.top};
        roundFill(dc, bar_.r, kSection, 5);
        RECT thumb{bar_.r.left + 1, at, bar_.r.right - 1, at + knob};
        roundFill(dc, thumb, draggingBar_ || bar_.has(mouse_) ? kAccent : kLineHot, 5);
    } else {
        bar_.r = {0, 0, 0, 0};
    }
}

// -------------------------------------------------------------- interaction

double Sheet::fractionOf(const Row& row, POINT p) const {
    // The knob has a width of its own, so the last few pixels at either end
    // were unreachable and the far left of a slider stopped at -99 instead of
    // -100. The travel is measured between the knob's own two end positions.
    const double edge = 7.0;
    const double span = static_cast<double>(row.hit.r.right - row.hit.r.left) - edge * 2.0;
    if (span <= 0.0) return 0.0;
    return std::max(0.0, std::min(1.0, (p.x - row.hit.r.left - edge) / span));
}

void Sheet::onDown(POINT p) {
    mouse_ = p;

    // The bar first: it lies over the right-hand edge of the rows.
    if (bar_.r.right > bar_.r.left && bar_.has(p)) {
        const int most = std::max(0, content_ - std::max(1, viewHeight_));
        const int track = bar_.r.bottom - bar_.r.top;
        const int knob = std::max(36, track * std::max(1, viewHeight_) / std::max(1, content_));
        const int at = bar_.r.top + (track - knob) * scroll_ / std::max(1, most);
        draggingBar_ = true;
        // Taken by the thumb it keeps its grip; taken by the groove it jumps
        // so the thumb lands under the pointer and then follows it.
        barGrab_ = (p.y >= at && p.y <= at + knob) ? p.y - at : knob / 2;
        scrollTo((p.y - bar_.r.top - barGrab_) * most /
                 std::max(1, track - knob));
        return;
    }
    if (save_.has(p)) {
        if (edit_ && textTarget_) {
            const int length = GetWindowTextLengthW(edit_);
            std::wstring held(static_cast<size_t>(length) + 1, L'\0');
            GetWindowTextW(edit_, &held[0], length + 1);
            held.resize(static_cast<size_t>(length));
            *textTarget_ = held;
        }
        saved_ = true;
        DestroyWindow(window_);
        return;
    }
    if (cancel_.has(p)) { DestroyWindow(window_); return; }
    if (!extraLabel_.empty() && extra_.has(p)) {
        extraPressed_ = true;
        saved_ = true;
        DestroyWindow(window_);
        return;
    }

    for (size_t i = 0; i < rows_.size(); ++i) {
        Row& row = rows_[i];
        if (row.kind == Kind::Gap || row.kind == Kind::Heading) continue;

        if (row.kind == Kind::Grid) {
            for (int cell = 1; cell <= 9; ++cell) {
                const int column = (cell - 1) % 3;
                const int band = (cell - 1) / 3;
                RECT box{row.hit.r.left + column * 32, row.hit.r.bottom - (band + 1) * 32,
                         row.hit.r.left + column * 32 + 28,
                         row.hit.r.bottom - band * 32 - 4};
                if (PtInRect(&box, p)) {
                    row.set(cell);
                    InvalidateRect(window_, nullptr, FALSE);
                    return;
                }
            }
            continue;
        }
        if (!row.hit.has(p)) continue;

        if (row.kind == Kind::Slider) {
            dragRow_ = static_cast<int>(i);
            double value = row.lo + fractionOf(row, p) * (row.hi - row.lo);
            if (row.step > 0.0) value = std::floor(value / row.step + 0.5) * row.step;
            row.set(value);
        } else if (row.kind == Kind::Toggle) {
            row.press();
        } else {
            row.press();
        }
        InvalidateRect(window_, nullptr, FALSE);
        return;
    }
}

void Sheet::onMove(POINT p) {
    if (draggingBar_) {
        mouse_ = p;
        const int most = std::max(0, content_ - std::max(1, viewHeight_));
        const int track = bar_.r.bottom - bar_.r.top;
        const int knob = std::max(36, track * std::max(1, viewHeight_) / std::max(1, content_));
        scrollTo((p.y - bar_.r.top - barGrab_) * most / std::max(1, track - knob));
        return;
    }
    mouse_ = p;
    if (dragRow_ >= 0 && dragRow_ < static_cast<int>(rows_.size())) {
        Row& row = rows_[static_cast<size_t>(dragRow_)];
        double value = row.lo + fractionOf(row, p) * (row.hi - row.lo);
        if (row.step > 0.0) value = std::floor(value / row.step + 0.5) * row.step;
        row.set(value);
    }
    InvalidateRect(window_, nullptr, FALSE);
}

// ----------------------------------------------------------------- plumbing

LRESULT CALLBACK Sheet::proc(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    auto* self = reinterpret_cast<Sheet*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message) {
        case WM_NCCREATE: {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(l);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            break;
        }
        case WM_CTLCOLOREDIT: {
            // The one child control here has to be talked into the dark theme;
            // it is a real edit box, because hand-drawing text entry is a
            // fortnight of work to get worse than the one Windows ships.
            auto dc = reinterpret_cast<HDC>(w);
            SetTextColor(dc, kText);
            SetBkColor(dc, kSection);
            static HBRUSH back = CreateSolidBrush(kSection);
            return reinterpret_cast<LRESULT>(back);
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
            if (self) {
                SetCapture(hwnd);
                self->onDown({GET_X_LPARAM(l), GET_Y_LPARAM(l)});
            }
            return 0;
        case WM_MOUSEMOVE:
            if (self) self->onMove({GET_X_LPARAM(l), GET_Y_LPARAM(l)});
            return 0;
        case WM_MOUSEWHEEL:
            if (self) self->scrollBy(-GET_WHEEL_DELTA_WPARAM(w) / 120 * 60);
            return 0;
        case WM_LBUTTONUP:
            if (self) {
                self->dragRow_ = -1;
                self->draggingBar_ = false;
                ReleaseCapture();
            }
            return 0;
        case WM_KEYDOWN:
            if (w == VK_ESCAPE) DestroyWindow(hwnd);
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            // A flag rather than a quit message: WM_QUIT belongs to the whole
            // thread, and one left over from a sheet that has already closed
            // would be taken by whichever loop asked for a message next - the
            // program's own, which would then shut down.
            if (self) self->closed_ = true;
            return 0;
        default: break;
    }
    return DefWindowProcW(hwnd, message, w, l);
}

bool Sheet::run(HWND owner, HINSTANCE instance) {
    fonts_.create();
    layout();

    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.style = CS_HREDRAW | CS_VREDRAW;
    cls.lpfnWndProc = &Sheet::proc;
    cls.hInstance = instance;
    cls.hCursor = LoadCursor(nullptr, IDC_ARROW);
    cls.hbrBackground = CreateSolidBrush(kBack);
    cls.lpszClassName = L"VideoPlayerCppSheet";
    RegisterClassExW(&cls);

    // Never taller than the screen it has to appear on.
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int room = std::max(320, static_cast<int>((work.bottom - work.top) * 0.88));
    viewHeight_ = std::min(bottom_, room);
    layout();

    RECT frame{0, 0, width_, viewHeight_};
    AdjustWindowRect(&frame, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
    RECT ownerRect{};
    GetWindowRect(owner, &ownerRect);
    const int width = frame.right - frame.left;
    const int height = frame.bottom - frame.top;

    // Placed over the owner, then pushed back onto the screen: a window that
    // hangs off the bottom is a window whose buttons cannot be reached.
    LONG left = (ownerRect.left + ownerRect.right - width) / 2;
    LONG top = (ownerRect.top + ownerRect.bottom - height) / 2;
    left = std::max(work.left, std::min(work.right - width, left));
    top = std::max(work.top, std::min(work.bottom - height, top));
    window_ = CreateWindowExW(WS_EX_DLGMODALFRAME, cls.lpszClassName, title_.c_str(),
                              WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                              left, top, width, height, owner, nullptr, instance, this);
    if (!window_) { fonts_.destroy(); return false; }

    if (textTarget_) {
        edit_ = CreateWindowExW(0, L"EDIT", textTarget_->c_str(),
                                WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE |
                                    ES_AUTOVSCROLL | ES_WANTRETURN,
                                24, textTop_, width_ - 48, textHeight_, window_,
                                nullptr, instance, nullptr);
        if (edit_) {
            SendMessageW(edit_, WM_SETFONT,
                         reinterpret_cast<WPARAM>(fonts_.ui), TRUE);
            SetFocus(edit_);
        }
    }

    EnableWindow(owner, FALSE);
    ShowWindow(window_, SW_SHOW);
    // Without this the wheel keeps going to whatever had the keyboard before -
    // the player behind - and the rows here never move.
    if (!edit_) SetFocus(window_);

    MSG message;
    while (!closed_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window_, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    fonts_.destroy();
    return saved_;
}

// ------------------------------------------------------------------ helpers

COLORREF askColour(HWND owner, COLORREF start) {
    static COLORREF custom[16] = {};
    CHOOSECOLORW choose{};
    choose.lStructSize = sizeof(choose);
    choose.hwndOwner = owner;
    choose.rgbResult = start;
    choose.lpCustColors = custom;
    choose.Flags = CC_FULLOPEN | CC_RGBINIT;
    return ChooseColorW(&choose) ? choose.rgbResult : start;
}

std::wstring colourName(COLORREF c) {
    wchar_t buf[24];
    _snwprintf_s(buf, _TRUNCATE, L"#%02X%02X%02X", GetRValue(c), GetGValue(c),
                 GetBValue(c));
    return buf;
}

std::wstring seconds(double v) {
    wchar_t buf[32];
    _snwprintf_s(buf, _TRUNCATE, L"%d:%05.2f", static_cast<int>(v) / 60,
                 v - 60.0 * (static_cast<int>(v) / 60));
    return buf;
}

Row heading(const std::wstring& label) {
    Row row;
    row.kind = Kind::Heading;
    row.label = label;
    return row;
}

Row gap() {
    Row row;
    row.kind = Kind::Gap;
    return row;
}

Row slide(const std::wstring& label, std::function<double()> get,
          std::function<void(double)> set, double lo, double hi, double step,
          int decimals, const std::wstring& suffix, bool centred) {
    Row row;
    row.kind = Kind::Slider;
    row.label = label;
    row.get = std::move(get);
    row.set = std::move(set);
    row.lo = lo;
    row.hi = hi;
    row.step = step;
    row.decimals = decimals;
    row.suffix = suffix;
    row.centred = centred;
    return row;
}

Row toggle(const std::wstring& label, const std::wstring& note,
           std::function<bool()> on, std::function<void()> press) {
    Row row;
    row.kind = Kind::Toggle;
    row.label = label;
    row.note = note;
    row.on = std::move(on);
    row.press = std::move(press);
    return row;
}

}  // namespace

// =========================================================== clip: the sound

bool runClipAudioWindow(HWND owner, HINSTANCE instance, ClipAudio& clip,
                        const std::wstring& trackName, const std::wstring& when,
                        std::function<void(const ClipAudio&)> preview) {
    ClipAudio work = clip;
    // Every setter goes through this, so what is being set is heard while
    // the window is still open rather than only once it is saved.
    auto mark = [&work, &preview] {
        work.set = true;
        if (preview) preview(work);
    };

    Sheet sheet(tr(L"Clip \x2014 sound"), trackName + L"   \x00b7   " + when, 560);
    sheet.setExtraButton(tr(L"Clear this clip"));

    sheet.rows().push_back(toggle(
        tr(L"Silent here"), tr(L"the track is muted over this piece only"),
        [&work] { return work.silent; },
        [&work, mark] { work.silent = !work.silent; mark(); }));
    sheet.rows().push_back(gap());
    sheet.rows().push_back(heading(tr(L"On top of the mixer")));
    sheet.rows().push_back(slide(
        tr(L"Volume"), [&work] { return work.gain * 100.0; },
        [&work, mark](double v) { work.gain = static_cast<float>(v / 100.0); mark(); },
        0.0, 200.0, 1.0, 0, L" %", false));
    sheet.rows().push_back(slide(
        tr(L"Bass"), [&work] { return work.eqLow; },
        [&work, mark](double v) { work.eqLow = static_cast<float>(v); mark(); },
        -12.0, 12.0, 0.5, 1, L" dB", true));
    sheet.rows().push_back(slide(
        tr(L"Mids"), [&work] { return work.eqMid; },
        [&work, mark](double v) { work.eqMid = static_cast<float>(v); mark(); },
        -12.0, 12.0, 0.5, 1, L" dB", true));
    sheet.rows().push_back(slide(
        tr(L"Treble"), [&work] { return work.eqHigh; },
        [&work, mark](double v) { work.eqHigh = static_cast<float>(v); mark(); },
        -12.0, 12.0, 0.5, 1, L" dB", true));
    sheet.rows().push_back(slide(
        tr(L"Pitch"), [&work] { return work.pitch; },
        [&work, mark](double v) { work.pitch = static_cast<float>(v); mark(); },
        -12.0, 12.0, 1.0, 0, tr(L" st"), true));

    if (!sheet.run(owner, instance)) return false;
    if (sheet.extraPressed()) {
        clip = ClipAudio{};
        return true;
    }
    // A clip that has been given a setting is marked as having one, so the
    // mixer knows to look at it. Silence alone counts.
    work.set = work.set || work.silent;
    clip = work;
    return true;
}

// ========================================================= clip: the picture

bool runClipVideoWindow(HWND owner, HINSTANCE instance, ClipVideo& clip,
                        const std::wstring& when,
                        std::function<void(const ClipVideo&)> preview) {
    ClipVideo work = clip;

    Sheet sheet(tr(L"Clip \x2014 picture"), when, 600);
    sheet.setExtraButton(tr(L"Clear this clip"));

    auto mark = [&work, &preview] {
        work.set = true;
        if (preview) preview(work);
    };

    sheet.rows().push_back(slide(
        tr(L"Visibility"), [&work] { return work.opacity * 100.0; },
        [&work, mark](double v) { work.opacity = static_cast<float>(v / 100.0); mark(); },
        0.0, 100.0, 1.0, 0, L" %", false));
    sheet.rows().push_back(gap());
    sheet.rows().push_back(heading(tr(L"On top of the picture panel")));

    struct Band { const wchar_t* label; int ClipVideo::* field; int lo, hi; };
    const Band bands[] = {
        {L"Contrast", &ClipVideo::contrast, -100, 100},
        {L"Brightness", &ClipVideo::brightness, -100, 100},
        {L"Saturation", &ClipVideo::saturation, -100, 100},
        {L"Gamma", &ClipVideo::gamma, -100, 100},
        {L"Hue", &ClipVideo::hue, -100, 100},
        {L"Sharpness", &ClipVideo::sharpen, 0, 200},
        {L"Denoise", &ClipVideo::denoise, 0, 100},
    };
    for (const Band& band : bands) {
        int ClipVideo::* field = band.field;
        sheet.rows().push_back(slide(
            tr(band.label), [&work, field] { return static_cast<double>(work.*field); },
            [&work, field, mark](double v) { work.*field = static_cast<int>(v + 0.5); mark(); },
            band.lo, band.hi, 1.0, 0, L"", band.lo < 0));
    }

    // Four edges rather than a corner and a size: trimming a frame is thought
    // of as "take this much off each side", and the width and the height then
    // follow from the four of them.
    sheet.rows().push_back(slide(
        tr(L"Crop left"), [&work] { return work.cropX * 100.0; },
        [&work, mark](double v) {
            const double right = work.cropX + work.cropW;
            work.cropX = std::min(v / 100.0, right - 0.05);
            work.cropW = right - work.cropX;
            mark();
        },
        0.0, 95.0, 0.5, 1, L" %", false));
    sheet.rows().push_back(slide(
        tr(L"Crop right"),
        [&work] { return (1.0 - work.cropX - work.cropW) * 100.0; },
        [&work, mark](double v) {
            const double right = std::max(work.cropX + 0.05, 1.0 - v / 100.0);
            work.cropW = right - work.cropX;
            mark();
        },
        0.0, 95.0, 0.5, 1, L" %", false));
    sheet.rows().push_back(slide(
        tr(L"Crop top"), [&work] { return work.cropY * 100.0; },
        [&work, mark](double v) {
            const double bottom = work.cropY + work.cropH;
            work.cropY = std::min(v / 100.0, bottom - 0.05);
            work.cropH = bottom - work.cropY;
            mark();
        },
        0.0, 95.0, 0.5, 1, L" %", false));
    sheet.rows().push_back(slide(
        tr(L"Crop bottom"),
        [&work] { return (1.0 - work.cropY - work.cropH) * 100.0; },
        [&work, mark](double v) {
            const double bottom = std::max(work.cropY + 0.05, 1.0 - v / 100.0);
            work.cropH = bottom - work.cropY;
            mark();
        },
        0.0, 95.0, 0.5, 1, L" %", false));

    sheet.rows().push_back(gap());
    sheet.rows().push_back(heading(tr(L"This stretch only")));
    sheet.rows().push_back(slide(
        tr(L"Speed"), [&work] { return work.speed * 100.0; },
        [&work, mark](double v) { work.speed = v / 100.0; mark(); },
        25.0, 400.0, 5.0, 0, L" %", false));
    sheet.rows().push_back(gap());
    sheet.rows().push_back(heading(tr(L"Crop and pan zone")));

    // The zoom closes in on the middle of whatever is left after the crop, and
    // the position then slides that window across the frame.
    sheet.rows().push_back(slide(
        tr(L"Zoom"), [&work] { return work.zoom * 100.0; },
        [&work, mark](double v) { work.zoom = v / 100.0; mark(); },
        100.0, 400.0, 1.0, 0, L" %", false));
    sheet.rows().push_back(slide(
        tr(L"Position across"), [&work] { return work.panX * 100.0; },
        [&work, mark](double v) { work.panX = v / 100.0; mark(); },
        -50.0, 50.0, 0.5, 1, L" %", true));
    sheet.rows().push_back(slide(
        tr(L"Position down"), [&work] { return work.panY * 100.0; },
        [&work, mark](double v) { work.panY = v / 100.0; mark(); },
        -50.0, 50.0, 0.5, 1, L" %", true));

    if (!sheet.run(owner, instance)) return false;
    if (sheet.extraPressed()) {
        clip = ClipVideo{};
        return true;
    }
    clip = work;
    return true;
}

// =============================================================== the caption

// ---------------------------------------------------------------------------
// Everything at once: the picture, the levelling and every track. The rows read
// and write straight through to whatever the caller pointed them at, so moving
// a slider here moves the same slider on the panel and is heard immediately.

void runPlayerWindow(HWND owner, HINSTANCE instance, PlayerSheet& sheet) {
    Sheet window(tr(L"Clip properties"), sheet.subtitle, 660);
    window.setButtons(tr(L"Done"), tr(L"Close"));

    auto heading = [](const std::wstring& label) {
        Row r;
        r.kind = Kind::Heading;
        r.label = label;
        return r;
    };
    auto gap = [] {
        Row r;
        r.kind = Kind::Gap;
        return r;
    };
    auto slide = [](const std::wstring& label, std::function<double()> get,
                    std::function<void(double)> set, double lo, double hi,
                    double step, int decimals, const wchar_t* suffix,
                    bool centred) {
        Row r;
        r.kind = Kind::Slider;
        r.label = label;
        r.get = std::move(get);
        r.set = std::move(set);
        r.lo = lo; r.hi = hi; r.step = step;
        r.decimals = decimals;
        r.suffix = suffix;
        r.centred = centred;
        return r;
    };
    auto toggle = [](const std::wstring& label, const std::wstring& note,
                     std::function<bool()> on, std::function<void()> press) {
        Row r;
        r.kind = Kind::Toggle;
        r.label = label;
        r.note = note;
        r.on = std::move(on);
        r.press = std::move(press);
        return r;
    };
    auto button = [](const std::wstring& label, const std::wstring& caption,
                     std::function<void()> press) {
        Row r;
        r.kind = Kind::Button;
        r.label = label;
        r.caption = [caption] { return caption; };
        r.press = std::move(press);
        return r;
    };

    window.rows().push_back(heading(tr(L"Where these apply")));
    {
        Row r;
        r.kind = Kind::Toggle;
        r.label = tr(L"From the player");
        r.on = [&sheet] { return sheet.fromPlayer(); };
        r.press = [&sheet] { sheet.setFromPlayer(!sheet.fromPlayer()); };
        r.caption = [&sheet]() -> std::wstring {
            return sheet.fromPlayer()
                       ? tr(L"what is set here plays and is exported")
                       : tr(L"the player keeps playing as it is; these are the "
                            L"editor's and the export's own");
        };
        window.rows().push_back(std::move(r));
    }

    window.rows().push_back(gap());
    window.rows().push_back(heading(tr(L"Picture")));
    struct Band { const wchar_t* label; int index; int lo, hi; };
    static const Band kBands[] = {
        {L"Contrast", 0, -100, 100}, {L"Brightness", 1, -100, 100},
        {L"Saturation", 2, -100, 100}, {L"Gamma", 3, -100, 100},
        {L"Hue", 4, -100, 100}, {L"Sharpness", 5, 0, 200},
        {L"Denoise", 6, 0, 100},
    };
    for (const Band& band : kBands) {
        const int which = band.index;
        window.rows().push_back(slide(
            tr(band.label), [&sheet, which] { return sheet.band(which); },
            [&sheet, which](double v) { sheet.setBand(which, v); },
            band.lo, band.hi, 1.0, 0, L"", band.lo < 0));
    }
    if (sheet.resetPicture) {
        window.rows().push_back(button(tr(L"Picture"), tr(L"Reset"),
                                       [&sheet] { sheet.resetPicture(); }));
    }

    if (sheet.view) {
        window.rows().push_back(gap());
        window.rows().push_back(heading(tr(L"Framing")));
        window.rows().push_back(slide(
            tr(L"Zoom"), [&sheet] { return sheet.view(0) * 100.0; },
            [&sheet](double v) { sheet.setView(0, v / 100.0); },
            25.0, 400.0, 1.0, 0, L" %", false));
        window.rows().push_back(slide(
            tr(L"Position across"), [&sheet] { return sheet.view(1) * 100.0; },
            [&sheet](double v) { sheet.setView(1, v / 100.0); },
            -100.0, 100.0, 1.0, 0, L" %", true));
        window.rows().push_back(slide(
            tr(L"Position down"), [&sheet] { return sheet.view(2) * 100.0; },
            [&sheet](double v) { sheet.setView(2, v / 100.0); },
            -100.0, 100.0, 1.0, 0, L" %", true));
    }

    window.rows().push_back(gap());
    window.rows().push_back(heading(tr(L"Sound")));
    window.rows().push_back(toggle(
        tr(L"Normalize"), tr(L"brings the tracks below to one loudness"),
        [&sheet] { return sheet.normaliseAll(); },
        [&sheet] { sheet.setNormaliseAll(!sheet.normaliseAll()); }));
    window.rows().push_back(slide(
        tr(L"Loudness"), [&sheet] { return sheet.target(); },
        [&sheet](double v) { sheet.setTarget(v); },
        -23.0, -11.0, 1.0, 0, L" LUFS", true));

    for (size_t i = 0; i < sheet.tracks.size(); ++i) {
        SheetTrack& t = sheet.tracks[i];
        wchar_t title[160];
        _snwprintf_s(title, _TRUNCATE, L"#%d  %s  " L"\x00b7" L"  %s",
                     static_cast<int>(i) + 1, t.name.c_str(), t.note.c_str());
        window.rows().push_back(gap());
        window.rows().push_back(heading(title));
        window.rows().push_back(toggle(
            tr(L"Playing"), L"", [&t] { return t.playing(); },
            [&t] { t.setPlaying(!t.playing()); }));
        window.rows().push_back(toggle(
            tr(L"Normalize this track"), L"", [&t] { return t.normalise(); },
            [&t] { t.setNormalise(!t.normalise()); }));
        window.rows().push_back(slide(
            tr(L"Volume"), [&t] { return t.gain() * 100.0; },
            [&t](double v) { t.setGain(v / 100.0); }, 0.0, 200.0, 1.0, 0, L" %",
            false));
        window.rows().push_back(slide(
            tr(L"Bass"), [&t] { return t.low(); }, [&t](double v) { t.setLow(v); },
            -12.0, 12.0, 0.5, 1, L" dB", true));
        window.rows().push_back(slide(
            tr(L"Mids"), [&t] { return t.mid(); }, [&t](double v) { t.setMid(v); },
            -12.0, 12.0, 0.5, 1, L" dB", true));
        window.rows().push_back(slide(
            tr(L"Treble"), [&t] { return t.high(); }, [&t](double v) { t.setHigh(v); },
            -12.0, 12.0, 0.5, 1, L" dB", true));
        window.rows().push_back(slide(
            tr(L"Pitch"), [&t] { return t.pitch(); }, [&t](double v) { t.setPitch(v); },
            -12.0, 12.0, 1.0, 0, tr(L" st"), true));
        if (t.stream) {
            Row r;
            r.kind = Kind::Chip;
            r.label = tr(L"Reads from");
            r.note = tr(L"the stream of the file this row's wave, loudness and "
                        L"export come from");
            r.caption = [&t] { return t.stream(); };
            r.press = [&t] { if (t.nextStream) t.nextStream(); };
            window.rows().push_back(std::move(r));
        }
        if (t.reset) {
            window.rows().push_back(button(tr(L"Tone"), tr(L"Reset"),
                                           [&t] { t.reset(); }));
        }
    }

    window.run(owner, instance);
}

bool runTextWindow(HWND owner, HINSTANCE instance, TextItem& item, double duration,
                   double playhead, bool* deleted) {
    if (deleted) *deleted = false;
    TextItem work = item;

    Sheet sheet(tr(L"Caption"), L"", 640);
    sheet.setExtraButton(tr(L"Delete caption"));
    sheet.wantTextBox(&work.text);

    HWND ownerWindow = owner;

    sheet.rows().push_back(heading(tr(L"Type")));
    {
        Row row;
        row.kind = Kind::Chip;
        row.label = tr(L"Font");
        row.caption = [&work] {
            wchar_t buf[128];
            _snwprintf_s(buf, _TRUNCATE, L"%s  %d pt%s%s", work.font.c_str(), work.size,
                         work.bold ? L"  bold" : L"", work.italic ? L"  italic" : L"");
            return std::wstring(buf);
        };
        row.note = tr(L"opens the system font picker");
        row.press = [&work, ownerWindow] {
            LOGFONTW font{};
            // The picker works in the logical units of the screen; the size that
            // comes back is turned into the points the caption is stored in.
            font.lfHeight = -MulDiv(work.size, GetDeviceCaps(GetDC(nullptr), LOGPIXELSY),
                                    72);
            font.lfWeight = work.bold ? FW_BOLD : FW_NORMAL;
            font.lfItalic = work.italic ? TRUE : FALSE;
            wcsncpy_s(font.lfFaceName, work.font.c_str(), _TRUNCATE);

            CHOOSEFONTW choose{};
            choose.lStructSize = sizeof(choose);
            choose.hwndOwner = ownerWindow;
            choose.lpLogFont = &font;
            choose.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_NOSCRIPTSEL;
            if (!ChooseFontW(&choose)) return;
            work.font = font.lfFaceName;
            work.size = std::max(8, choose.iPointSize / 10);
            work.bold = font.lfWeight >= FW_SEMIBOLD;
            work.italic = font.lfItalic != 0;
        };
        sheet.rows().push_back(row);
    }
    sheet.rows().push_back(slide(
        tr(L"Size"), [&work] { return static_cast<double>(work.size); },
        [&work](double v) { work.size = static_cast<int>(v + 0.5); }, 8.0, 200.0, 1.0, 0,
        tr(L" pt"), false));

    sheet.rows().push_back(gap());
    sheet.rows().push_back(heading(tr(L"Colours")));
    auto colourRow = [&](const wchar_t* label, COLORREF TextItem::* field) {
        Row row;
        row.kind = Kind::Chip;
        row.isSwatch = true;
        row.label = tr(label);
        row.swatch = work.*field;
        row.caption = [&work, field] { return colourName(work.*field); };
        row.press = [&work, field, ownerWindow] {
            work.*field = askColour(ownerWindow, work.*field);
        };
        return row;
    };
    sheet.rows().push_back(colourRow(L"Fill", &TextItem::colour));
    sheet.rows().push_back(colourRow(L"Outline", &TextItem::outlineColour));
    sheet.rows().push_back(colourRow(L"Shadow", &TextItem::shadowColour));
    sheet.rows().push_back(slide(
        tr(L"Outline width"), [&work] { return static_cast<double>(work.outline); },
        [&work](double v) { work.outline = static_cast<int>(v + 0.5); }, 0.0, 12.0, 1.0,
        0, L" px", false));
    sheet.rows().push_back(slide(
        tr(L"Shadow offset"), [&work] { return static_cast<double>(work.shadow); },
        [&work](double v) { work.shadow = static_cast<int>(v + 0.5); }, 0.0, 12.0, 1.0,
        0, L" px", false));
    sheet.rows().push_back(slide(
        tr(L"Opacity"), [&work] { return static_cast<double>(work.opacity); },
        [&work](double v) { work.opacity = static_cast<int>(v + 0.5); }, 0.0, 100.0, 1.0,
        0, L" %", false));

    sheet.rows().push_back(gap());
    sheet.rows().push_back(heading(tr(L"Place")));
    {
        Row row;
        row.kind = Kind::Grid;
        row.label = tr(L"Grid");
        row.note = tr(L"where on the frame the caption sits, and which of its "
                      L"corners the margins are measured from");
        row.get = [&work] { return static_cast<double>(work.grid); };
        row.set = [&work](double v) { work.grid = static_cast<int>(v + 0.5); };
        sheet.rows().push_back(row);
    }
    sheet.rows().push_back(slide(
        tr(L"Margin across"), [&work] { return static_cast<double>(work.marginX); },
        [&work](double v) { work.marginX = static_cast<int>(v + 0.5); }, 0.0, 400.0, 5.0,
        0, L" px", false));
    sheet.rows().push_back(slide(
        tr(L"Margin down"), [&work] { return static_cast<double>(work.marginY); },
        [&work](double v) { work.marginY = static_cast<int>(v + 0.5); }, 0.0, 400.0, 5.0,
        0, L" px", false));

    sheet.rows().push_back(gap());
    sheet.rows().push_back(heading(tr(L"When")));
    sheet.rows().push_back(slide(
        tr(L"Starts"), [&work] { return work.start; },
        [&work, duration](double v) {
            const double length = work.end - work.start;
            work.start = std::max(0.0, std::min(duration - 0.1, v));
            work.end = std::min(duration, work.start + std::max(0.1, length));
        },
        0.0, std::max(1.0, duration), 0.05, 2, tr(L" s"), false));
    sheet.rows().push_back(slide(
        tr(L"Ends"), [&work] { return work.end; },
        [&work, duration](double v) {
            work.end = std::max(work.start + 0.1, std::min(duration, v));
        },
        0.0, std::max(1.0, duration), 0.05, 2, tr(L" s"), false));
    {
        Row row;
        row.kind = Kind::Button;
        row.label = tr(L"Playhead");
        row.caption = [playhead] {
            return std::wstring(tr(L"Start here")) + L"   " + seconds(playhead);
        };
        row.press = [&work, playhead, duration] {
            const double length = std::max(0.1, work.end - work.start);
            work.start = std::max(0.0, std::min(duration - 0.1, playhead));
            work.end = std::min(duration, work.start + length);
        };
        sheet.rows().push_back(row);
    }
    sheet.rows().push_back(slide(
        tr(L"Fade in"), [&work] { return work.fadeIn; },
        [&work](double v) { work.fadeIn = v; }, 0.0, 3.0, 0.05, 2, tr(L" s"), false));
    sheet.rows().push_back(slide(
        tr(L"Fade out"), [&work] { return work.fadeOut; },
        [&work](double v) { work.fadeOut = v; }, 0.0, 3.0, 0.05, 2, tr(L" s"), false));

    if (!sheet.run(owner, instance)) return false;
    if (sheet.extraPressed()) {
        if (deleted) *deleted = true;
        return true;
    }
    item = work;
    return true;
}

}  // namespace vp
