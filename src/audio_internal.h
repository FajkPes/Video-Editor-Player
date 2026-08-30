#pragma once
// Shared between the decoding half and the output half of the engine.

#include "audio_engine.h"
#include "audio_dsp.h"
#include "audio_stretch.h"

#ifndef NOMINMAX
#define NOMINMAX   // windows.h defines min and max as macros
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <audioclient.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

namespace vp {

// Twenty seconds of slack per track. Large on purpose: it costs only memory
// (about 7 MB a track) and it buys that a stalled disk or a slow demux never
// reaches the mixer at all.
constexpr int64_t kRingSeconds = 20;

// Media Foundation counts in 100 ns units.
constexpr int64_t kHnsPerSecond = 10000000;

inline int64_t hnsToFrames(int64_t hns) {
    return hns * kMixRate / kHnsPerSecond;
}

// One track's decoded audio, addressed by absolute frame number.
//
// Absolute addressing rather than "append to the end" is what keeps the tracks
// locked to each other and to the file. A block that arrives out of order, or a
// demuxer that hands back a little more than was asked for after a seek, lands
// where its timestamp says it belongs instead of shifting everything after it.
class TrackBuffer {
public:
    void reset(int64_t startFrame) {
        std::lock_guard<std::mutex> lock(mutex_);
        data_.assign(static_cast<size_t>(kRingSeconds) * kMixRate * kMixChannels, 0.0f);
        lo_ = hi_ = startFrame;
    }

    void write(int64_t frame, const float* samples, int64_t frames) {
        if (frames <= 0) return;
        std::lock_guard<std::mutex> lock(mutex_);
        const int64_t capacity = static_cast<int64_t>(data_.size()) / kMixChannels;
        if (capacity <= 0) return;

        // Older than the ring still holds is a block nobody can use.
        if (frame + frames <= hi_ - capacity) return;

        // Where the block goes is decided by the timestamp only when the
        // timestamp says something has actually moved. Otherwise it simply
        // follows the one before it.
        //
        // This distinction is the whole difference between clean audio and
        // audio that crackles. A timestamp is a count of 100 ns ticks and a
        // frame is 1/48000 of a second, and the two do not divide: an AAC
        // frame of 1024 samples is 21333.33 ticks, so converting each
        // timestamp to a frame number lands up to a sample away from where the
        // previous block ended. Honouring that would trim or insert a sample
        // roughly fifty times a second - each one a step in the waveform. They
        // are far too small to hear one at a time and far too many not to hear
        // together: levels, dropout counters and every audible band still
        // measure perfectly, while everything above 20 kHz fills with the
        // energy of the steps and the sound turns harsh.
        //
        // The tolerance is a tenth of a second, and it is deliberately wide.
        //
        // One millisecond was tried first and was far too strict: measured on a
        // real recording, this decoder's timestamps wander by up to six
        // milliseconds, so a tight tolerance treated ninety-six of them per
        // sixteen seconds as gaps to pad and a hundred and thirty-five as
        // overlaps to trim. Every one of those is a step in the waveform.
        //
        // Decoded audio is contiguous until something says otherwise, and the
        // things that say otherwise - a seek, an edit, a real hole in the file -
        // are all far larger than a tenth of a second. Anything smaller is the
        // clock jittering, and the right response to a jittering clock is to
        // ignore it, not to cut the audio to fit.
        const int64_t tolerance = kMixRate / 10;
        if (frame >= hi_ - tolerance && frame <= hi_ + tolerance) {
            frame = hi_;                       // rounding only: carry straight on
        } else if (frame > hi_ + kMixRate * 2) {
            lo_ = hi_ = frame;                 // a jump, not a gap worth filling
        } else if (frame > hi_) {
            gapFills++;
            gapSamples += frame - hi_;
            biggestGap = std::max(biggestGap, frame - hi_);
            fillLocked(hi_, frame - hi_);      // a real gap: silence holds the axis
        } else {
            const int64_t skip = hi_ - frame;  // a real overlap: trim the front
            overlaps++;
            if (skip >= frames) return;
            samples += skip * kMixChannels;
            frames -= skip;
            frame = hi_;
        }

        for (int64_t done = 0; done < frames;) {
            const int64_t at = (frame + done) % capacity;
            const int64_t run = std::min(frames - done, capacity - at);
            std::memcpy(&data_[static_cast<size_t>(at) * kMixChannels],
                        samples + done * kMixChannels,
                        static_cast<size_t>(run) * kMixChannels * sizeof(float));
            done += run;
        }
        hi_ = frame + frames;
        lo_ = std::max(lo_, hi_ - capacity);
    }

