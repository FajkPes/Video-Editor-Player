// A bench for the speed changer on its own.
//
// It matters that this is measured rather than listened to once: the two paths
// fail in ways that are easy to miss by ear on a short clip. A resampler with
// an off-by-one in its history sounds almost right; a time stretcher that finds
// the wrong overlap sounds almost right too, and then buzzes on a held note.
//
// So: a pure tone in, and three questions out. Is the output the length it
// should be, is the energy still there, and is the pitch where it belongs.

#include "audio_stretch.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

// How much of the signal sits at `frequency`. One bin of a Fourier transform,
// which is all that is needed to ask about a tone whose frequency is known.
double energyAt(const std::vector<float>& mono, double frequency, double rate) {
    const double w = 2.0 * kPi * frequency / rate;
    double real = 0.0, imaginary = 0.0;
    for (size_t i = 0; i < mono.size(); ++i) {
        real += mono[i] * std::cos(w * i);
        imaginary += mono[i] * std::sin(w * i);
    }
    return std::sqrt(real * real + imaginary * imaginary) / mono.size();
}

double rms(const std::vector<float>& mono) {
    double sum = 0.0;
    for (float v : mono) sum += static_cast<double>(v) * v;
    return mono.empty() ? 0.0 : std::sqrt(sum / mono.size());
}

// The longest run of near-silence, in milliseconds. A stretcher that starves
// its own input shows up here and nowhere else.
double longestGapMs(const std::vector<float>& mono, double rate) {
    size_t run = 0, worst = 0;
    for (float v : mono) {
        if (std::fabs(v) < 1e-4f) {
            if (++run > worst) worst = run;
        } else {
            run = 0;
        }
    }
    return worst * 1000.0 / rate;
}

struct Result {
    double outSeconds = 0.0;
    double level = 0.0;
    double atTone = 0.0;
    double atShifted = 0.0;
    double gapMs = 0.0;
    double consumed = 0.0;
};

Result run(double speed, bool keepPitch, double tone, double seconds) {
    constexpr double rate = 48000.0;
    const int64_t inFrames = static_cast<int64_t>(rate * seconds);

    std::vector<float> input(static_cast<size_t>(inFrames) * 2);
    for (int64_t i = 0; i < inFrames; ++i) {
        const float v = static_cast<float>(0.5 * std::sin(2.0 * kPi * tone * i / rate));
        input[static_cast<size_t>(i) * 2] = v;
        input[static_cast<size_t>(i) * 2 + 1] = v;
    }

    vp::SpeedChanger changer;
    changer.configure(speed, keepPitch);

    std::vector<float> output;
    std::vector<float> block(1024 * 2);
    int64_t pushed = 0;

    // Driven the way the render thread drives it: ask for a block, top up the
    // input when it says it needs more, stop when the input runs out.
    while (true) {
        for (int guard = 0; guard < 64; ++guard) {
            const int64_t want = changer.wantInput(1024);
            if (want <= 0) break;
            const int64_t chunk = std::min<int64_t>({want, 8192, inFrames - pushed});
            if (chunk <= 0) break;
            changer.push(&input[static_cast<size_t>(pushed) * 2], chunk);
            pushed += chunk;
        }
        const int64_t made = changer.pull(block.data(), 1024);
        if (made <= 0) break;
        output.insert(output.end(), block.begin(), block.begin() + made * 2);
    }

    std::vector<float> mono(output.size() / 2);
    for (size_t i = 0; i < mono.size(); ++i) mono[i] = output[i * 2];

    Result r;
    r.outSeconds = mono.size() / rate;
    r.level = rms(mono);
    r.atTone = energyAt(mono, tone, rate);
    r.atShifted = energyAt(mono, tone * speed, rate);
    r.gapMs = longestGapMs(mono, rate);
    r.consumed = pushed / rate;
    return r;
}

void report(const char* what, double speed, bool keepPitch, double tone,
            double seconds) {
    const Result r = run(speed, keepPitch, tone, seconds);
    const double expected = seconds / speed;
    std::printf("%-28s speed %.2f  out %.2fs (cekano %.2fs, odchylka %+.1f %%)\n",
                what, speed, r.outSeconds, expected,
                (r.outSeconds / expected - 1.0) * 100.0);
    std::printf("%-28s rms %.4f (vstup 0.3536)   na %.0f Hz %.4f   na %.0f Hz %.4f"
                "   ticho %.1f ms   vstupu %.2fs\n\n",
                "", r.level, tone, r.atTone, tone * speed, r.atShifted, r.gapMs,
                r.consumed);
}

}  // namespace

int main() {
    std::printf("\n=== rychlost prehravani ===\n\n");

    // The guarantee that matters most: at 1.0 nothing happens at all.
    vp::SpeedChanger idle;
    idle.configure(1.0, true);
    std::printf("pri 1.0 je prepocet %s\n\n", idle.active() ? "ZAPNUTY (chyba)" : "vypnuty");

    report("drzi ton, rychleji", 1.5, true, 440.0, 6.0);
    report("drzi ton, pomaleji", 0.75, true, 440.0, 6.0);
    report("drzi ton, dvojnasobne", 2.0, true, 220.0, 6.0);
    report("jako pasek, rychleji", 1.5, false, 440.0, 6.0);
    report("jako pasek, pomaleji", 0.75, false, 440.0, 6.0);
    return 0;
}
