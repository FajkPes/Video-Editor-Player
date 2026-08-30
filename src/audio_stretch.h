#pragma once
// Playing faster or slower than the recording.
//
// Two ways to do it, because they are two different things:
//
//   * Resampling reads the samples closer together. Everything moves - a voice
//     at 1.5x sounds a fifth higher. This is what a tape does, and it is the
//     right answer when somebody wants that.
//
//   * Time stretching keeps the pitch. The signal is cut into overlapping
//     pieces and the pieces are laid down closer together, each one slid to the
//     spot where it lines up best with what is already there. That last part is
//     the whole trick: dropping pieces on a fixed grid puts waveform cycles out
//     of phase with each other and the result buzzes.
//
// Neither runs at all at 1.0. That is deliberate and it is the important
// guarantee here: ordinary playback goes through none of this code, so nothing
// below can affect the sound at normal speed.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace vp {

// Interleaved stereo, addressed from the front, dropped from the front.
class SampleFifo {
public:
    void clear() {
        data_.clear();
        read_ = 0;
    }

    int64_t size() const {
        return static_cast<int64_t>(data_.size() / 2) - static_cast<int64_t>(read_);
    }

    const float* at(int64_t frame) const { return &data_[(read_ + frame) * 2]; }

    void push(const float* samples, int64_t frames) {
        data_.insert(data_.end(), samples, samples + frames * 2);
    }

    void drop(int64_t frames) {
        read_ += static_cast<size_t>(frames);
        // Compacted rather than erased every time: erasing from the front of a
        // vector moves everything after it, and this is called on the render
        // thread hundreds of times a second.
        if (read_ > 65536) {
            data_.erase(data_.begin(), data_.begin() + static_cast<long>(read_) * 2);
            read_ = 0;
        }
    }

    void take(float* out, int64_t frames) {
        std::memcpy(out, at(0), static_cast<size_t>(frames) * 2 * sizeof(float));
        drop(frames);
    }

private:
    std::vector<float> data_;
    size_t read_ = 0;
};

class SpeedChanger {
public:
    // 60 ms of output per piece, a fifth of it overlapped, and 14 ms of slack to
    // slide each piece in. These are the figures the established implementations
    // settled on: shorter pieces smear transients, longer ones make a voice
    // warble, and a search window under about ten milliseconds is not wide
    // enough to find the matching cycle of a low note.
    static constexpr int64_t kStride = 48000 * 60 / 1000;
    static constexpr int64_t kOverlap = 48000 * 12 / 1000;
    static constexpr int64_t kSearch = 48000 * 14 / 1000;

    void configure(double speed, bool keepPitch) {
        const bool wasActive = active_;
        speed_ = speed;
        keepPitch_ = keepPitch;
        active_ = std::fabs(speed - 1.0) > 1e-6;
        if (!active_ && wasActive) reset();
        if (active_ && !wasActive) reset();
    }

    bool active() const { return active_; }

    void reset() {
        in_.clear();
        out_.clear();
        overlap_.assign(static_cast<size_t>(kOverlap) * 2, 0.0f);
        haveOverlap_ = false;
        position_ = 1.0;
        carry_ = 0.0;
        pendingDrop_ = 0;
        primed_ = false;
    }

    // Input frames still missing before another piece of output can be made.
    // Zero means there is already enough, or that nothing more would help.
    int64_t wantInput(int64_t outFrames) const {
        if (out_.size() >= outFrames) return 0;
        if (keepPitch_) {
            // The window one piece is built from, plus anything still owed from
            // the last advance. Leaving the debt out is what made high speeds
            // quietly play too slowly: the input to be skipped had not arrived
            // yet, the skip was silently shortened, and the ratio came out at
            // about 1.43 whatever was asked for.
            return std::max<int64_t>(
                0, pendingDrop_ + kSearch + kStride + kOverlap - in_.size());
        }
        const int64_t needed =
            static_cast<int64_t>(std::floor(position_ + outFrames * speed_)) + 3;
        return std::max<int64_t>(0, needed - in_.size());
    }

    void push(const float* samples, int64_t frames) {
        in_.push(samples, frames);
        payDebt();
    }

    // Fills up to `frames` and returns how many it managed.
    int64_t pull(float* output, int64_t frames) {
        if (keepPitch_) {
            while (out_.size() < frames && makeStretchedPiece()) {
            }
        } else {
            resampleInto(frames);
        }
        const int64_t made = std::min(frames, out_.size());
        if (made > 0) out_.take(output, made);
        return made;
    }

private:
    // ---------------------------------------------------------- resampling
    void resampleInto(int64_t frames) {
        // Cubic through four points. Linear interpolation is a low-pass filter
        // that changes with the fractional position, which at a constant speed
        // becomes a slow wobble in the top octave.
        while (out_.size() < frames) {
            const int64_t k = static_cast<int64_t>(position_);
            if (in_.size() < k + 3) break;

            const double t = position_ - k;
            float frame[2];
            for (int c = 0; c < 2; ++c) {
                const float y0 = in_.at(k - 1)[c];
                const float y1 = in_.at(k)[c];
                const float y2 = in_.at(k + 1)[c];
                const float y3 = in_.at(k + 2)[c];
                const double a = -0.5 * y0 + 1.5 * y1 - 1.5 * y2 + 0.5 * y3;
                const double b = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3;
                const double c1 = -0.5 * y0 + 0.5 * y2;
                frame[c] = static_cast<float>(((a * t + b) * t + c1) * t + y1);
            }
            out_.push(frame, 1);
            position_ += speed_;

            // One frame of history is kept, because the cubic reaches back.
            const int64_t consumed = static_cast<int64_t>(position_) - 1;
            if (consumed > 0) {
                in_.drop(consumed);
                position_ -= consumed;
            }
        }
    }

