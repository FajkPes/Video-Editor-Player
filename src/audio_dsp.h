#pragma once
// Filters for the mixer path.
//
// Two things live here: a biquad, which is the standard second-order section
// every equaliser is built from, and a loudness meter, which is what tells the
// normaliser how much to turn a track down.
//
// Both are written to be cheap enough to run on the render thread. Nothing here
// allocates, branches on data, or calls anything that could block.

#include <algorithm>
#include <cmath>
#include <vector>

namespace vp {

constexpr double kPi = 3.14159265358979323846;

// A peaking equaliser section. Flat at 0 dB, and then it is a straight copy:
// the coefficients are checked once and the whole filter is skipped, so a band
// nobody has touched costs nothing at all.
class Biquad {
public:
    void setPeaking(double sampleRate, double frequency, double q, double gainDb) {
        gainDb_ = gainDb;
        bypass_ = std::fabs(gainDb) < 0.01;
        if (bypass_) return;

        const double a = std::pow(10.0, gainDb / 40.0);
        const double w = 2.0 * kPi * frequency / sampleRate;
        const double alpha = std::sin(w) / (2.0 * q);
        const double cosw = std::cos(w);

        const double b0 = 1.0 + alpha * a;
        const double b1 = -2.0 * cosw;
        const double b2 = 1.0 - alpha * a;
        const double a0 = 1.0 + alpha / a;
        const double a1 = -2.0 * cosw;
        const double a2 = 1.0 - alpha / a;

        b0_ = static_cast<float>(b0 / a0);
        b1_ = static_cast<float>(b1 / a0);
        b2_ = static_cast<float>(b2 / a0);
        a1_ = static_cast<float>(a1 / a0);
        a2_ = static_cast<float>(a2 / a0);
    }

    void reset() { z1_[0] = z1_[1] = z2_[0] = z2_[1] = 0.0f; }

    // Interleaved stereo, in place.
    void run(float* samples, long long frames) {
        if (bypass_) return;
        for (long long i = 0; i < frames; ++i) {
            for (int c = 0; c < 2; ++c) {
                const float in = samples[i * 2 + c];
                const float out = b0_ * in + z1_[c];
                z1_[c] = b1_ * in - a1_ * out + z2_[c];
                z2_[c] = b2_ * in - a2_ * out;
                samples[i * 2 + c] = out;
            }
        }
    }

    bool bypassed() const { return bypass_; }
    double gainDb() const { return gainDb_; }

private:
    float b0_ = 1.0f, b1_ = 0.0f, b2_ = 0.0f, a1_ = 0.0f, a2_ = 0.0f;
    float z1_[2]{}, z2_[2]{};
    double gainDb_ = 0.0;
    bool bypass_ = true;
};

// Three bands, at the frequencies a listener actually reaches for.
struct Equaliser {
    Biquad low, mid, high;

    void configure(double sampleRate, double lowDb, double midDb, double highDb) {
        low.setPeaking(sampleRate, 120.0, 0.7, lowDb);
        mid.setPeaking(sampleRate, 1200.0, 0.9, midDb);
        high.setPeaking(sampleRate, 8000.0, 0.7, highDb);
    }
    void reset() { low.reset(); mid.reset(); high.reset(); }
    void run(float* samples, long long frames) {
        low.run(samples, frames);
        mid.run(samples, frames);
        high.run(samples, frames);
    }
    bool flat() const { return low.bypassed() && mid.bypassed() && high.bypassed(); }
};

// Pitch, per track, without changing how long anything lasts.
//
// A track is moved up or down in semitones by reading its own recent history
// back at a different rate. One read pointer would run away from the write
// pointer and eventually jump; two, half a window apart and crossfaded, hand
// over to each other before either can, and the seam lands where the other tap
// is at full weight. That is the classic delay-line shifter, and it is the only
// kind cheap enough to run per track on the render thread: a few adds and one
// linear interpolation per sample, no transforms and no allocation once it is
// set up.
//
// It is not transparent on a full mix - a large shift smears transients, as it
// does in every hardware unit built this way - but on the range this is for,
// nudging a voice or an instrument a few semitones against the rest, it does
// the job and costs almost nothing.
class PitchShifter {
public:
    // `semitones` is what the listener asked for; zero switches the whole thing
    // off, so a track nobody has touched is a straight copy.
    void configure(double sampleRate, int channels, float semitones) {
        const bool wanted = std::fabs(semitones) > 0.01f;
        if (!wanted) {
            active_ = false;
            return;
        }
        if (!active_ || channels != channels_ || sampleRate != rate_) {
            rate_ = sampleRate;
            channels_ = std::max(1, channels);
            // The window is the whole travel of the read head, and the two taps
            // sit half of it apart. 46 ms is the usual compromise: shorter and
            // the crossfade flutters on held notes, longer and a transient is
            // heard twice.
            window_ = static_cast<long long>(rate_ * 0.046);
            history_.assign(static_cast<size_t>((window_ + 8) * channels_), 0.0f);
            written_ = 0;
            phase_ = 0.0;
            active_ = true;
        }
        ratio_ = std::pow(2.0, semitones / 12.0);
    }

