// Decoding half of the audio engine, plus the small public surface.
//
// Media Foundation does the demuxing and decoding. It ships with Windows, it
// handles every audio codec these recordings use, and it decodes more than one
// stream out of the same file at once - which is the whole reason this player
// exists. Nothing here has to be downloaded or bundled.

#include "audio_engine.h"
#include "audio_internal.h"

#include <mfapi.h>
#include <mferror.h>

#include <cstdlib>
#include <cstring>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")

using Microsoft::WRL::ComPtr;

namespace vp {

AudioEngine::AudioEngine() : impl_(std::make_unique<Impl>()) {
    MFStartup(MF_VERSION);
}

AudioEngine::~AudioEngine() {
    close();
    MFShutdown();
}

std::string AudioEngine::outputDeviceName() const { return impl_->outputName; }

std::string AudioEngine::outputDescription() const {
    return impl_->outputName + "  |  " + impl_->formatNote;
}

std::vector<std::string> AudioEngine::decodeDescriptions() const {
    return impl_->decodeNotes;
}

std::string AudioEngine::continuityReport() const {
    std::string out;
    for (size_t i = 0; i < impl_->buffers.size(); ++i) {
        char line[192];
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "track %zu: gaps filled %lld (%lld samples, biggest %lld), overlaps %lld",
                    i + 1, (long long)impl_->buffers[i]->gapFills,
                    (long long)impl_->buffers[i]->gapSamples,
                    (long long)impl_->buffers[i]->biggestGap,
                    (long long)impl_->buffers[i]->overlaps);
        out += line;
        out += "; ";
    }
    return out;
}

double AudioEngine::position() const {
    const double seconds = impl_->outputPosition.load(std::memory_order_relaxed) -
        impl_->queuedSeconds.load(std::memory_order_relaxed);
    return seconds < 0.0 ? 0.0 : seconds;
}

void AudioEngine::play() { playing_.store(true, std::memory_order_relaxed); }
void AudioEngine::pause() { playing_.store(false, std::memory_order_relaxed); }

void AudioEngine::setSpeed(double s) {
    s = std::isfinite(s) ? std::clamp(s, 0.025, 16.0) : 1.0;
    speed_.store(s, std::memory_order_relaxed);
}

void AudioEngine::setKeepPitch(bool on) {
    keepPitch_.store(on, std::memory_order_relaxed);
}

void AudioEngine::setClipEdits(const std::vector<Segment>& segments) {
    const auto previous = std::atomic_load(&impl_->clipEdits);
    auto equal = [](const ClipAudio& a, const ClipAudio& b) {
        return a.set == b.set && a.gain == b.gain && a.eqLow == b.eqLow &&
            a.eqMid == b.eqMid && a.eqHigh == b.eqHigh && a.pitch == b.pitch && a.silent == b.silent;
    };
    bool same = previous && previous->size() == segments.size();
    for (size_t i=0;same && i<segments.size();++i) {
        const auto& a=(*previous)[i]; const auto& b=segments[i];
        same = a.start==b.start && a.end==b.end && a.muted==b.muted && a.audio.size()==b.audio.size();
        auto x=a.audio.begin(), y=b.audio.begin();
        for (;same && x!=a.audio.end();++x,++y) same=x->first==y->first && equal(x->second,y->second);
    }
    if (!same) {
        std::shared_ptr<const std::vector<Segment>> next = std::make_shared<std::vector<Segment>>(segments);
        std::atomic_store(&impl_->clipEdits,next);
    }
}

void AudioEngine::seek(double seconds) {
    std::lock_guard<std::mutex> lock(impl_->renderMutex);
    if (seconds < 0.0) seconds = 0.0;
    const int64_t frame = static_cast<int64_t>(seconds * kMixRate);
    impl_->playPos.store(frame, std::memory_order_relaxed);
    impl_->outputPosition.store(seconds, std::memory_order_relaxed);
    impl_->queuedSeconds.store(0.0, std::memory_order_relaxed);
    impl_->seekRequest.store(frame, std::memory_order_release);
    impl_->flushSpeed.store(true, std::memory_order_release);
}