    // ------------------------------------------------------- time stretching
    bool makeStretchedPiece() {
        payDebt();
        if (pendingDrop_ > 0) return false;   // still skipping past old input
        if (in_.size() < kSearch + kStride + kOverlap) return false;

        // The first piece has nothing to line up against, so it is simply taken.
        if (!primed_) {
            out_.push(in_.at(0), kStride);
            std::memcpy(overlap_.data(), in_.at(kStride),
                        static_cast<size_t>(kOverlap) * 2 * sizeof(float));
            haveOverlap_ = true;
            primed_ = true;
            advance();
            return true;
        }

        const int64_t best = bestOffset();

        // The join: the tail that was already played fades out while the new
        // piece fades in. Linear, and that is not the usual choice - an
        // equal-power curve is right when two signals are unrelated, and these
        // are related on purpose. The search above lines them up, so through
        // the middle of the join they reinforce each other, and equal power
        // measured half a decibel of gain that should not be there.
        for (int64_t i = 0; i < kOverlap; ++i) {
            const double t = static_cast<double>(i) / kOverlap;
            const double fadeIn = t;
            const double fadeOut = 1.0 - t;
            const float* fresh = in_.at(best + i);
            float frame[2];
            for (int c = 0; c < 2; ++c) {
                frame[c] = static_cast<float>(overlap_[i * 2 + c] * fadeOut +
                                              fresh[c] * fadeIn);
            }
            out_.push(frame, 1);
        }
        out_.push(in_.at(best + kOverlap), kStride - kOverlap);

        std::memcpy(overlap_.data(), in_.at(best + kStride),
                    static_cast<size_t>(kOverlap) * 2 * sizeof(float));
        advance();
        return true;
    }

    // Where in the search window the incoming audio best continues what was
    // just played. Plain cross-correlation on the sum of the two channels:
    // normalising it costs a square root per position and moves the answer by
    // less than the overlap is long.
    int64_t bestOffset() const {
        if (!haveOverlap_) return 0;

        int64_t best = 0;
        double bestScore = -1e30;

        // Two passes: every eighth position first, then the neighbourhood of the
        // winner. Sixty-odd correlations instead of six hundred and change, and
        // the coarse pass cannot miss a peak because a correlation peak at this
        // scale is many samples wide.
        for (int stage = 0; stage < 2; ++stage) {
            const int64_t step = stage == 0 ? 8 : 1;
            const int64_t from = stage == 0 ? 0 : std::max<int64_t>(0, best - 8);
            const int64_t to = stage == 0 ? kSearch : std::min(kSearch, best + 8);

            for (int64_t off = from; off <= to; off += step) {
                double score = 0.0;
                for (int64_t i = 0; i < kOverlap; i += 2) {
                    const float* a = in_.at(off + i);
                    score += (overlap_[i * 2] + overlap_[i * 2 + 1]) *
                             (static_cast<double>(a[0]) + a[1]);
                }
                if (score > bestScore) {
                    bestScore = score;
                    best = off;
                }
            }
            bestScore = -1e30;   // the fine pass re-scores its own range
            if (stage == 0) {
                // Re-score the coarse winner so the fine pass has something to
                // beat rather than inheriting a score from a different step.
                double score = 0.0;
                for (int64_t i = 0; i < kOverlap; i += 2) {
                    const float* a = in_.at(best + i);
                    score += (overlap_[i * 2] + overlap_[i * 2 + 1]) *
                             (static_cast<double>(a[0]) + a[1]);
                }
                bestScore = score;
            }
        }
        return best;
    }

    void advance() {
        // The input moves by a whole stride times the factor; the output moved
        // by exactly one stride. That ratio is the speed change.
        //
        // At speeds above about 1.4 a whole advance is longer than the window
        // the buffer holds, so the skip cannot always be paid at once. What is
        // left over is remembered and taken out of whatever arrives next, which
        // is what keeps the ratio exact instead of drifting towards whatever
        // the buffer happened to hold.
        carry_ += kStride * speed_;
        const int64_t whole = static_cast<int64_t>(carry_);
        carry_ -= whole;
        pendingDrop_ += whole;
        payDebt();
    }

    void payDebt() {
        const int64_t now = std::min(pendingDrop_, in_.size());
        if (now <= 0) return;
        in_.drop(now);
        pendingDrop_ -= now;
    }

    SampleFifo in_, out_;
    std::vector<float> overlap_;
    double speed_ = 1.0;
    double position_ = 1.0;
    double carry_ = 0.0;
    int64_t pendingDrop_ = 0;
    bool keepPitch_ = true;
    bool active_ = false;
    bool haveOverlap_ = false;
    bool primed_ = false;
};

}  // namespace vp