    // Adds `frames` starting at `frame` into `out`, scaled by `gain`.
    int64_t mixInto(int64_t frame, float* out, int64_t frames, float gain) {
        std::lock_guard<std::mutex> lock(mutex_);
        const int64_t capacity = static_cast<int64_t>(data_.size()) / kMixChannels;
        if (capacity <= 0 || frame < lo_) return 0;
        const int64_t have = std::min(frames, hi_ - frame);
        if (have <= 0) return 0;

        for (int64_t done = 0; done < have;) {
            const int64_t at = (frame + done) % capacity;
            const int64_t run = std::min(have - done, capacity - at);
            const float* in = &data_[static_cast<size_t>(at) * kMixChannels];
            float* dst = out + done * kMixChannels;
            for (int64_t i = 0; i < run * kMixChannels; ++i) dst[i] += in[i] * gain;
            done += run;
        }
        return have;
    }

    int64_t available(int64_t frame) {
        std::lock_guard<std::mutex> lock(mutex_);
        return hi_ - frame;
    }

private:
    void fillLocked(int64_t frame, int64_t frames) {
        const int64_t capacity = static_cast<int64_t>(data_.size()) / kMixChannels;
        frames = std::min(frames, capacity);
        for (int64_t done = 0; done < frames;) {
            const int64_t at = (frame + done) % capacity;
            const int64_t run = std::min(frames - done, capacity - at);
            std::memset(&data_[static_cast<size_t>(at) * kMixChannels], 0,
                        static_cast<size_t>(run) * kMixChannels * sizeof(float));
            done += run;
        }
    }

    std::mutex mutex_;
    std::vector<float> data_;
    int64_t lo_ = 0;
    int64_t hi_ = 0;

public:
    // How often the decoder handed back a block that did not follow the one
    // before it. Every one of these is a step in the waveform.
    int64_t gapFills = 0;
    int64_t gapSamples = 0;
    int64_t biggestGap = 0;
    int64_t overlaps = 0;
};

struct AudioEngine::Impl {
    // --- decoding -----------------------------------------------------------
    Microsoft::WRL::ComPtr<IMFSourceReader> reader;
    std::vector<DWORD> streamIndex;              // MF stream index per track
    std::vector<std::unique_ptr<TrackBuffer>> buffers;
    std::thread decodeThread;
    std::atomic<bool> decodeStop{false};
    std::atomic<int64_t> seekRequest{-1};        // frames, -1 when none
    std::atomic<bool> eof{false};

    // --- output -------------------------------------------------------------
    Microsoft::WRL::ComPtr<IAudioClient> client;
    Microsoft::WRL::ComPtr<IAudioRenderClient> render;
    HANDLE bufferEvent = nullptr;
    UINT32 bufferFrames = 0;
    WAVEFORMATEX* mixFormat = nullptr;
    std::thread renderThread;
    std::atomic<bool> renderStop{false};
    std::string outputName;
    std::string formatNote;
    std::vector<std::string> decodeNotes;

    // --- the clock ----------------------------------------------------------
    // Frames the mixer has handed to the device. Everything visible outside
    // this class is derived from it, which is what makes audio the master.
    std::atomic<int64_t> playPos{0};
    std::vector<float> mixBuffer;

    // Speed, and the scratch the mixer reads into when it is not 1.0. Both are
    // touched only by the render thread; a seek asks for a flush through the
    // flag rather than reaching in from another thread.
    SpeedChanger speedChanger;
    std::vector<float> speedSource;
    std::atomic<bool> flushSpeed{false};

    // One track's worth of audio on its way through the chain. Filtering has to
    // happen on a track before it joins the others, so each is gathered here,
    // shaped, and only then added to the mix.
    std::vector<float> trackBuffer;

    // Per track, and touched only by the render thread.
    std::vector<Equaliser> equalisers;
    std::vector<PitchShifter> shifters;
    std::vector<float> pitchSetting;
    std::vector<LoudnessMeter> meters;
    std::vector<float> eqSetting;     // last low/mid/high seen, three per track

    ~Impl() {
        if (mixFormat) CoTaskMemFree(mixFormat);
        if (bufferEvent) CloseHandle(bufferEvent);
    }
};

}  // namespace vp