bool AudioEngine::open(const std::wstring& path, std::string* error) {
    close();

    ComPtr<IMFAttributes> attrs;
    if (FAILED(MFCreateAttributes(&attrs, 2))) {
        if (error) *error = "MFCreateAttributes failed";
        return false;
    }
    // The reader would happily decode video too. Only audio is wanted here; the
    // picture is mpv's job and it reads the file for itself.
    attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, FALSE);

    HRESULT hr = MFCreateSourceReaderFromURL(path.c_str(), attrs.Get(), &impl_->reader);
    if (FAILED(hr)) {
        if (error) *error = "this file could not be opened for audio";
        return false;
    }

    impl_->reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);

    // One decoded format for everything, decided here and never revisited.
    // Media Foundation inserts whatever decoder and resampler each stream needs
    // to reach it, so nothing downstream ever has to ask what the file held.
    int audioSeen = 0;
    bool hasVideo = false;
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> native;
        hr = impl_->reader->GetNativeMediaType(i, 0, &native);
        if (hr == MF_E_INVALIDSTREAMNUMBER) break;
        if (FAILED(hr)) continue;

        GUID major{};
        if (FAILED(native->GetMajorType(&major))) continue;
        if (major == MFMediaType_Video) hasVideo = true;
        if (major != MFMediaType_Audio) continue;

        // Counted before anything can go wrong with it, so a stream that will
        // not decode still uses up its number - which is what ffmpeg will do.
        const int audioOrdinal = audioSeen++;

        UINT32 srcRate = 0, srcChannels = 0;
        native->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &srcRate);
        native->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &srcChannels);

        ComPtr<IMFMediaType> want;
        if (FAILED(MFCreateMediaType(&want))) continue;
        want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
        want->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kMixRate);
        want->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kMixChannels);
        want->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
        want->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, kMixChannels * 4);
        want->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kMixRate * kMixChannels * 4);
        want->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);

        if (FAILED(impl_->reader->SetCurrentMediaType(i, nullptr, want.Get()))) continue;
        if (FAILED(impl_->reader->SetStreamSelection(i, TRUE))) continue;

        // Read the format back rather than trusting the request. A source
        // reader may accept a media type and still deliver something else, and
        // interpreting mono bytes as stereo halves the frame count - which
        // plays the track at double speed and shifts its whole spectrum up an
        // octave. That sounds like a broken codec, not like a counting mistake.
        {
            ComPtr<IMFMediaType> got;
            if (SUCCEEDED(impl_->reader->GetCurrentMediaType(i, &got))) {
                UINT32 rate = 0, ch = 0, bits = 0;
                got->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
                got->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
                got->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
                char note[128];
                _snprintf_s(note, sizeof(note), _TRUNCATE,
                            "decoding as %u Hz, %u ch, %u bits", rate, ch, bits);
                impl_->decodeNotes.push_back(note);
                if (rate != kMixRate || ch != kMixChannels) {
                    impl_->decodeNotes.back() += "  <-- NOT what was asked for";
                }
            }
        }

        TrackInfo info;
        info.index = static_cast<int>(i);
        info.audioIndex = audioOrdinal;
        info.sourceRate = static_cast<int>(srcRate);
        info.sourceChannels = static_cast<int>(srcChannels);
        info.name = "Track " + std::to_string(tracks_.size() + 1);
        tracks_.push_back(info);

        impl_->streamIndex.push_back(i);
        impl_->buffers.push_back(std::make_unique<TrackBuffer>());
        impl_->buffers.back()->reset(0);
        controls_.push_back(std::make_unique<TrackControls>());
    }

    if (tracks_.empty() && !hasVideo) {
        if (error) *error = "the file has no audio track this machine can decode";
        impl_->reader.Reset();
        return false;
    }

    // Media Foundation hands the streams over in the reverse of the order the
    // file holds them, and offers no way to ask which is which, so they are put
    // back. Done here, before anything is playing and before the window has
    // seen the list, so nothing has to be moved about later: the first row is
    // the first track of the file, called Track 1, reading a:0. When a file
    // turns out not to follow this, the "Reads from" chip in the clip window
    // points a row at another stream.
    std::reverse(tracks_.begin(), tracks_.end());
    std::reverse(controls_.begin(), controls_.end());
    std::reverse(impl_->buffers.begin(), impl_->buffers.end());
    std::reverse(impl_->streamIndex.begin(), impl_->streamIndex.end());
    for (size_t i = 0; i < tracks_.size(); ++i) {
        tracks_[i].audioIndex = static_cast<int>(i);
        tracks_[i].name = "Track " + std::to_string(i + 1);
    }

    PROPVARIANT var;
    PropVariantInit(&var);
    if (SUCCEEDED(impl_->reader->GetPresentationAttribute(
            static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &var))) {
        duration_ = static_cast<double>(var.uhVal.QuadPart) / kHnsPerSecond;
    }
    PropVariantClear(&var);

    if (!startOutput(error)) {
        close();
        return false;
    }

    impl_->decodeStop.store(false);
    if (!tracks_.empty()) impl_->decodeThread = std::thread([this] { decodeLoop(); });
    else impl_->eof.store(true);
    return true;
}

