#include "render.h"
#include "media_tools.h"
#include <fstream>
#include <iostream>
#include <cmath>

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    const std::wstring source = argv[1], dir = argv[2];
    vp::Timeline timeline;
    timeline.setMedia(6.0, {"one", "two"});
    timeline.splitAt(2.0);
    timeline.splitAt(4.0);
    timeline.segments()[1].kept = false;
    timeline.setMarkIn(1.0);
    timeline.setMarkOut(5.0);
    vp::RenderJob job;
    job.source = source; job.duration = 6;
    job.sourceWidth = 640; job.sourceHeight = 360; job.sourceFps = 30;
    job.segments = timeline.keptSegments();
    if (job.segments.size() != 2 || job.segments[0].start != 1 || job.segments[1].end != 5) return 3;
    vp::RenderTrack a, b;
    b.sourceIndex = b.editIndex = 1;
    job.tracks = {a,b};
    vp::RenderPreset preset;
    preset.encoder = 4; preset.hardwareDecode = false; preset.useQuality = true;
    int failures = 0;
    auto test = [&](const wchar_t* name, const vp::RenderJob& j, const vp::RenderPreset& p, double duration) {
        const std::wstring output = dir + L"/" + name + L".mp4";
        const auto command = vp::buildRenderCommand(j,p,output);
        std::string log;
        int code = vp::runHidden(command, &log, 60000);
        std::ofstream f(dir + L"/" + name + L".log"); f << vp::toUtf8(command) << "\n" << log;
        if (!code) {
            code = vp::runHidden(vp::quoteArgument(vp::ffprobePath()) + L" -v error -show_entries format=duration -of default=nw=1:nk=1 " + vp::quoteArgument(output), &log, 10000);
            if (std::abs(atof(log.c_str()) - duration) > 0.08) code = 100;
        }
        if (!code) code = vp::runHidden(vp::quoteArgument(vp::ffmpegPath()) + L" -v error -xerror -i " + vp::quoteArgument(output) + L" -f null -", &log, 10000);
        if (!code) {
            code = vp::runHidden(vp::quoteArgument(vp::ffprobePath()) + L" -v error -select_streams a -show_entries stream=index -of csv=p=0 " + vp::quoteArgument(output), &log, 10000);
            bool audio = false; for (const auto& t : j.tracks) audio |= t.include;
            if (audio != !log.empty()) code = 101;
        }
        std::wcout << name << L": " << (code ? L"FAIL" : L"PASS") << L" (" << code << L")\n";
        failures += code != 0;
    };
    test(L"cuts", job, preset, 2.0);
    auto silent = job; for (auto& t : silent.tracks) t.include = false;
    test(L"disabled_audio", silent, preset, 2.0);
    silent.tracks.clear(); test(L"video_only", silent, preset, 2.0);
    auto captions = job;
    vp::TextItem text; text.start=0; text.end=6; text.text=L"Render test";
    timeline.texts().push_back(text);
    captions.captions = dir + L"/captions.ass";
    { std::ofstream f(captions.captions); f << timeline.assDocument(640,360); }
    test(L"captions", captions, preset, 2.0);
    captions.captions = dir + L"/caption's.ass";
    { std::ofstream f(captions.captions); f << timeline.assDocument(640,360); }
    test(L"caption_quote", captions, preset, 2.0);
    auto fractional = job;
    fractional.segments = {{1.013,1.987},{4.013,4.987}};
    test(L"fractional_cuts", fractional, preset, 1.948);
    job.segments[1].video.set = true;
    job.segments[1].video.cropW = 0.5;
    job.segments[1].video.speed = 2;
    job.fades = {{0.2,0.2,vp::FadeCurve::Smooth},{0.1,0.1},{0.3,0.3}};
    job.tracks[0].include = false;
    test(L"crop_speed_fades", job, preset, 1.5);
    preset.speed = 1.5;
    test(L"global_speed", job, preset, 1.0);
    preset.speed = 1;
    preset.encoder = 1; preset.hardwareDecode = true;
    test(L"gpu", job, preset, 1.5);
    preset.encoder = 0; test(L"gpu_hevc", job, preset, 1.5);
    auto empty = job; empty.segments.clear();
    if (!vp::buildRenderCommand(empty,preset,L"unused.mp4").empty()) ++failures;
    std::string log;
    const auto began = GetTickCount64();
    const int timeout = vp::runHidden(vp::quoteArgument(vp::ffmpegPath()) + L" -v quiet -re -f lavfi -i color=s=640x360 -f null -", &log, 250);
    if (timeout == 0 || GetTickCount64()-began > 3000) ++failures;
    return failures ? 1 : 0;
}