    bool active() const { return active_; }

    void reset() {
        std::fill(history_.begin(), history_.end(), 0.0f);
        written_ = 0;
        phase_ = 0.0;
    }

    // In place: the block that came in is the block that goes out, which is
    // what lets this sit in the mixer between the equaliser and the fader
    // without any of the buffering a time stretcher would need. The speed of
    // the track is untouched - only the read head moves at a different rate,
    // and it is wrapped around a window so it never runs away from the write
    // head.
    void run(float* samples, long long frames) {
        if (!active_ || history_.empty()) return;
        const long long span = window_ + 8;
        const double travel = static_cast<double>(window_);
        // Higher pitch means reading faster than writing, which means the delay
        // has to shrink: the phase walks backwards. This had the wrong sign,
        // which is why asking for a higher note gave a lower one.
        const double step = (1.0 - ratio_) / travel;

        for (long long i = 0; i < frames; ++i) {
            for (int c = 0; c < channels_; ++c) {
                history_[static_cast<size_t>((written_ % span) * channels_ + c)] =
                    samples[i * channels_ + c];
            }
            ++written_;

            // Two taps half a window apart, each faded out as it passes the
            // write head, so the jump one of them has to make is never heard.
            const double pa = phase_;
            const double pb = phase_ < 0.5 ? phase_ + 0.5 : phase_ - 0.5;
            const double ga = 0.5 - 0.5 * std::cos(2.0 * kPi * pa);
            const double gb = 1.0 - ga;
            // Two samples of clearance, so interpolation never reaches a slot
            // this block has not written yet.
            const double da = 2.0 + pa * travel;
            const double db = 2.0 + pb * travel;

            for (int c = 0; c < channels_; ++c) {
                const float first = tap(da, c, span);
                const float second = tap(db, c, span);
                samples[i * channels_ + c] =
                    static_cast<float>(first * ga + second * gb);
            }

            phase_ += step;
            while (phase_ >= 1.0) phase_ -= 1.0;
            while (phase_ < 0.0) phase_ += 1.0;
        }
    }

private:
    // `back` samples behind the write head, read with a cubic through four
    // points rather than a straight line between two: linear interpolation on
    // a signal this is already resampling costs audible top end.
    float tap(double back, int channel, long long span) const {
        const double at = static_cast<double>(written_) - back;
        const long long base = static_cast<long long>(std::floor(at));
        const float f = static_cast<float>(at - base);
        const float p0 = slot(base - 1, channel, span);
        const float p1 = slot(base, channel, span);
        const float p2 = slot(base + 1, channel, span);
        const float p3 = slot(base + 2, channel, span);
        // Catmull-Rom.
        const float a = -0.5f * p0 + 1.5f * p1 - 1.5f * p2 + 0.5f * p3;
        const float b = p0 - 2.5f * p1 + 2.0f * p2 - 0.5f * p3;
        const float c = -0.5f * p0 + 0.5f * p2;
        return ((a * f + b) * f + c) * f + p1;
    }

    float slot(long long index, int channel, long long span) const {
        long long where = index % span;
        if (where < 0) where += span;
        return history_[static_cast<size_t>(where * channels_ + channel)];
    }

    std::vector<float> history_;
    double rate_ = 48000.0;
    double ratio_ = 1.0;
    double phase_ = 0.0;
    long long window_ = 2208;
    long long written_ = 0;
    int channels_ = 2;
    bool active_ = false;
};

// How loud a track has been, as a slow average of its energy.
//
// Not a certified loudness meter: it is a mean square with a long memory,
// expressed on the same scale, which is what a normaliser needs. A real LUFS
// measurement would add a K-weighting filter and gating; both matter for
// publishing a number and neither changes which way a fader has to move.
class LoudnessMeter {
public:
    void reset() { mean_ = 0.0; seen_ = 0; }

    void add(const float* samples, long long frames) {
        if (frames <= 0) return;
        double sum = 0.0;
        for (long long i = 0; i < frames * 2; ++i) {
            sum += static_cast<double>(samples[i]) * samples[i];
        }
        const double block = sum / (frames * 2);

        // Roughly a three-second memory at 48 kHz, so a quiet passage does not
        // send the fader chasing it.
        const double weight = std::min(1.0, frames / (48000.0 * 3.0));
        mean_ += weight * (block - mean_);
        seen_ += frames;
    }

    bool ready() const { return seen_ > 48000; }   // a second before it is trusted

    double levelDb() const {
        if (mean_ <= 1e-12) return -70.0;
        return 10.0 * std::log10(mean_);
    }

private:
    double mean_ = 0.0;
    long long seen_ = 0;
};

}  // namespace vp
