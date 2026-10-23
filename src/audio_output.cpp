// Output half of the audio engine: the device, the render thread, the mixer.
//
// Split from the decoding half on purpose. Everything in this file runs to a
// deadline and everything in the other file does not, and keeping the two apart
// makes it obvious which rules apply where.

#include "audio_engine.h"

#ifndef NOMINMAX
#define NOMINMAX   // windows.h defines min and max as macros
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <ksmedia.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "audio_internal.h"

using Microsoft::WRL::ComPtr;

namespace vp {

bool AudioEngine::startOutput(std::string* error) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) { if (error) *error = "no audio device enumerator"; return false; }

    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) {
        if (error) *error = "Windows reports no default playback device";
        return false;
    }

    ComPtr<IPropertyStore> props;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
        PROPVARIANT name;
        PropVariantInit(&name);
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &name)) && name.pwszVal) {
            const int need = WideCharToMultiByte(CP_UTF8, 0, name.pwszVal, -1,
                                                 nullptr, 0, nullptr, nullptr);
            if (need > 1) {
                std::string out(static_cast<size_t>(need), '\0');
                WideCharToMultiByte(CP_UTF8, 0, name.pwszVal, -1, &out[0], need,
                                    nullptr, nullptr);
                out.resize(static_cast<size_t>(need-1));
                impl_->outputName = out;
            }
        }
        PropVariantClear(&name);
    }

    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(impl_->client.GetAddressOf())))) {
        if (error) *error = "the playback device refused to open";
        return false;
    }

    if (FAILED(impl_->client->GetMixFormat(&impl_->mixFormat))) {
        if (error) *error = "the playback device did not report a format";
        return false;
    }

    // The mixer always produces stereo float at 48 kHz. Ask Windows to convert
    // to the endpoint's format; writing our samples at a 44.1 kHz device rate
    // otherwise changes pitch, duration and the video clock.
    CoTaskMemFree(impl_->mixFormat);
    impl_->mixFormat = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
    if (!impl_->mixFormat) { if (error) *error = "out of memory for audio format"; return false; }
    *impl_->mixFormat = {};
    impl_->mixFormat->wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    impl_->mixFormat->nChannels = kMixChannels;
    impl_->mixFormat->nSamplesPerSec = kMixRate;
    impl_->mixFormat->wBitsPerSample = 32;
    impl_->mixFormat->nBlockAlign = kMixChannels * sizeof(float);
    impl_->mixFormat->nAvgBytesPerSec = kMixRate * impl_->mixFormat->nBlockAlign;

    // Shared mode, event driven, at the device's own period.
    //
    // This is the ordinary path every application takes, and taking it is the
    // whole point. Asking for a custom buffer size pushes WASAPI onto a polling
    // path where the host has to guess when to refill; the event says exactly
    // when, so a block is never late for a reason nobody can see. Sharing also
    // leaves the rest of the system its sound.
    REFERENCE_TIME period = 0;
    impl_->client->GetDevicePeriod(&period, nullptr);
    hr = impl_->client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                   AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                       AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                       AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                   period, 0, impl_->mixFormat, nullptr);
    if (FAILED(hr)) {
        if (error) *error = "the playback device refused the format";
        return false;
    }

    impl_->bufferEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!impl_->bufferEvent) {
        if (error) *error = "could not create the buffer event";
        return false;
    }
    if (FAILED(impl_->client->SetEventHandle(impl_->bufferEvent))) {
        if (error) *error = "could not attach the audio buffer event";
        return false;
    }

    if (FAILED(impl_->client->GetBufferSize(&impl_->bufferFrames))) {
        if (error) *error = "could not read the device buffer size";
        return false;
    }
    if (FAILED(impl_->client->GetService(IID_PPV_ARGS(&impl_->render)))) {
        if (error) *error = "could not open the render client";
        return false;
    }

    {
        const WAVEFORMATEX* f = impl_->mixFormat;
        char note[256];
        _snprintf_s(note, sizeof(note), _TRUNCATE,
                    "device format: tag %u, %u ch, %u Hz, %u bits, block %u, buffer %u frames",
                    (unsigned)f->wFormatTag, (unsigned)f->nChannels,
                    (unsigned)f->nSamplesPerSec, (unsigned)f->wBitsPerSample,
                    (unsigned)f->nBlockAlign, (unsigned)impl_->bufferFrames);
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f);
        const bool flt = f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                         (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                          IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT));
        impl_->formatNote = std::string(note) + (flt ? ", float" : ", integer");
    }

    outputLatency_ = static_cast<double>(impl_->bufferFrames) /
                     static_cast<double>(impl_->mixFormat->nSamplesPerSec);

    impl_->renderStop.store(false);
    if (FAILED(impl_->client->Start())) {
        if (error) *error = "the playback device refused to start";
        return false;
    }
    impl_->renderThread = std::thread([this] { renderLoop(); });
    return true;
}

