#include "audio_internal.h"
#include <iostream>
#include <cmath>

namespace vp {
struct AudioSemanticProbe {
    static int run(const std::wstring& source,const std::wstring& videoOnly,const std::wstring& unequalTracks) {
        int failures=0;
        auto check=[&](bool pass,const char* name) {
            std::cout << (pass ? "PASS " : "FAIL ") << name << '\n'; failures += !pass;
        };
        {
            AudioEngine engine;
            engine.duration_=4;
            engine.tracks_.resize(2);
            std::vector<float> tones[2];
            for (int t=0;t<2;++t) {
                engine.controls_.push_back(std::make_unique<TrackControls>());
                engine.impl_->buffers.push_back(std::make_unique<TrackBuffer>());
                engine.impl_->buffers.back()->reset(0);
                tones[t].resize(3*kMixRate*2);
                for (int f=0;f<3*kMixRate;++f) for (int c=0;c<2;++c)
                    tones[t][f*2+c]=static_cast<float>(.1*std::sin(2*kPi*(t ? 880 : 440)*f/kMixRate));
                engine.impl_->buffers.back()->write(0,tones[t].data(),3*kMixRate);
            }
            engine.controls(1).enabled=false;
            engine.playing_=true;
            std::vector<Segment> edits={{0,1},{1,3}};
            edits[1].muted.insert(0);
            engine.setClipEdits(edits);
            engine.impl_->playPos=kMixRate/2;
            std::vector<float> out(kMixRate*2,0);
            check(engine.mixBlock(out.data(),kMixRate)==kMixRate,"mixer spans clip boundary without dropping samples");
            double before=0,after=0;
            for (int f=0;f<kMixRate;++f) (f<kMixRate/2 ? before : after)+=out[f*2]*out[f*2];
            check(before>100 && after<1e-10,"local mute switches at exact source sample, independent of UI timer");
            edits[1].muted.clear(); edits[1].audio[0].set=true; edits[1].audio[0].gain=.25;
            engine.setClipEdits(edits); engine.impl_->playPos=kMixRate/2;
            std::fill(out.begin(),out.end(),0.0f); engine.mixBlock(out.data(),kMixRate);
            before=after=0;
            for (int f=0;f<kMixRate;++f) (f<kMixRate/2 ? before : after)+=out[f*2]*out[f*2];
            check(std::abs(after/before-.0625)<.001,"clip volume is applied only to its source interval");
            edits[1].audio[0].gain=1;
            edits[1].audio[0].pitch=12; edits[1].audio[0].eqMid=6;
            engine.controls(0).pitch=-12; engine.controls(0).eqMid=-6;
            engine.setClipEdits(edits); engine.impl_->playPos=kMixRate;
            std::fill(out.begin(),out.end(),0.0f); engine.mixBlock(out.data(),kMixRate);
            double difference=0;
            for(int i=0;i<kMixRate*2;++i) difference+=std::abs(out[i]-tones[0][kMixRate*2+i]);
            check(difference<.01,"global and local pitch/EQ cancel as summed settings");
            engine.controls(0).pitch=0; engine.controls(0).eqMid=0;
            engine.controls(0).normalise=true; engine.controls(0).normaliseReference=-20;
            engine.setNormalise(true); engine.setNormaliseTarget(-14);
            engine.setClipEdits({}); engine.impl_->playPos=0;
            std::fill(out.begin(),out.end(),0.0f); engine.mixBlock(out.data(),kMixRate);
            double actual=0,original=0;
            for(int i=0;i<kMixRate*2;++i) { actual+=out[i]*out[i]; original+=tones[0][i]*tones[0][i]; }
            check(std::abs(std::sqrt(actual/original)-std::pow(10.0,6.0/20))<.001,
                  "preview uses same measured normalization gain as export");
            engine.setSpeed(.1); check(std::abs(engine.speed()-.1)<1e-9,"0.1x audio speed is not clamped to 0.25x");
            engine.setSpeed(16); check(engine.speed()==16,"combined clip/player speeds support 16x");
        }
        {
            AudioEngine engine;
            std::string error;
            check(engine.open(source,&error),"open real multi-track source on WASAPI");
            if (!engine.impl_->client) { std::cout << error << '\n'; return failures+1; }
            engine.setMasterGain(0);
            check(engine.impl_->mixFormat->nSamplesPerSec==48000 && engine.impl_->mixFormat->nChannels==2,
                  "WASAPI client uses mixer rate and Windows converts endpoint format");
            for(double position : {.5,2.2,4.4}) {
                engine.seek(position);
                check(std::abs(engine.position()-position)<1.0/48000,"paused seek clock reports requested source position");
            }
            for(double speed : {.1,.5,2.0,4.0}) {
                engine.pause(); engine.seek(.5); engine.setSpeed(speed); Sleep(150);
                const double before=engine.position();
                const auto start=GetTickCount64(); engine.play(); Sleep(500); engine.pause();
                const double elapsed=(GetTickCount64()-start)/1000.0;
                const double movement=engine.position()-before;
                check(std::abs(movement-elapsed*speed)<.04+speed*.04,
                      ("audible clock tracks real output at "+std::to_string(speed)+"x").c_str());
                std::cout << "  source movement " << movement << "s in " << elapsed << "s\n";
            }
            engine.close();
            if(!videoOnly.empty()) {
                check(engine.open(videoOnly,&error),"open video without any audio stream");
                check(engine.tracks().empty() && engine.duration()>0,"video-only source retains duration and empty mixer");
                engine.setMasterGain(0); engine.play(); Sleep(150); engine.pause();
                check(engine.position()>.08,"video-only playback clock advances without a sound track");
                engine.close();
            }
            if(!unequalTracks.empty()) {
                check(engine.open(unequalTracks,&error),"open source with audio tracks of different lengths");
                engine.setMasterGain(0); engine.setSpeed(1);
                engine.play(); Sleep(1800); engine.pause();
                check(engine.position()>1.6,"short audio track does not stall the shared playback clock");
                const bool unfinished=std::any_of(engine.impl_->buffers.begin(),engine.impl_->buffers.end(),
                    [](const auto& buffer){ return !buffer->finished.load(); });
                check(unfinished,"decoder keeps decoding longer track after first track ends");
                {
                    std::lock_guard<std::mutex> lock(engine.impl_->renderMutex);
                    engine.playing_=true; engine.setMasterGain(1);
                    std::vector<float> block(480*2,0);
                    engine.mixBlock(block.data(),480);
                    float peak=0;
                    for(float sample:block) peak=std::max(peak,std::abs(sample));
                    check(peak>.03f,"longer track still contains sound after shorter track ends");
                    engine.playing_=false; engine.setMasterGain(0);
                }
                engine.close();
            }
        }
        return failures ? 1 : 0;
    }
};
}
int wmain(int argc,wchar_t** argv) {
    if(argc<2 || argc>4) return 2;
    CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    const int result=vp::AudioSemanticProbe::run(argv[1],argc>2 ? argv[2] : L"",argc>3 ? argv[3] : L"");
    CoUninitialize(); return result;
}