void AudioEngine::close() {
    impl_->renderStop.store(true);
    if (impl_->renderThread.joinable()) impl_->renderThread.join();
    impl_->decodeStop.store(true);
    if (impl_->decodeThread.joinable()) impl_->decodeThread.join();

    if (impl_->client) impl_->client->Stop();
    impl_->render.Reset();
    impl_->client.Reset();
    impl_->reader.Reset();

    impl_->streamIndex.clear();
    impl_->buffers.clear();
    tracks_.clear();
    controls_.clear();
    duration_ = 0.0;
    impl_->playPos.store(0);
    impl_->outputPosition.store(0);
    impl_->queuedSeconds.store(0);
    impl_->seekRequest.store(-1);
    impl_->speedChanger.reset();
    std::atomic_store(&impl_->clipEdits, std::shared_ptr<const std::vector<Segment>>{});
    impl_->equalisers.clear(); impl_->shifters.clear(); impl_->meters.clear();
    impl_->eqSetting.clear(); impl_->pitchSetting.clear();
    if (impl_->mixFormat) { CoTaskMemFree(impl_->mixFormat); impl_->mixFormat = nullptr; }
    if (impl_->bufferEvent) { CloseHandle(impl_->bufferEvent); impl_->bufferEvent = nullptr; }
    impl_->eof.store(false);
    playing_.store(false);
}

void AudioEngine::decodeLoop() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // Below normal on purpose. This thread may stall on a disk; the one that
    // must never stall is the renderer, and giving them equal standing is how a
    // slow read turns into a hole in the sound.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    std::vector<float> scratch;

    while (!impl_->decodeStop.load(std::memory_order_relaxed)) {
        const int64_t wanted = impl_->seekRequest.exchange(-1, std::memory_order_acquire);
        if (wanted >= 0) {
            PROPVARIANT pos;
            PropVariantInit(&pos);
            pos.vt = VT_I8;
            pos.hVal.QuadPart = wanted * kHnsPerSecond / kMixRate;
            impl_->reader->SetCurrentPosition(GUID_NULL, pos);
            PropVariantClear(&pos);
            for (auto& b : impl_->buffers) b->reset(wanted);
            impl_->eof.store(false, std::memory_order_relaxed);
            continue;
        }

        // Stay a couple of seconds ahead and no more. Decoding further would
        // only mean throwing more of it away at the next seek.
        const int64_t pos = impl_->playPos.load(std::memory_order_relaxed);
        int64_t least = INT64_MAX;
        for (auto& b : impl_->buffers) if (!b->finished.load(std::memory_order_relaxed))
            least = std::min(least, b->available(pos));
        if (least > kMixRate * 2 || impl_->eof.load(std::memory_order_relaxed)) {
            Sleep(5);
            continue;
        }

        DWORD stream = 0, flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        HRESULT hr = impl_->reader->ReadSample(
            static_cast<DWORD>(MF_SOURCE_READER_ANY_STREAM), 0,
            &stream, &flags, &timestamp, &sample);
        if (FAILED(hr)) { Sleep(5); continue; }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            for (size_t i=0;i<impl_->streamIndex.size();++i)
                if (impl_->streamIndex[i]==stream) impl_->buffers[i]->finished.store(true,std::memory_order_relaxed);
            const bool all = std::all_of(impl_->buffers.begin(),impl_->buffers.end(),
                [](const auto& buffer) { return buffer->finished.load(std::memory_order_relaxed); });
            impl_->eof.store(all, std::memory_order_relaxed);
        }
        if (!sample) continue;

        size_t track = impl_->streamIndex.size();
        for (size_t i = 0; i < impl_->streamIndex.size(); ++i) {
            if (impl_->streamIndex[i] == stream) { track = i; break; }
        }
        if (track >= impl_->buffers.size()) continue;

        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) continue;

        BYTE* raw = nullptr;
        DWORD length = 0;
        if (FAILED(buffer->Lock(&raw, nullptr, &length))) continue;

        const int64_t frames = length / (kMixChannels * sizeof(float));
        if (frames > 0) {
            scratch.resize(static_cast<size_t>(frames) * kMixChannels);
            std::memcpy(scratch.data(), raw,
                        static_cast<size_t>(frames) * kMixChannels * sizeof(float));
            impl_->buffers[track]->write(hnsToFrames(timestamp), scratch.data(), frames);
        }
        buffer->Unlock();
    }
    if (SUCCEEDED(com)) CoUninitialize();
}

}  // namespace vp
