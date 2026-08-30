#pragma once
// Everything that shells out to ffmpeg.
//
// The editor needs three things the player itself does not: a loudness figure
// per track, an envelope to draw, and a strip of thumbnails. All three are
// cheapest as a pipe from ffmpeg, and all three have to happen without the
// mixer noticing - so each runs on its own thread at below-normal priority and
// hands the result back through an atomic flag the window polls.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace vp {

// Full path to ffmpeg.exe, looked for beside the executable first and then on
// PATH. Empty when it is nowhere, which disables the render button and the
// preview builders rather than failing later with something cryptic.
const std::wstring& ffmpegPath();
const std::wstring& ffprobePath();

// Runs a command with no console window and returns its exit code. `output`
// collects both streams; pass null to discard them.
int runHidden(const std::wstring& commandLine, std::string* output, DWORD timeoutMs);

std::wstring quoteArgument(const std::wstring& s);

// ---------------------------------------------------------------- waveforms

// One track's envelope: peak magnitude per bucket, 0..1, evenly spaced across
// the whole file. Drawn as a filled shape either side of a centre line.
struct Waveform {
    std::vector<float> peaks;
    std::vector<float> rms;
};

class WaveformJob {
public:
    ~WaveformJob() { cancel(); }

    // `trackCount` streams of the file, `duration` seconds, `buckets` columns.
    // `streams` is one ffmpeg audio-stream number per row, in row order.
    void start(const std::wstring& path, const std::vector<int>& streams,
               double duration, int buckets);
    void cancel();

    bool running() const { return running_.load(); }
    bool done() const { return done_.load(); }
    int progress() const { return progress_.load(); }   // 0..100

    // Valid once `done`. Indexed the same way the tracks are.
    const std::vector<Waveform>& result() const { return result_; }

private:
    std::thread worker_;
    std::atomic<bool> stop_{false}, running_{false}, done_{false};
    std::atomic<int> progress_{0};
    std::vector<Waveform> result_;
};

// ---------------------------------------------------------------- thumbnails

// Fixed size, so every frame arrives as exactly the same number of bytes and
// the strip needs no per-image bookkeeping.
constexpr int kThumbWidth = 96;
constexpr int kThumbHeight = 54;

struct Thumb {
    double at = 0.0;
    HBITMAP bitmap = nullptr;
};

class ThumbJob {
public:
    ~ThumbJob() { cancel(); }

    void start(const std::wstring& path, double duration, int count);
    void cancel();
    void clear();

    bool running() const { return running_.load(); }
    bool done() const { return done_.load(); }
    int progress() const { return progress_.load(); }

    // Safe to read while the job runs: frames are appended under a lock and the
    // strip simply draws fewer of them until the rest arrive.
    std::vector<Thumb> taken() const;

private:
    std::thread worker_;
    mutable CRITICAL_SECTION lock_{};
    bool lockReady_ = false;
    std::atomic<bool> stop_{false}, running_{false}, done_{false};
    std::atomic<int> progress_{0};
    std::vector<Thumb> thumbs_;
};

// ---------------------------------------------------------------- loudness

// EBU R128 integrated loudness per track, which is what the render needs to
// turn normalisation into a fixed gain rather than a second pass.
class LoudnessJob {
public:
    ~LoudnessJob() { cancel(); }

    void start(const std::wstring& path, const std::vector<int>& streams);
    void cancel();

    bool running() const { return running_.load(); }
    bool done() const { return done_.load(); }

    // -70 where it could not be measured.
    const std::vector<float>& result() const { return result_; }

private:
    std::thread worker_;
    std::atomic<bool> stop_{false}, running_{false}, done_{false};
    std::vector<float> result_;
};

}  // namespace vp
