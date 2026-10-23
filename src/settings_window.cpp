#include "settings_window.h"

#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "media_tools.h"
#include "lang.h"
#include "settings.h"
#include "theme.h"

using namespace vp::theme;

namespace vp {

namespace {

struct Hit {
    RECT r{};
    bool has(POINT p) const { return PtInRect(&r, p) != 0; }
};

// English here, translated where they are used.
const wchar_t* const kModeNames[] = {L"When the editor opens", L"Only when asked",
                                     L"Never"};
const char* const kModeKeys[] = {"editor", "manual", "off"};

int modeIndex(const std::string& value) {
    for (int i = 0; i < 3; ++i) {
        if (value == kModeKeys[i]) return i;
    }
    return 0;
}

constexpr int kWidth = 640;

class SettingsWindow {
public:
    bool run(HWND owner, HINSTANCE instance);

private:
    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
    void layout();
    void paint(HDC dc);
    void onDown(POINT p);

    HWND window_ = nullptr;
    theme::Fonts fonts_;
    POINT mouse_{};

    Settings work_;                 // edited here, copied back only on Ulozit
    bool changed_ = false;
    bool closed_ = false;

    Hit languageChip_, languageFolder_;
    std::vector<LanguageInfo> languages_;
    int languageIndex_ = 0;
    Hit editorCheck_, rememberCheck_, fullscreenCheck_, topCheck_, pitchCheck_;
    Hit stepChip_;
    Hit fpsChip_;
    Hit waveChip_, thumbChip_;
    Hit forgetPositions_, clearPresets_;
    Hit save_, cancel_;
    int bottom_ = 0;
};

void SettingsWindow::layout() {
    const int left = 26;
    const int right = kWidth - 26;
    int y = 84;

    // Language first: somebody who cannot read the rest of this window needs to
    // reach this row without reading the rest of this window.
    languageChip_.r = {left + 260, y, left + 470, y + 26};
    languageFolder_.r = {left + 480, y, right, y + 26};
    y += 46;

    editorCheck_.r = {left, y, right, y + 26};
    y += 34;
    rememberCheck_.r = {left, y, right, y + 26};
    y += 34;
    fullscreenCheck_.r = {left, y, right, y + 26};
    y += 34;
    topCheck_.r = {left, y, right, y + 26};
    y += 34;
    pitchCheck_.r = {left, y, right, y + 26};
    y += 40;
    stepChip_.r = {left + 260, y, left + 380, y + 26};
    y += 40;
    fpsChip_.r = {left + 260, y, left + 380, y + 26};
    y += 70;

    waveChip_.r = {left + 260, y, left + 470, y + 26};
    y += 42;
    thumbChip_.r = {left + 260, y, left + 470, y + 26};
    y += 56;

    forgetPositions_.r = {left, y, left + 240, y + 30};
    clearPresets_.r = {left + 252, y, left + 472, y + 30};
    y += 56;

    cancel_.r = {right - 110, y, right, y + 34};
    save_.r = {right - 230, y, right - 120, y + 34};
    bottom_ = y + 34 + 26;
}

void SettingsWindow::paint(HDC dc) {
    RECT client{};
    GetClientRect(window_, &client);
    fill(dc, client, kBack);

    const int left = 26;
    const int right = kWidth - 26;

    text(dc, {left, 24, right, 56}, tr(L"Settings"), kText,
         DT_LEFT | DT_SINGLELINE | DT_VCENTER, fonts_.bold);
    RECT rule{left, 68, right, 69};
    fill(dc, rule, kLine);

    {
        text(dc, {left, languageChip_.r.top, left + 250, languageChip_.r.bottom},
             tr(L"Language"), kTextDim, DT_LEFT | DT_SINGLELINE | DT_VCENTER, fonts_.ui);
        roundBox(dc, languageChip_.r, languageChip_.has(mouse_) ? kRowHot : kSection,
                 languageChip_.has(mouse_) ? kLineHot : kLine, 6);
        const std::wstring name =
            languageIndex_ >= 0 && languageIndex_ < static_cast<int>(languages_.size())
                ? languages_[static_cast<size_t>(languageIndex_)].name
                : L"English";
        text(dc, languageChip_.r, name, kText,
             DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS, fonts_.ui);
        roundBox(dc, languageFolder_.r, languageFolder_.has(mouse_) ? kRowHot : kSection,
                 languageFolder_.has(mouse_) ? kLineHot : kLine, 6);
        text(dc, languageFolder_.r, tr(L"Add a language\x2026"), kTextDim,
             DT_CENTER | DT_SINGLELINE | DT_VCENTER, fonts_.tiny);
    }

    auto check = [&](const Hit& h, const wchar_t* label, bool on, const wchar_t* note) {
        RECT box{h.r.left, h.r.top + 4, h.r.left + 16, h.r.top + 20};
        roundFill(dc, box, on ? kAccent : kLine, 4);
        if (on) {
            text(dc, box, L"\x2713", RGB(255, 255, 255),
                 DT_CENTER | DT_SINGLELINE | DT_VCENTER, fonts_.sub);
        }
        text(dc, {h.r.left + 26, h.r.top, h.r.left + 300, h.r.bottom}, label, kText,
             DT_LEFT | DT_SINGLELINE | DT_VCENTER, fonts_.ui);
        text(dc, {h.r.left + 306, h.r.top, h.r.right, h.r.bottom}, note, kTextDim,
             DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS, fonts_.tiny);
    };

    check(editorCheck_, tr(L"Open with the editor showing"), work_.editorByDefault,
          tr(L"the time axis is there from the start"));
    check(rememberCheck_, tr(L"Remember where each file was left"), work_.rememberPosition,
          tr(L"playback picks up where it stopped"));
    check(fullscreenCheck_, tr(L"Start full screen"), work_.startFullscreen, L"");
    check(topCheck_, tr(L"Window always on top"), work_.alwaysOnTop, L"");
    check(pitchCheck_, tr(L"Hold the pitch when the speed changes"), work_.keepPitch,
          tr(L"off speeds the sound up like tape"));

    text(dc, {left, stepChip_.r.top, left + 250, stepChip_.r.bottom},
         tr(L"Volume step for the arrow keys"), kTextDim, DT_LEFT | DT_SINGLELINE | DT_VCENTER,
         fonts_.sub);
    roundFill(dc, stepChip_.r, stepChip_.has(mouse_) ? kRowHot : kRow, 6);
    {
        wchar_t stepText[24];
        _snwprintf_s(stepText, _TRUNCATE, L"%d %%", work_.volumeStep);
        text(dc, stepChip_.r, stepText, kText, DT_CENTER | DT_SINGLELINE | DT_VCENTER,
             fonts_.sub);
    }

    text(dc, {left, fpsChip_.r.top, left + 250, fpsChip_.r.bottom},
         tr(L"How often the panels redraw"), kTextDim,
         DT_LEFT | DT_SINGLELINE | DT_VCENTER, fonts_.sub);
    roundFill(dc, fpsChip_.r, fpsChip_.has(mouse_) ? kRowHot : kRow, 6);
    {
        wchar_t fpsText[24];
        _snwprintf_s(fpsText, _TRUNCATE, L"%d fps", work_.uiFps);
        text(dc, fpsChip_.r, fpsText, kText, DT_CENTER | DT_SINGLELINE | DT_VCENTER,
             fonts_.sub);
    }

    text(dc, {left, waveChip_.r.top - 32, right, waveChip_.r.top - 8},
         tr(L"Previews in the editor"), kText, DT_LEFT | DT_SINGLELINE | DT_VCENTER, fonts_.ui);

    auto chipRow = [&](const Hit& h, const wchar_t* label, int index,
                       const wchar_t* note) {
        text(dc, {left, h.r.top, left + 250, h.r.bottom}, label, kTextDim,
             DT_LEFT | DT_SINGLELINE | DT_VCENTER, fonts_.sub);
        roundFill(dc, h.r, h.has(mouse_) ? kRowHot : kRow, 6);
        text(dc, h.r, tr(kModeNames[index]), kText,
             DT_CENTER | DT_SINGLELINE | DT_VCENTER, fonts_.sub);
        text(dc, {h.r.right + 12, h.r.top, right, h.r.bottom}, note, kTextDim,
             DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS, fonts_.tiny);
    };
    chipRow(waveChip_, tr(L"Waveforms of the sound tracks"), modeIndex(work_.waveMode),
            tr(L"reads the whole sound track"));
    chipRow(thumbChip_, tr(L"Film strip of the picture"), modeIndex(work_.thumbMode),
            tr(L"a few dozen quick seeks"));

    auto button = [&](const Hit& h, const wchar_t* label, bool primary) {
        roundFill(dc, h.r,
                  primary ? (h.has(mouse_) ? kAccentLit : kAccent)
                          : (h.has(mouse_) ? kRowHot : kRow),
                  6);
        text(dc, h.r, label, primary ? RGB(255, 255, 255) : kText,
             DT_CENTER | DT_SINGLELINE | DT_VCENTER, fonts_.ui);
    };

    wchar_t forget[64];
    _snwprintf_s(forget, _TRUNCATE, tr(L"Forget positions (%d)"),
                 static_cast<int>(work_.positions.size()));
    button(forgetPositions_, forget, false);
    wchar_t presets[64];
    _snwprintf_s(presets, _TRUNCATE, tr(L"Delete presets (%d)"),
                 static_cast<int>(work_.audioPresets.size() + work_.videoPresets.size() +
                                  work_.renderPresets.size()));
    button(clearPresets_, presets, false);

    // Where the tools were found, which is the answer to why the render button
    // is dead when it is.
    const std::wstring where =
        ffmpegPath().empty() ? tr(L"ffmpeg.exe was not found \x2013 no render and no previews")
                             : L"ffmpeg: " + ffmpegPath();
    text(dc, {left, save_.r.top - 26, right, save_.r.top - 4}, where,
         ffmpegPath().empty() ? kWarn : kTextDim,
         DT_LEFT | DT_SINGLELINE | DT_PATH_ELLIPSIS, fonts_.tiny);

    button(save_, tr(L"Save"), true);
    button(cancel_, tr(L"Cancel"), false);
}

void SettingsWindow::onDown(POINT p) {
    mouse_ = p;
    if (languageChip_.has(p)) {
        if (!languages_.empty()) {
            languageIndex_ = (languageIndex_ + 1) % static_cast<int>(languages_.size());
            work_.language = languages_[static_cast<size_t>(languageIndex_)].code;
            // Applied at once rather than on Save, so the rest of this window is
            // already in the chosen language while it is being chosen.
            loadLanguage(work_.language);
            // The frame is Windows' own, so it does not repaint with the rest.
            SetWindowTextW(window_, tr(L"Settings"));
        }
    }
    else if (languageFolder_.has(p)) {
        // The template is written every run; this opens the folder it is in, so
        // the answer to "how do I add my language" is one click and a rename.
        writeLanguageTemplate();
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring dir = exe;
        const size_t slash = dir.find_last_of(L"\\/");
        if (slash != std::wstring::npos) dir.resize(slash + 1);
        dir += L"lang";
        CreateDirectoryW(dir.c_str(), nullptr);
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    else if (editorCheck_.has(p)) { work_.editorByDefault = !work_.editorByDefault; }
    else if (rememberCheck_.has(p)) { work_.rememberPosition = !work_.rememberPosition; }
    else if (fullscreenCheck_.has(p)) { work_.startFullscreen = !work_.startFullscreen; }
    else if (topCheck_.has(p)) { work_.alwaysOnTop = !work_.alwaysOnTop; }
    else if (pitchCheck_.has(p)) { work_.keepPitch = !work_.keepPitch; }
    else if (stepChip_.has(p)) {
        const int steps[] = {1, 2, 5, 10};
        int at = 0;
        for (int i = 0; i < 4; ++i) {
            if (steps[i] == work_.volumeStep) at = i;
        }
        work_.volumeStep = steps[(at + 1) % 4];
    }
    else if (fpsChip_.has(p)) {
        const int rates[] = {15, 30, 60, 120};
        int at = 1;
        for (int i = 0; i < 4; ++i) {
            if (rates[i] == work_.uiFps) at = i;
        }
        work_.uiFps = rates[(at + 1) % 4];
    }
    else if (waveChip_.has(p)) {
        work_.waveMode = kModeKeys[(modeIndex(work_.waveMode) + 1) % 3];
    } else if (thumbChip_.has(p)) {
        work_.thumbMode = kModeKeys[(modeIndex(work_.thumbMode) + 1) % 3];
    } else if (forgetPositions_.has(p)) {
        work_.positions.clear();
    } else if (clearPresets_.has(p)) {
        work_.audioPresets.clear();
        work_.videoPresets.clear();
        work_.renderPresets.clear();
    } else if (save_.has(p)) {
        // Copied back wholesale. The live settings hold nothing this window
        // does not, so there is no field to lose by replacing all of them.
        settings() = work_;
        changed_ = true;
        DestroyWindow(window_);
        return;
    } else if (cancel_.has(p)) {
        // The language was applied live while it was being picked, so cancelling
        // has to put the old one back.
        loadLanguage(settings().language);
        SetWindowTextW(window_, tr(L"Settings"));
        DestroyWindow(window_);
        return;
    } else {
        return;
    }
    InvalidateRect(window_, nullptr, FALSE);
}

LRESULT CALLBACK SettingsWindow::proc(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    auto* self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
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
            if (self) self->onDown({GET_X_LPARAM(l), GET_Y_LPARAM(l)});
            return 0;
        case WM_MOUSEMOVE:
            if (self) {
                self->mouse_ = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_KEYDOWN:
            if (w == VK_ESCAPE) DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (self) self->closed_ = true;
            return 0;
        default: break;
    }
    return DefWindowProcW(hwnd, message, w, l);
}

bool SettingsWindow::run(HWND owner, HINSTANCE instance) {
    work_ = settings();

    // Rewritten every time this window opens, so the list a translator starts
    // from is always the list this build actually shows.
    writeLanguageTemplate();
    languages_ = availableLanguages();
    for (size_t i = 0; i < languages_.size(); ++i) {
        if (languages_[i].code == work_.language) languageIndex_ = static_cast<int>(i);
    }

    fonts_.create();
    layout();

    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.style = CS_HREDRAW | CS_VREDRAW;
    cls.lpfnWndProc = &SettingsWindow::proc;
    cls.hInstance = instance;
    cls.hCursor = LoadCursor(nullptr, IDC_ARROW);
    cls.hbrBackground = CreateSolidBrush(kBack);
    cls.lpszClassName = L"VideoPlayerCppSettings";
    RegisterClassExW(&cls);

    RECT frame{0, 0, kWidth, bottom_};
    AdjustWindowRect(&frame, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
    const int width = frame.right - frame.left;
    const int height = frame.bottom - frame.top;

    RECT ownerRect{};
    GetWindowRect(owner, &ownerRect);
    window_ = CreateWindowExW(WS_EX_DLGMODALFRAME, cls.lpszClassName, tr(L"Settings"),
                              WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                              (ownerRect.left + ownerRect.right - width) / 2,
                              std::max<LONG>(40, (ownerRect.top + ownerRect.bottom - height) / 2),
                              width, height, owner, nullptr, instance, this);
    if (!window_) { fonts_.destroy(); return false; }

    EnableWindow(owner, FALSE);
    ShowWindow(window_, SW_SHOW);

    MSG message{};
    while (!closed_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (message.message == WM_QUIT) PostQuitMessage(static_cast<int>(message.wParam));

    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    fonts_.destroy();
    return changed_;
}

}  // namespace

bool runSettingsWindow(HWND owner, HINSTANCE instance) {
    SettingsWindow window;
    return window.run(owner, instance);
}

}  // namespace vp
