#include "render.h"
#include "media_tools.h"
#include <fstream>
#include <iostream>

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    const std::wstring source=argv[1], directory=argv[2];
    vp::RenderJob base;
    base.source=source; base.duration=6; base.sourceWidth=320; base.sourceHeight=180; base.sourceFps=30;
    base.segments={{.5,1.5},{3,4.5}};
    vp::RenderTrack a,b; b.sourceIndex=b.editIndex=1;
    base.tracks={a,b};
    vp::RenderPreset preset;
    preset.encoder=4; preset.hardwareDecode=false; preset.useQuality=true;
    preset.quality=10; preset.speedPreset=0;
    int failures=0;
    auto render=[&](const wchar_t* name, const vp::RenderJob& job, const vp::RenderPreset& options) {
        std::string log;
        const auto command=vp::buildRenderCommand(job,options,directory+L"/"+name+L".mp4");
        const int code=vp::runHidden(command,&log,60000);
        std::ofstream out(directory+L"/"+name+L".log"); out << vp::toUtf8(command) << '\n' << log;
        std::wcout << name << (code ? L": FAIL\n" : L": PASS\n");
        failures += code != 0;
    };
    render(L"baseline",base,preset);
    auto job=base; job.segments[1].video.set=true; job.segments[1].video.opacity=0;
    render(L"local_opacity",job,preset);
    job=base; job.segments[1].video.set=true; job.segments[1].video.brightness=30;
    render(L"local_brightness",job,preset);
    job=base; job.segments[1].muted.insert(0);
    render(L"local_mute",job,preset);
    job=base; job.tracks[0].include=false;
    render(L"track_two",job,preset);
    job=base; job.segments[1].video.set=true; job.segments[1].video.speed=2;
    render(L"local_speed",job,preset);
    auto fast=preset; fast.speed=1.5;
    render(L"combined_speed",job,fast);
    fast=preset; fast.keepPitch=false;
    render(L"local_tape_speed",job,fast);
    job=base; job.zoom=.5;
    render(L"zoom_out",job,preset);
    job=base; job.zoom=2; job.panX=.25;
    render(L"zoom_pan",job,preset);
    job=base; job.segments[1].video.set=true; job.segments[1].video.cropW=.5;
    render(L"local_crop",job,preset);
    job=base; job.fades={{2,2},{2,2},{2,2}};
    render(L"source_fades",job,preset);
    job.fades[0].curve=vp::FadeCurve::Smooth;
    render(L"smooth_fades",job,preset);
    job=base; job.zoom=.25; job.panX=.8;
    render(L"small_zoom_pan",job,preset);
    job=base; job.tracks.clear();
    render(L"no_audio",job,preset);
    job=base; job.tracks[1].include=false; job.tracks[0].normalise=true;
    job.tracks[0].measured=-20; job.tracks[0].target=-14;
    render(L"normalization",job,preset);
    job=base; job.segments[1].audio[0].set=true; job.segments[1].audio[0].gain=.25;
    render(L"local_gain",job,preset);
    job=base; job.tracks[1].include=false; job.tracks[0].pitch=-12;
    job.segments[1].audio[0].set=true; job.segments[1].audio[0].pitch=12;
    render(L"combined_pitch",job,preset);
    job=base; job.rotate=90; job.zoom=.75; job.panX=.1;
    render(L"rotation",job,preset);
    job=base;
    vp::Timeline timeline; timeline.setMedia(6,{});
    vp::TextItem text; text.start=3.2; text.end=3.7; text.grid=5; text.size=180;
    text.text=L"TEST"; text.fadeIn=text.fadeOut=0;
    timeline.texts().push_back(text);
    job.captions=directory+L"/semantic.ass";
    { std::ofstream out(job.captions); out << timeline.assDocument(320,180); }
    render(L"caption_timing",job,preset);
    auto padded=preset; padded.outWidth=320; padded.outHeight=320;
    render(L"square_padding",base,padded);
    job=base; job.segments.clear();
    for(int i=0;i<20;++i) {
        vp::Segment piece; piece.start=i*.2; piece.end=piece.start+.1;
        piece.video.set=true; piece.video.speed=2;
        job.segments.push_back(piece);
    }
    render(L"many_fast_cuts",job,preset);
    return failures ? 1 : 0;
}