void AudioEngine::renderLoop() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // The one thread here with a deadline, and MMCSS is how Windows is told so.
    // Priority alone only decides who wins a contest for the processor; this
    // says the work counts as late if it is not finished by the next device
    // period, which is the thing that actually keeps other software from
    // stealing the moment this thread needed.
    DWORD taskIndex = 0;
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    // What the endpoint actually wants, decided once.
    //
    // Getting this wrong is not a subtle bug. Writing sixteen-bit samples into
    // a buffer the device reads as thirty-two-bit leaves the upper half of every
    // sample filled with whatever the next sample happened to be: the content is
    // still faintly there, buried under a wall of high-frequency noise. It
    // sounds like a broken decoder rather than like a format mistake, which is
    // exactly why the sample width has to be read rather than assumed.
    const WAVEFORMATEX* fmt = impl_->mixFormat;
    const int outChannels = fmt->nChannels;
    const int bytesPerSample = fmt->wBitsPerSample / 8;
    bool isFloat = fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
        isFloat = IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != 0;
    }

    while (!impl_->renderStop.load(std::memory_order_relaxed)) {
        if (WaitForSingleObject(impl_->bufferEvent, 200) != WAIT_OBJECT_0) continue;
        std::lock_guard<std::mutex> lock(impl_->renderMutex);

        UINT32 padding = 0;
        if (FAILED(impl_->client->GetCurrentPadding(&padding))) continue;
        const UINT32 frames = impl_->bufferFrames - padding;
        if (frames == 0) continue;

        BYTE* out = nullptr;
        if (FAILED(impl_->render->GetBuffer(frames, &out))) continue;

        impl_->mixBuffer.assign(static_cast<size_t>(frames) * kMixChannels, 0.0f);

        // Speed lives here rather than inside the mixer: the mixer's job is to
        // produce the file's own audio at the file's own rate, and how fast that
        // is then handed to the device is a separate question.
        if (impl_->flushSpeed.exchange(false, std::memory_order_acquire)) {
            impl_->speedChanger.reset();
            for (auto& eq : impl_->equalisers) eq.reset();
            for (auto& shifter : impl_->shifters) shifter = PitchShifter{};
            std::fill(impl_->pitchSetting.begin(), impl_->pitchSetting.end(), 0.0f);
        }
        impl_->speedChanger.configure(speed_.load(std::memory_order_relaxed),
                                      keepPitch_.load(std::memory_order_relaxed));

        int64_t produced = 0;
        if (!playing_.load(std::memory_order_relaxed) || impl_->outputPosition.load() >= duration_) {
            // Pausing must preserve the stretcher's queued samples.
        } else if (!impl_->speedChanger.active()) {
            produced = mixBlock(impl_->mixBuffer.data(), frames);
        } else {
            // Topped up until it can answer, but never forever: a track that has
            // not decoded far enough stalls the mixer, and spinning here would
            // hold the device buffer past its deadline.
            for (int guard = 0; guard < 16; ++guard) {
                const int64_t want = impl_->speedChanger.wantInput(frames);
                if (want <= 0) break;
                const int64_t chunk = std::min<int64_t>(want, 8192);
                impl_->speedSource.assign(static_cast<size_t>(chunk) * kMixChannels, 0.0f);
                if (mixBlock(impl_->speedSource.data(), chunk) <= 0) break;
                impl_->speedChanger.push(impl_->speedSource.data(), chunk);
            }
            produced = impl_->speedChanger.pull(impl_->mixBuffer.data(), frames);
        }
        const double speed = speed_.load(std::memory_order_relaxed);
        if (produced > 0) {
            const double cursor = impl_->outputPosition.load(std::memory_order_relaxed);
            impl_->outputPosition.store(std::min(duration_, cursor + produced*speed/kMixRate), std::memory_order_relaxed);
        }
        impl_->queuedSeconds.store(produced > 0 ? (padding+produced)*speed/kMixRate : 0.0,
                                   std::memory_order_relaxed);

        // Spread our stereo over whatever the endpoint wants, in whatever width
        // it wants. The ordinary case is two channels of float and a copy.
        for (UINT32 i = 0; i < frames; ++i) {
            for (int c = 0; c < outChannels; ++c) {
                float v = 0.0f;
                if (c == 0) v = impl_->mixBuffer[i * 2 + 0];
                else if (c == 1) v = impl_->mixBuffer[i * 2 + 1];
                v = std::min(1.0f, std::max(-1.0f, v));

                BYTE* at = out + (static_cast<size_t>(i) * outChannels + c) * bytesPerSample;
                if (isFloat) {
                    if (bytesPerSample == 4) {
                        *reinterpret_cast<float*>(at) = v;
                    } else {
                        *reinterpret_cast<double*>(at) = v;
                    }
                } else if (bytesPerSample == 2) {
                    *reinterpret_cast<int16_t*>(at) = static_cast<int16_t>(v * 32767.0f);
                } else if (bytesPerSample == 3) {
                    const int32_t s = static_cast<int32_t>(v * 8388607.0f);
                    at[0] = static_cast<BYTE>(s & 0xFF);
                    at[1] = static_cast<BYTE>((s >> 8) & 0xFF);
                    at[2] = static_cast<BYTE>((s >> 16) & 0xFF);
                } else {
                    *reinterpret_cast<int32_t*>(at) =
                        static_cast<int32_t>(v * 2147483520.0f);
                }
            }
        }
        impl_->render->ReleaseBuffer(frames, 0);
    }

    if (task) AvRevertMmThreadCharacteristics(task);
    if (SUCCEEDED(com)) CoUninitialize();
}

