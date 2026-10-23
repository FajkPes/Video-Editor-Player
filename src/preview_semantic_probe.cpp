#include <iostream>
#define wWinMain playerEntryForPreviewProbe
#include "main.cpp"
#undef wWinMain

struct PlayerUiProbe {
    static void pump(int ms) {
        const auto until=GetTickCount64()+ms;
        do {
            MSG message{};
            while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
                if(message.message!=WM_QUIT) { TranslateMessage(&message); DispatchMessageW(&message); }
            }
            Sleep(5);
        } while(GetTickCount64()<until);
    }
    static int run(const std::wstring& source,const std::wstring& directory) {
        Player p;
        if(!p.create(GetModuleHandleW(nullptr),L"")) return 1;
        KillTimer(p.window_,1);
        p.editorOpen_=false;
        p.open(source); p.audio_.pause(); p.audio_.setMasterGain(0); p.video_.setPaused(true);
        p.waveJob_.cancel(); p.thumbJob_.cancel(); p.loudnessJob_.cancel();
        p.avOffset_=0; p.fitIndex_=0;
        for(int i=0;i<200 && !p.video_.width();++i) pump(25);
        if(!p.video_.width()) return 2;
        auto capture=[&](const wchar_t* name,double time,const vp::ClipVideo& local,
                         double zoom=1,double pan=0,int rotate=0,bool portrait=false) {
            p.timeline_.setMedia(6,{"one","two"}); p.timeline_.splitAt(2);
            p.timeline_.segments()[1].video=local;
            p.frameZoom_=zoom; p.framePanX_=pan; p.framePanY_=0;
            p.rotate_=rotate; p.portrait_=portrait; p.flipH_=p.flipV_=false;
            p.audio_.seek(time);
            p.applyClipSettings(true); p.applyClipPicture(); p.applyGeometry();
            p.video_.seek(time); p.video_.showText("",1);
            pump(400);
            p.stageW_=p.video_.displayWidth(); p.stageH_=p.video_.displayHeight(); p.layout();
            pump(200);
            const std::wstring filename=directory+L"/preview_"+name+L".png";
            const std::string target=vp::toUtf8(filename);
            const char* command[]={"screenshot-to-file",target.c_str(),"window",nullptr};
            p.video_.command(command);
            pump(100);
            const bool exists=GetFileAttributesW(filename.c_str())!=INVALID_FILE_ATTRIBUTES;
            std::wcout << name << (exists ? L": PASS\n" : L": FAIL\n");
            return exists;
        };
        int failed=0;
        vp::ClipVideo local;
        failed+=!capture(L"baseline",.7,local);
        local.set=true; local.opacity=0;
        failed+=!capture(L"local_opacity",3.3,local);
        local={}; local.set=true; local.brightness=30;
        failed+=!capture(L"local_brightness",3.3,local);
        local={}; local.set=true; local.cropW=.5;
        failed+=!capture(L"local_crop",3.3,local);
        local={};
        failed+=!capture(L"zoom_out",.7,local,.5);
        failed+=!capture(L"zoom_pan",.7,local,2,.25);
        failed+=!capture(L"rotation",.7,local,.75,.1,90);
        failed+=!capture(L"small_zoom_pan",.7,local,.25,.8);
        p.waveJob_.cancel(); p.thumbJob_.cancel(); p.loudnessJob_.cancel();
        DestroyWindow(p.window_);
        return failed ? 1 : 0;
    }
};
int wmain(int argc,wchar_t** argv) {
    if(argc!=3) return 2;
    CoInitializeEx(nullptr,COINIT_MULTITHREADED); SetProcessDPIAware();
    const int code=PlayerUiProbe::run(argv[1],argv[2]);
    CoUninitialize(); return code;
}
