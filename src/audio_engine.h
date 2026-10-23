#pragma once
//
// Audio engine: decode every audio track of a file, mix them, play them.
//
// The shape of this is deliberately the one every working player uses, and it
// differs from the usual first attempt in three ways that matter:
//
//   1. The clock is the audio. Position is the number of frames the device has
//      actually consumed, and video is expected to follow it. Making audio
//      chase the video means resampling it continuously, and continuous
//      resampling of a signal is audible in a way that a repeated video frame
//      never is.
//
//   2. Nothing between the decoder and the device runs in a language with a
//      garbage collector or a global lock. The render callback is a plain
//      thread that owns its buffers, registered with MMCSS so the scheduler
//      knows it has a deadline.
//
//   3. Every track is decoded to one common format up front. Mixing is then a
//      sum of float arrays, with no per-sample branching on rate or layout.
//
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vp {
struct Segment;

// The format everything is converted to on the way in. Chosen once, so nothing
// downstream ever has to ask what it is holding.
constexpr int kMixRate = 48000;
constexpr int kMixChannels = 2;

struct TrackInfo {
    int index = 0;             // stream index inside the file

    // Which audio stream of the file this is, counted among audio streams only
    // and including any this machine cannot decode. It is what ffmpeg means by
    // 0:a:N, so the waveform under a row, the loudness figure beside it and the
    // stream the export reads all belong to the row they are drawn on. Counting
    // our own list instead put them one apart whenever a stream was skipped.
    int audioIndex = 0;
    std::string name;          // language or title, for the row in the UI
    int sourceRate = 0;
    int sourceChannels = 0;
};

// One track's live controls. Read by the mixer thread every block, so they are
// atomic rather than lock-protected: a torn read of a gain is inaudible, a
// blocked mixer thread is not.
struct TrackControls {
    std::atomic<float> gain{1.0f};
    std::atomic<bool> enabled{true};
    std::atomic<bool> solo{false};
    std::atomic<float> peak{0.0f};      // most recent block peak, for a meter

    // Whether the normaliser is allowed to touch this track. The master switch
    // says whether normalising happens at all; this says which tracks it
    // happens to, so a commentary track can be levelled while the music under
    // it is left at the mix the film was made with.
    // Off to begin with. It used to start on, so switching levelling on for
    // one track switched it on for every track at once - the master switch was
    // the only thing that had been off.
    std::atomic<bool> normalise{false};

    // Levelling cannot answer until it has heard a second of the track, and a
    // switch that appears to do nothing for a second is a switch people press
    // twice. This says whether the measurement has settled, so the row can say
    // so instead of sitting there.
    std::atomic<bool> levelReady{false};

    // The row's fades, in seconds of the file. The mixer reads them per sample
    // rather than the window pushing a gain every so often, because a gain that
    // moves in steps of a frame of video is a gain that is heard stepping.
    std::atomic<float> fadeInAt{0.0f};
    std::atomic<float> fadeInLen{0.0f};
    std::atomic<float> fadeOutAt{0.0f};
    std::atomic<float> fadeOutLen{0.0f};
    std::atomic<bool> fadeSmooth{false};

    // Three bands, in dB. Read by the mixer every block; the filter itself is
    // rebuilt only when one of these actually moves, because rebuilding
    // coefficients per block would cost more than the filtering does.
    std::atomic<float> eqLow{0.0f};
    std::atomic<float> eqMid{0.0f};
    std::atomic<float> eqHigh{0.0f};

    // Semitones, this track only. Moving one track against the others is
    // what this is for - a commentary that sits under the music, a song a
    // tone off from the rest of a mix - so it belongs beside the bands
    // rather than beside the speed, which moves everything at once.
    std::atomic<float> pitch{0.0f};

    // What the piece of the edit under the playhead adds on top. The editor
    // writes these as the playhead crosses from one piece to the next; the
    // controls above are the listener's own and are never touched by it, so
    // a clip setting and a fader setting can both be true at once.
    std::atomic<float> clipGain{1.0f};
    std::atomic<float> clipEqLow{0.0f};
    std::atomic<float> clipEqMid{0.0f};
    std::atomic<float> clipEqHigh{0.0f};
    std::atomic<float> clipPitch{0.0f};
    std::atomic<float> normaliseReference{-70.0f}; // completed source loudness measurement
    std::atomic<bool> clipSilent{false};

    // What the normaliser is doing to this track right now, in dB, so the
    // interface can show it rather than leaving the listener to guess why a
    // fader at 100 % is not as loud as the one below it.
    std::atomic<float> normalisationDb{0.0f};
    std::atomic<float> measuredDb{-70.0f};
};

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // Opens a file and starts the decoder. Returns false and fills `error`
    // when the file has no audio this machine can decode.
    bool open(const std::wstring& path, std::string* error);
    void close();

    const std::vector<TrackInfo>& tracks() const { return tracks_; }

    // Which stream of the file a track really is. Media Foundation does not
    // always hand the streams over in the order the file holds them, and when
    // it does not, everything that reaches the file through ffmpeg - the wave
    // under the row, the loudness beside it, the export - would otherwise be
    // reading a different track from the one the row plays.
    // Puts the tracks in the file's own order. `order[k]` is the track that
    // should end up in place k. Everything that belongs to a track travels
    // with it - its buffer, its controls, its filters and which stream of the
    // file feeds it - so nothing downstream has to know this happened.
    void reorderTracks(const std::vector<size_t>& order);

    // Which stream of the file feeds this row. The name is not touched: a row
    // is called after its place in the list, and the stream it reads is a fact
    // about the file that is shown separately - the two were being conflated,
    // which is how "A1" ended up next to "Track 2".
    void setAudioIndex(size_t track, int audioIndex) {
        if (track >= tracks_.size()) return;
        tracks_[track].audioIndex = audioIndex;
    }

    // Swaps which streams two rows read. Nothing about the sound changes -
    // Media Foundation goes on decoding what it decodes - but the wave under
    // the row, the loudness beside it and the stream the export reads follow
    // the row they belong to.
    void swapAudioIndex(size_t a, size_t b) {
        if (a >= tracks_.size() || b >= tracks_.size() || a == b) return;
        std::swap(tracks_[a].audioIndex, tracks_[b].audioIndex);
    }
    TrackControls& controls(size_t i) { return *controls_[i]; }

    void play();
    void pause();
    bool playing() const { return playing_.load(std::memory_order_relaxed); }

    // Seconds. `seek` is exact to the frame once the decoder has caught up.
    double position() const;
    double duration() const { return duration_; }
    void seek(double seconds);

    // Playing faster or slower. 1.0 bypasses the whole mechanism, so ordinary
    // playback never goes near it. `keepPitch` chooses between stretching the
    // time (a voice keeps its pitch) and resampling (a voice goes up, the way a
    // tape does).
    void setSpeed(double s);
    void setClipEdits(const std::vector<Segment>& segments);
    double speed() const { return speed_.load(std::memory_order_relaxed); }
    void setKeepPitch(bool on);
    bool keepPitch() const { return keepPitch_.load(std::memory_order_relaxed); }

    void setMasterGain(float g) { master_.store(g, std::memory_order_relaxed); }
    float masterGain() const { return master_.load(std::memory_order_relaxed); }

    // Bring every track towards the same loudness. The target is in dB on the
    // same scale the meter reports, so -14 is the figure streaming services
    // publish and the one the old version offered.
    void setNormalise(bool on) { normalise_.store(on, std::memory_order_relaxed); }
    bool normalising() const { return normalise_.load(std::memory_order_relaxed); }
    void setNormaliseTarget(float db) { normaliseTarget_.store(db, std::memory_order_relaxed); }
    float normaliseTarget() const { return normaliseTarget_.load(std::memory_order_relaxed); }

    // True while any track is soloed, in which case the others are silent
    // whatever their own switch says.
    bool anySolo() const;

    // How far ahead of the speakers the mixer is, in seconds. The video side
    // needs this to place a frame against a sample that has not been heard yet.
    double outputLatency() const { return outputLatency_; }

    // Diagnostics that a person can act on. `dropouts` counts only the blocks
    // the device asked for and nobody could fill - the single number that
    // separates "the computer cannot keep up" from every other complaint.
    uint64_t dropouts() const { return dropouts_.load(std::memory_order_relaxed); }

    // Every mixed block, exactly as the mixer produced it and before anything
    // downstream sees it. For proving what the engine itself makes, separately
    // from what the device and the driver do with it afterwards.
    void setMixTap(void (*fn)(const float*, int64_t, void*), void* user) {
        mixTap_ = fn;
        mixTapUser_ = user;
    }
    std::string outputDescription() const;
    std::string outputDeviceName() const;
    std::vector<std::string> decodeDescriptions() const;
    std::string continuityReport() const;

private:
    friend struct AudioSemanticProbe;
    bool startOutput(std::string* error);
    void decodeLoop();
    void renderLoop();
    // Returns how many frames it actually produced: zero when a track has not
    // been decoded far enough yet and the clock has to wait.
    int64_t mixBlock(float* out, int64_t frames);
    int64_t mixChunk(float* out, int64_t frames, const Segment* segment, bool scheduled);

    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::vector<TrackInfo> tracks_;
    std::vector<std::unique_ptr<TrackControls>> controls_;

    std::atomic<bool> playing_{false};
    std::atomic<float> master_{1.0f};
    std::atomic<uint64_t> dropouts_{0};
    std::atomic<bool> normalise_{false};
    std::atomic<double> speed_{1.0};
    std::atomic<bool> keepPitch_{true};
    std::atomic<float> normaliseTarget_{-14.0f};
    double duration_ = 0.0;
    void (*mixTap_)(const float*, int64_t, void*) = nullptr;
    void* mixTapUser_ = nullptr;
    double outputLatency_ = 0.0;
};

}  // namespace vp