bool AudioEngine::anySolo() const {
    for (const auto& c : controls_) {
        if (c->solo.load(std::memory_order_relaxed)) return true;
    }
    return false;
}

// The mixer is stopped by the caller before this runs, and the reader only ever
// finds its slot by the file's stream number, so it keeps writing to the right
// place. Filter state is thrown away rather than carried across: a bank of
// coefficients belonging to another track is worse than a block of settling.
void AudioEngine::reorderTracks(const std::vector<size_t>& order) {
    const size_t n = tracks_.size();
    if (order.size() != n) return;
    for (size_t k = 0; k < n; ++k) {
        if (order[k] >= n) return;
    }

    std::vector<TrackInfo> info;
    std::vector<std::unique_ptr<TrackControls>> controls;
    std::vector<std::unique_ptr<TrackBuffer>> buffers;
    std::vector<DWORD> streams;
    info.reserve(n);
    controls.reserve(n);
    buffers.reserve(n);
    streams.reserve(n);
    for (size_t k = 0; k < n; ++k) {
        const size_t from = order[k];
        info.push_back(tracks_[from]);
        controls.push_back(std::move(controls_[from]));
        buffers.push_back(std::move(impl_->buffers[from]));
        streams.push_back(impl_->streamIndex[from]);
    }
    tracks_ = std::move(info);
    controls_ = std::move(controls);
    impl_->buffers = std::move(buffers);
    impl_->streamIndex = std::move(streams);

    impl_->equalisers.clear();
    impl_->shifters.clear();
    impl_->meters.clear();
    impl_->eqSetting.clear();
    impl_->pitchSetting.clear();
}

int64_t AudioEngine::mixBlock(float* out, int64_t frames) {
    const auto edits = std::atomic_load(&impl_->clipEdits);
    int64_t done = 0;
    while (done < frames) {
        const int64_t position = impl_->playPos.load(std::memory_order_relaxed);
        const Segment* segment = nullptr;
        int64_t chunk = frames - done;
        if (edits) for (const auto& item : *edits) {
            const int64_t start = static_cast<int64_t>(std::llround(item.start*kMixRate));
            const int64_t end = static_cast<int64_t>(std::llround(item.end*kMixRate));
            if (position >= start && position < end) {
                segment = &item; chunk = std::min(chunk,end-position); break;
            }
            if (start > position) chunk = std::min(chunk,start-position);
        }
        const int64_t made = mixChunk(out+done*kMixChannels,chunk,segment,static_cast<bool>(edits));
        if (made <= 0) break;
        done += made;
    }
    return done;
}

