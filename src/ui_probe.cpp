// Native regression checks against the real Player, Win32 hierarchy and libmpv.
#include <iostream>
#include <functional>
#define wWinMain playerEntryForProbe
#include "main.cpp"
#undef wWinMain

namespace {
int failures = 0;
void check(bool ok, const char* name) {
    std::cout << (ok ? "PASS " : "FAIL ") << name << '\n';
    if (!ok) ++failures;
}
void pump(int milliseconds) {
    const auto until = GetTickCount64() + milliseconds;
    do {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { check(false, "unexpected WM_QUIT"); continue; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(5);
    } while (GetTickCount64() < until);
}
bool waitFor(const std::function<bool()>& predicate) {
    for (int i = 0; i < 200; ++i) {
        if (predicate()) return true;
        pump(25);
    }
    return false;
}
void CALLBACK closeModal(HWND, UINT, UINT_PTR id, DWORD) {
    for (const wchar_t* cls : {L"VideoPlayerCppSettings", L"VideoPlayerCppRender"}) {
        HWND modal = FindWindowW(cls, nullptr);
        DWORD pid = 0;
        GetWindowThreadProcessId(modal, &pid);
        if (modal && pid == GetCurrentProcessId()) {
            KillTimer(nullptr, id);
            SendMessageW(modal, WM_CLOSE, 0, 0);
            return;
        }
    }
}
void CALLBACK closeOverflowRender(HWND, UINT, UINT_PTR id, DWORD) {
    HWND modal=FindWindowW(L"VideoPlayerCppRender",nullptr);
    DWORD pid=0; GetWindowThreadProcessId(modal,&pid);
    if (!modal || pid!=GetCurrentProcessId()) return;
    KillTimer(nullptr,id);
    RECT frame{},client{}; GetWindowRect(modal,&frame); GetClientRect(modal,&client);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(modal,MONITOR_DEFAULTTONEAREST),&monitor);
    check(frame.top>=monitor.rcWork.top && frame.bottom<=monitor.rcWork.bottom,
          "render window with many tracks fits monitor work area");
    SendMessageW(modal,WM_KEYDOWN,VK_END,0);
    SendMessageW(modal,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(client.right-70,client.bottom-41));
    check(!IsWindow(modal),"scrolled render footer keeps working hit targets");
    if(IsWindow(modal)) SendMessageW(modal,WM_CLOSE,0,0);
}
}