int64_t AudioEngine::mixChunk(float* out, int64_t frames, const Segment* segment, bool scheduled) {
    if (!playing_.load(std::memory_order_relaxed)) return 0;  // silence, clock held

    const int64_t pos = impl_->playPos.load(std::memory_order_relaxed);
    const size_t tracks = impl_->buffers.size();
    const bool solo = anySolo();

    auto audible = [&](size_t i) {
        return solo ? controls_[i]->solo.load(std::memory_order_relaxed)
                    : controls_[i]->enabled.load(std::memory_order_relaxed);
    };

    // Wait rather than skip. Advancing the clock past audio that has not been
    // decoded yet would lose it for good and leave a hole in the sound; holding
    // the clock costs a moment of silence the next block continues from.
    int64_t shortest = frames;
    bool any = false;
    for (size_t i = 0; i < tracks; ++i) {
        if (!audible(i)) continue;
        any = true;
        const int64_t have = impl_->buffers[i]->available(pos);
        if (impl_->buffers[i]->finished.load(std::memory_order_relaxed)) continue;
        shortest = std::min(shortest, have < 0 ? 0 : have);
    }
    if (!any) shortest = frames;
    if (shortest < frames && !impl_->eof.load(std::memory_order_relaxed)) {
        dropouts_.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    if (impl_->equalisers.size() != tracks) {
        impl_->equalisers.assign(tracks, Equaliser{});
        impl_->shifters.assign(tracks, PitchShifter{});
        impl_->pitchSetting.assign(tracks, 0.0f);
        impl_->meters.assign(tracks, LoudnessMeter{});
        impl_->eqSetting.assign(tracks * 3, 0.0f);
    }
    impl_->trackBuffer.assign(static_cast<size_t>(frames) * kMixChannels, 0.0f);

    const float master = master_.load(std::memory_order_relaxed);
    const bool normalise = normalise_.load(std::memory_order_relaxed);
    const float target = normaliseTarget_.load(std::memory_order_relaxed);

    for (size_t i = 0; i < tracks; ++i) {
        if (!audible(i)) {
            controls_[i]->peak.store(0.0f, std::memory_order_relaxed);
            continue;
        }
        ClipAudio local;
        if (scheduled) {
            if (segment) {
                const auto found = segment->audio.find(static_cast<int>(i));
                if (found != segment->audio.end() && found->second.set) local = found->second;
                if (segment->muted.count(static_cast<int>(i))) local.silent = true;
            }
        } else {
            local.gain=controls_[i]->clipGain.load(); local.eqLow=controls_[i]->clipEqLow.load();
            local.eqMid=controls_[i]->clipEqMid.load(); local.eqHigh=controls_[i]->clipEqHigh.load();
            local.pitch=controls_[i]->clipPitch.load(); local.silent=controls_[i]->clipSilent.load();
        }

        // Gathered on its own first, because everything below shapes one track
        // and mixing before shaping would put every track through one filter.
        std::fill(impl_->trackBuffer.begin(), impl_->trackBuffer.end(), 0.0f);
        if (impl_->buffers[i]->mixInto(pos, impl_->trackBuffer.data(), frames, 1.0f) <
            frames) {
            if (!impl_->buffers[i]->finished.load(std::memory_order_relaxed))
                dropouts_.fetch_add(1, std::memory_order_relaxed);
        }

        // Coefficients are rebuilt only when a band has actually moved. Doing it
        // per block would cost more than the filtering.
        // The mixer's bands plus whatever this piece of the edit adds.
        const float lo = controls_[i]->eqLow.load(std::memory_order_relaxed) +
                         local.eqLow;
        const float mid = controls_[i]->eqMid.load(std::memory_order_relaxed) +
                          local.eqMid;
        const float hi = controls_[i]->eqHigh.load(std::memory_order_relaxed) +
                         local.eqHigh;
        float* seen = &impl_->eqSetting[i * 3];
        if (seen[0] != lo || seen[1] != mid || seen[2] != hi) {
            seen[0] = lo; seen[1] = mid; seen[2] = hi;
            impl_->equalisers[i].configure(kMixRate, lo, mid, hi);
        }
        impl_->equalisers[i].run(impl_->trackBuffer.data(), frames);

        // Pitch after the bands and before the meter, so a track that has
        // been moved is measured as it will be heard.
        const float semitones =
            controls_[i]->pitch.load(std::memory_order_relaxed) +
            local.pitch;
        if (impl_->pitchSetting[i] != semitones) {
            impl_->pitchSetting[i] = semitones;
            impl_->shifters[i].configure(kMixRate, kMixChannels, semitones);
        }
        impl_->shifters[i].run(impl_->trackBuffer.data(), frames);

        // Loudness is measured before the fader, so the number describes the
        // recording rather than where somebody left the slider.
        impl_->meters[i].add(impl_->trackBuffer.data(), frames);
        controls_[i]->measuredDb.store(
            static_cast<float>(impl_->meters[i].levelDb()), std::memory_order_relaxed);
        controls_[i]->levelReady.store(impl_->meters[i].ready(),
                                       std::memory_order_relaxed);

        float correction = 1.0f;
        const double reference = controls_[i]->normaliseReference.load(std::memory_order_relaxed);
        if (normalise && controls_[i]->normalise.load(std::memory_order_relaxed) &&
            (reference > -69.0 || impl_->meters[i].ready())) {
            // Bounded both ways. Without a ceiling a nearly silent track would
            // be lifted until its noise floor was the loudest thing playing.
            const double db = std::max(-12.0, std::min(12.0,
                                  target - (reference > -69.0 ? reference : impl_->meters[i].levelDb())));
            correction = static_cast<float>(std::pow(10.0, db / 20.0));
            controls_[i]->normalisationDb.store(static_cast<float>(db),
                                                std::memory_order_relaxed);
        } else {
            controls_[i]->normalisationDb.store(0.0f, std::memory_order_relaxed);
        }

        const float clip =
            local.silent
                ? 0.0f
                : local.gain;
        const float gain = controls_[i]->gain.load(std::memory_order_relaxed) *
                           master * correction * clip;

        // The fades of this row, worked out per sample so a ramp of a second
        // is a ramp and not a staircase.
        const double inAt = controls_[i]->fadeInAt.load(std::memory_order_relaxed);
        const double inLen = controls_[i]->fadeInLen.load(std::memory_order_relaxed);
        const double outAt = controls_[i]->fadeOutAt.load(std::memory_order_relaxed);
        const double outLen = controls_[i]->fadeOutLen.load(std::memory_order_relaxed);
        const bool smooth = controls_[i]->fadeSmooth.load(std::memory_order_relaxed);
        const bool fading = inLen > 0.001 || outLen > 0.001;

        float peak = 0.0f;
        for (int64_t f = 0; f < frames; ++f) {
            float ramp = 1.0f;
            if (fading) {
                const double at = static_cast<double>(pos + f) / kMixRate;
                double g = 1.0;
                if (inLen > 0.001 && at < inAt + inLen) {
                    g *= std::max(0.0, (at - inAt) / inLen);
                }
                if (outLen > 0.001 && at > outAt) {
                    g *= std::max(0.0, 1.0 - (at - outAt) / outLen);
                }
                g = std::min(1.0, g);
                if (smooth) g = (1.0 - std::cos(g * 3.14159265358979)) * 0.5;
                ramp = static_cast<float>(g);
            }
            for (int c = 0; c < kMixChannels; ++c) {
                const int64_t s = f * kMixChannels + c;
                const float v = impl_->trackBuffer[s] * gain * ramp;
                out[s] += v;
                peak = std::max(peak, std::fabs(v));
            }
        }
        controls_[i]->peak.store(peak, std::memory_order_relaxed);
    }

    // Nothing is attenuated or limited on the way out. A sum that clips is a mix
    // somebody set too loud, and quietly pulling it down would hide that. The
    // bound here exists only so a bad frame cannot reach a speaker.
    for (int64_t i = 0; i < frames * kMixChannels; ++i) {
        out[i] = std::min(1.0f, std::max(-1.0f, out[i]));
    }

    if (mixTap_) mixTap_(out, frames, mixTapUser_);

    impl_->playPos.store(pos + frames, std::memory_order_relaxed);
    return frames;
}

}  // namespace vp