struct PlayerUiProbe {
    static int run(const std::wstring& source) {
        Player p;
        check(p.create(GetModuleHandleW(nullptr), L""), "create player");
        if (!p.window_) return 1;
        KillTimer(p.window_, 1);
        p.editorOpen_ = false;
        p.layout();
        check(p.video_.ready(), "load bundled libmpv");
        check(GetParent(p.videoHost_) == p.videoViewport_ &&
              GetParent(p.videoViewport_) == p.window_, "video clipped by dedicated parent");

        // A transport-only update must paint even when there is no open file.
        HDC screen = GetDC(p.window_);
        HDC mem = CreateCompatibleDC(screen);
        HBITMAP bitmap = CreateCompatibleBitmap(screen, 2400, 1400);
        auto old = SelectObject(mem, bitmap);
        RECT all{0, 0, 2400, 1400};
        fill(mem, all, RGB(255, 0, 255));
        p.paintClip_ = {0, p.videoStage_.bottom, p.videoStage_.right,
                         p.videoStage_.bottom + p.transportHeight_};
        p.paint(mem);
        check(GetPixel(mem, 2, p.videoStage_.bottom + 3) == kPanel,
              "empty player paints transport on partial invalidation");

        const std::vector<Hit*> transport = {&p.play_, &p.back_, &p.stepBack_,
            &p.stepForward_, &p.forward_, &p.speedChip_, &p.pitchChip_, &p.rotateLeft_,
            &p.rotateRight_, &p.flipHChip_, &p.flipVChip_, &p.orientChip_, &p.fitChip_,
            &p.zoomReset_, &p.loopChip_, &p.full_, &p.gear_, &p.statsChip_,
            &p.editorChip_, &p.volume_, &p.muteButton_};
        const std::vector<Hit*> editor = {&p.editSplit_, &p.editDelete_, &p.editRestore_,
            &p.editResetCuts_, &p.editMarkIn_, &p.editMarkOut_, &p.editClearMarks_,
            &p.editAddText_, &p.editPreviews_, &p.editSaveProject_, &p.editOpenProject_,
            &p.editLocalProps_, &p.editClipProps_, &p.editRender_};
        auto disjoint = [&](const std::vector<Hit*>& hits) {
            bool ok = true;
            for (size_t i = 0; i < hits.size(); ++i) {
                ok &= hits[i]->r.left >= 0 && hits[i]->r.right <= p.videoStage_.right;
                for (size_t j = 0; j < i; ++j) {
                    RECT both{};
                    ok &= !IntersectRect(&both, &hits[i]->r, &hits[j]->r);
                }
            }
            return ok;
        };
        for (int width : {1240, 1366, 1500, 1800, 2200}) {
            RECT frame{0, 0, width, 800};
            AdjustWindowRectEx(&frame, static_cast<DWORD>(GetWindowLongPtrW(p.window_, GWL_STYLE)),
                                FALSE, WS_EX_ACCEPTFILES);
            SetWindowPos(p.window_, nullptr, 0, 0, frame.right - frame.left,
                          frame.bottom - frame.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            p.editorOpen_ = true;
            p.layout();
            check(disjoint(transport), ("transport controls do not overlap at " + std::to_string(width)).c_str());
            check(disjoint(editor), ("editor controls do not overlap at " + std::to_string(width)).c_str());
        }
        SetWindowPos(p.window_, nullptr, 0, 0, 1500, 900,
                      SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

        p.open(source);
        p.audio_.pause();
        p.audio_.setMasterGain(0);
        p.video_.setPaused(true);
        check(waitFor([&] { return p.video_.width() > 0; }), "decode real test video");
        p.stageW_ = p.video_.displayWidth();
        p.stageH_ = p.video_.displayHeight();
        p.layout();
        check(!IsWindowEnabled(p.videoHost_), "render subtree leaves input to viewport");
        check(p.video_.property("video-unscaled") == "no", "mpv scaling does not cancel view zoom");
        p.video_.seek(3.25);
        check(waitFor([&] { return std::abs(p.video_.videoPosition() - 3.25) < 0.08; }),
              "exact seek reaches requested timestamp while paused");

        for (double zoom : {0.25, 1.0, 2.0, 4.0}) {
            for (double pan : {-4.0, 0.0, 4.0}) {
                p.zoom_ = zoom;
                p.panX_ = pan; p.panY_ = -pan;
                p.placeCanvas();
                RECT viewport{};
                GetWindowRect(p.videoViewport_, &viewport);
                MapWindowPoints(HWND_DESKTOP, p.window_, reinterpret_cast<POINT*>(&viewport), 2);
                check(EqualRect(&viewport, &p.videoStage_) != FALSE, "viewport remains fixed during zoom and pan");
                POINT ui{p.videoStage_.right + 30, 120};
                ClientToScreen(p.window_, &ui);
                HWND hit = WindowFromPoint(ui);
                check(hit != p.videoHost_ && !IsChild(p.videoHost_, hit), "video never owns panel pixels");
            }
        }

        p.resetView();
        p.fitIndex_ = 2; p.placeCanvas();
        const int nativeWidth = p.videoCanvas_.right - p.videoCanvas_.left;
        check(nativeWidth == 640, "Original mode uses source pixel dimensions");
        p.bumpZoom(2);
        check(p.videoCanvas_.right - p.videoCanvas_.left == 2 * nativeWidth,
              "Original mode supports view zoom");
        p.zoom_ = 1;
        p.fitIndex_ = 0; p.placeCanvas();
        check(p.videoCanvas_.right <= p.videoStage_.right && p.videoCanvas_.bottom <= p.videoStage_.bottom,
              "Fit contains the whole picture");
        p.fitIndex_ = 1; p.placeCanvas();
        check(p.videoCanvas_.left <= 0 && p.videoCanvas_.top <= 0 &&
              p.videoCanvas_.right >= p.videoStage_.right && p.videoCanvas_.bottom >= p.videoStage_.bottom,
              "Fill covers the video viewport");
        p.fitIndex_ = 0; p.resetView();
        POINT wheelAt{p.videoStage_.right / 2, p.videoStage_.bottom / 2};
        // Hit-test our own hierarchy so another foreground app cannot change
        // the result of this regression test.
        HWND wheelHit = ChildWindowFromPointEx(p.videoViewport_,wheelAt,
                                               CWP_SKIPDISABLED | CWP_SKIPINVISIBLE);
        check(wheelHit == p.videoViewport_, "video hit testing reaches enabled viewport");
        ClientToScreen(p.window_, &wheelAt);
        if (wheelHit == p.videoViewport_) {
            SendMessageW(wheelHit, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA),
                         MAKELPARAM(wheelAt.x, wheelAt.y));
        }
        check(p.zoom_ > 1.0, "wheel delivered to video viewport zooms picture");
        p.onKey('Z');
        check(p.zoom_ == 1 && p.videoCanvas_.right - p.videoCanvas_.left <= p.videoStage_.right,
              "Z immediately resets canvas geometry");
        POINT down{100, 100};
        POINT inHost = down;
        MapWindowPoints(p.window_, p.videoHost_, &inHost, 1);
        SendMessageW(p.videoHost_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(inHost.x, inHost.y));
        check(p.pressedVideo_ && p.pressedAt_.x == down.x && p.pressedAt_.y == down.y,
              "nested host forwards mouse coordinates to player");
        SendMessageW(p.window_, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(120, 110));
        check(p.panning_ && p.panX_ > 0 && p.panY_ > 0, "first drag movement pans immediately");
        ReleaseCapture();
        check(!p.panning_ && !p.pressedVideo_ && p.drag_ == Player::Drag::None,
              "capture loss cancels drag without toggling playback");

        p.rotate_ = 90; p.applyGeometry();
        pump(250);
        check(p.video_.displayWidth() == 640 && p.video_.displayHeight() == 360,
              "display dimensions are read before output rotation");
        p.stageW_ = p.video_.displayWidth(); p.stageH_ = p.video_.displayHeight();
        p.placeCanvas();
        check(p.videoCanvas_.bottom - p.videoCanvas_.top > p.videoCanvas_.right - p.videoCanvas_.left,
              "rotation updates canvas aspect immediately");
        p.resetView();
        p.zoom_ = 4; p.panX_ = .7; p.panY_ = .4; p.placeCanvas();
        InvalidateRect(p.window_, nullptr, TRUE); UpdateWindow(p.window_); pump(250);

        p.zoom_ = 4;
        p.placeCanvas();
        p.toggleFullscreen();
        check((GetWindowLongPtrW(p.window_, GWL_STYLE) & WS_CLIPCHILDREN) != 0,
              "fullscreen preserves child clipping");
        p.onDown({100, 100}); p.onMove({200, 200}); p.onUp(true);
        check(p.panX_ == 0 && p.panY_ == 0, "fullscreen cannot accidentally pan video");
        p.toggleFullscreen();
        check((GetWindowLongPtrW(p.window_, GWL_STYLE) & WS_CLIPCHILDREN) != 0,
              "leaving fullscreen preserves child clipping");

        p.expanded_.assign(12, 1);
        p.trackScroll_ = 10000;
        p.layout();
        p.expanded_.assign(2, 0);
        p.layout();
        check(p.trackScroll_ == 0, "collapsing track list clamps stale scroll offset");
        p.timeline_.setMedia(6, std::vector<std::string>(24, "track"));
        p.editorOpen_ = true; p.layout();
        check(p.videoStage_.bottom >= 160, "many tracks preserve usable video area");
        p.timeline_.scrollRows(10000);
        const RECT bounds = p.timeline_.bounds();
        bool lastVisible = false;
        for (LONG y = bounds.top; y < bounds.bottom; ++y)
            lastVisible |= p.timeline_.rowAt({bounds.left + 100, y}) == 24;
        check(lastVisible, "last timeline row reachable by scrolling");
        check(p.timeline_.rowAt({bounds.left + 100, bounds.top + 1}) == -1,
              "scrolled rows do not claim ruler clicks");

        p.timeline_.setMedia(6,{"one","two"});
        p.timeline_.clipVideo(0).set=true; p.timeline_.clipVideo(0).brightness=40;
        for(auto& adjustment:p.adjust_) adjustment.value=adjustment.kind==AdjustKind::Zoom ? 100 : 0;
        p.audio_.seek(.5); p.applyClipSettings(false);
        check(p.video_.property("vf").find("eq")!=std::string::npos,
              "current clip picture effect reaches preview immediately");
        p.timeline_.resetCuts(); p.applyClipSettings(false);
        check(p.video_.property("vf").find("eq")==std::string::npos,
              "clearing cuts removes stale local picture settings at same clip index");

        fill(mem, all, RGB(255, 0, 255));
        IntersectClipRect(mem, bounds.left, bounds.top + 30, bounds.right, bounds.bottom);
        RECT clipBefore{}, clipAfter{};
        GetClipBox(mem, &clipBefore);
        p.timeline_.fades(0).in = 2;
        p.timeline_.paint(mem, 2, {}, &p.fonts_);
        GetClipBox(mem, &clipAfter);
        check(EqualRect(&clipBefore, &clipAfter) != FALSE, "timeline restores caller clipping");
        SelectClipRgn(mem, nullptr);
        check(GetPixel(mem, 1, 1) == RGB(255, 0, 255), "timeline drawing stays inside viewport");

        for (int i = 0; i < 2; ++i) {
            SetTimer(nullptr, 0, 150, closeModal);
            vp::runSettingsWindow(p.window_, p.instance_);
            MSG msg{};
            check(!PeekMessageW(&msg, nullptr, WM_QUIT, WM_QUIT, PM_NOREMOVE),
                  "closing settings leaves main message loop alive");
            vp::RenderJob job;
            job.source = source; job.duration = 6; job.sourceWidth = 640; job.sourceHeight = 360;
            job.segments = {{0, 6}};
            SetTimer(nullptr, 0, 150, closeModal);
            vp::runRenderWindow(p.window_, p.instance_, job);
            check(!PeekMessageW(&msg, nullptr, WM_QUIT, WM_QUIT, PM_NOREMOVE),
                  "closing render leaves main message loop alive");
        }
        vp::RenderJob many;
        many.source=source; many.duration=6; many.sourceWidth=640; many.sourceHeight=360;
        many.segments={{0,6}}; many.tracks.resize(40);
        SetTimer(nullptr,0,150,closeOverflowRender);
        vp::runRenderWindow(p.window_,p.instance_,many);

        vp::Timeline original;
        original.setMedia(6,{"one","two"}); original.splitAt(2); original.splitAt(4);
        original.segments()[1].kept=false;
        original.segments()[2].muted.insert(1);
        original.clipVideo(2).set=true; original.clipVideo(2).speed=2;
        original.clipVideo(2).brightness=30; original.clipVideo(2).cropW=.5;
        original.clipAudio(2,0).set=true; original.clipAudio(2,0).gain=.25;
        original.clipAudio(2,0).pitch=3;
        original.setMarkIn(.5); original.setMarkOut(5.5);
        original.fades(1)={.5,.75,vp::FadeCurve::Smooth};
        vp::TextItem caption; caption.text=L"Český titulek \U0001F43E";
        caption.start=3; caption.end=5; original.texts().push_back(caption);
        vp::Timeline restored; restored.setMedia(6,{"one","two"});
        restored.fromJson(vp::json::parse(original.toJson().dump()));
        check(restored.toJson().dump()==original.toJson().dump(),
              "project timeline roundtrip preserves cuts, local effects, audio, fades and captions");
        check(std::abs(original.keptDuration()-2.25)<1e-6,
              "timeline output duration accounts for local clip speed");
        check(original.assDocument(640,360).find(vp::toUtf8(caption.text))!=std::string::npos,
              "caption export preserves Czech text and emoji");

        SelectObject(mem, old); DeleteObject(bitmap); DeleteDC(mem); ReleaseDC(p.window_, screen);
        p.waveJob_.cancel(); p.thumbJob_.cancel(); p.loudnessJob_.cancel();
        DestroyWindow(p.window_); // Deliberately avoid saving test settings/positions.
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {}
        std::cout << "Failures: " << failures << '\n';
        return failures ? 1 : 0;
    }
};

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetProcessDPIAware();
    const int result = PlayerUiProbe::run(argv[1]);
    CoUninitialize();
    return result;
}
