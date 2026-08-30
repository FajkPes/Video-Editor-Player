#include "media_tools.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "settings.h"

namespace vp {

namespace {

std::wstring beside(const wchar_t* name) {
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir.resize(slash + 1);

    // Beside the executable, then one folder up, then the folder the old Python
    // version lives in - a person who copies this next to the original should
    // not have to copy a hundred megabytes of ffmpeg with it.
    const std::wstring candidates[] = {dir + name, dir + L"..\\" + name,
                                       dir + L"..\\..\\Video Player\\" + name};
    for (const std::wstring& c : candidates) {
        if (GetFileAttributesW(c.c_str()) != INVALID_FILE_ATTRIBUTES) return c;
    }

    wchar_t found[MAX_PATH]{};
    if (SearchPathW(nullptr, name, nullptr, MAX_PATH, found, nullptr)) return found;
    return {};
}

}  // namespace

const std::wstring& ffmpegPath() {
    static const std::wstring path = beside(L"ffmpeg.exe");
    return path;
}

const std::wstring& ffprobePath() {
    static const std::wstring path = beside(L"ffprobe.exe");
    return path;
}

std::wstring quoteArgument(const std::wstring& s) {
    return L"\"" + s + L"\"";
}

int runHidden(const std::wstring& commandLine, std::string* output, DWORD timeoutMs) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 1 << 16)) return -1;
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = writeEnd;
    si.hStdError = writeEnd;

    std::wstring mutableLine = commandLine;
    PROCESS_INFORMATION pi{};
    const BOOL started =
        CreateProcessW(nullptr, &mutableLine[0], nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr,
                       &si, &pi);
    CloseHandle(writeEnd);
    if (!started) {
        CloseHandle(readEnd);
        return -1;
    }

    // Read while it runs. Waiting first and reading after deadlocks the moment
    // the child writes more than the pipe holds, which ffmpeg does immediately.
    std::string collected;
    char buffer[4096];
    DWORD got = 0;
    while (ReadFile(readEnd, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
        if (output) collected.append(buffer, got);
    }
    CloseHandle(readEnd);

    WaitForSingleObject(pi.hProcess, timeoutMs);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    if (code == STILL_ACTIVE) {
        TerminateProcess(pi.hProcess, 1);
        code = 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (output) *output = std::move(collected);
    return static_cast<int>(code);
}

namespace {

// Opens a child process and hands back the read end of its output, so a caller
// can consume a stream as it is produced instead of buffering all of it.
struct Pipe {
    HANDLE read = nullptr;
    HANDLE process = nullptr;

    bool open(const std::wstring& commandLine) {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE writeEnd = nullptr;
        if (!CreatePipe(&read, &writeEnd, &sa, 1 << 18)) return false;
        SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        si.hStdOutput = writeEnd;
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

        std::wstring mutableLine = commandLine;
        PROCESS_INFORMATION pi{};
        const BOOL ok = CreateProcessW(nullptr, &mutableLine[0], nullptr, nullptr, TRUE,
                                       CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                                       nullptr, nullptr, &si, &pi);
        CloseHandle(writeEnd);
        if (!ok) {
            CloseHandle(read);
            read = nullptr;
            return false;
        }
        CloseHandle(pi.hThread);
        process = pi.hProcess;
        return true;
    }

    void close() {
        if (read) { CloseHandle(read); read = nullptr; }
        if (process) {
            if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
                TerminateProcess(process, 1);
            }
            CloseHandle(process);
            process = nullptr;
        }
    }
};

}  // namespace

// ---------------------------------------------------------------- waveforms

void WaveformJob::start(const std::wstring& path, const std::vector<int>& streams,
                        double duration, int buckets) {
    cancel();
    const int trackCount = static_cast<int>(streams.size());
    if (ffmpegPath().empty() || trackCount <= 0 || duration <= 0.0) return;

    stop_.store(false);
    done_.store(false);
    running_.store(true);
    progress_.store(0);
    result_.assign(static_cast<size_t>(trackCount), Waveform{});

    worker_ = std::thread([this, path, streams, trackCount, duration, buckets] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

        // 4 kHz mono is plenty for an envelope: a bucket on screen covers tens
        // of milliseconds, so the peak inside it is the same whether it was
        // found among four thousand samples per second or forty-eight thousand,
        // and this way an hour-long file is a few megabytes of pipe rather than
        // a hundred.
        constexpr int kProbeRate = 4000;

        for (int track = 0; track < trackCount && !stop_.load(); ++track) {
            Waveform wave;
            wave.peaks.assign(static_cast<size_t>(buckets), 0.0f);
            wave.rms.assign(static_cast<size_t>(buckets), 0.0f);
            std::vector<double> energy(static_cast<size_t>(buckets), 0.0);
            std::vector<double> counted(static_cast<size_t>(buckets), 0.0);

            wchar_t line[1024];
            _snwprintf_s(line, _TRUNCATE,
                         L"\"%s\" -v quiet -nostdin -i \"%s\" -map 0:a:%d -ac 1 -ar %d "
                         L"-f s16le -",
                         ffmpegPath().c_str(), path.c_str(),
                         streams[static_cast<size_t>(track)], kProbeRate);

            Pipe pipe;
            if (!pipe.open(line)) continue;

            std::vector<int16_t> chunk(8192);
            long long index = 0;
            const double perBucket = duration * kProbeRate / std::max(1, buckets);
            DWORD got = 0;
            while (!stop_.load() &&
                   ReadFile(pipe.read, chunk.data(),
                            static_cast<DWORD>(chunk.size() * sizeof(int16_t)), &got,
                            nullptr) &&
                   got > 0) {
                const size_t samples = got / sizeof(int16_t);
                for (size_t i = 0; i < samples; ++i, ++index) {
                    const size_t bucket = static_cast<size_t>(index / perBucket);
                    if (bucket >= wave.peaks.size()) break;
                    const float value = std::fabs(chunk[i] / 32768.0f);
                    wave.peaks[bucket] = std::max(wave.peaks[bucket], value);
                    energy[bucket] += static_cast<double>(value) * value;
                    counted[bucket] += 1.0;
                }
                progress_.store(static_cast<int>(
                    (track * 100.0 + std::min(100.0, index / (duration * kProbeRate) * 100.0)) /
                    trackCount));
            }
            pipe.close();

            for (size_t i = 0; i < wave.rms.size(); ++i) {
                wave.rms[i] = counted[i] > 0.0
                                  ? static_cast<float>(std::sqrt(energy[i] / counted[i]))
                                  : 0.0f;
            }

            // Normalised per track, because the shape is what is being read off
            // the screen, not the level - the meter and the loudness figure say
            // that, and a quiet track drawn as a flat line is useless for
            // finding the moment somebody spoke.
            const float loudest = *std::max_element(wave.peaks.begin(), wave.peaks.end());
            if (loudest > 0.0f) {
                for (float& p : wave.peaks) p /= loudest;
                for (float& r : wave.rms) r /= loudest;
            }
            result_[static_cast<size_t>(track)] = std::move(wave);
        }

        progress_.store(100);
        running_.store(false);
        done_.store(true);
    });
}

void WaveformJob::cancel() {
    stop_.store(true);
    if (worker_.joinable()) worker_.join();
    running_.store(false);
}

// ---------------------------------------------------------------- thumbnails

void ThumbJob::start(const std::wstring& path, double duration, int count) {
    cancel();
    clear();
    if (ffmpegPath().empty() || duration <= 0.0 || count <= 0) return;

    if (!lockReady_) { InitializeCriticalSection(&lock_); lockReady_ = true; }
    stop_.store(false);
    done_.store(false);
    running_.store(true);
    progress_.store(0);

    worker_ = std::thread([this, path, duration, count] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        const size_t bytes = static_cast<size_t>(kThumbWidth) * kThumbHeight * 4;
        std::vector<unsigned char> frame(bytes);

        for (int i = 0; i < count && !stop_.load(); ++i) {
            const double at = duration * (i + 0.5) / count;

            // -ss before -i is the fast seek: ffmpeg jumps to the nearest key
            // frame instead of decoding up to the moment, which turns minutes
            // into a fraction of a second and costs a picture that may be a few
            // frames off - invisible on a strip this size.
            //
            // Padded to an exact size so every frame is the same count of bytes
            // and no header parsing is needed at this end.
            wchar_t line[1200];
            _snwprintf_s(line, _TRUNCATE,
                         L"\"%s\" -v quiet -nostdin -noaccurate_seek -ss %.3f -i \"%s\" "
                         L"-frames:v 1 -vf "
                         L"\"scale=%d:%d:force_original_aspect_ratio=decrease,"
                         L"pad=%d:%d:(ow-iw)/2:(oh-ih)/2\" "
                         L"-pix_fmt bgra -f rawvideo -",
                         ffmpegPath().c_str(), at, path.c_str(), kThumbWidth, kThumbHeight,
                         kThumbWidth, kThumbHeight);

            Pipe pipe;
            if (!pipe.open(line)) continue;

            size_t filled = 0;
            DWORD got = 0;
            while (filled < bytes &&
                   ReadFile(pipe.read, frame.data() + filled,
                            static_cast<DWORD>(bytes - filled), &got, nullptr) &&
                   got > 0) {
                filled += got;
            }
            pipe.close();
            if (stop_.load()) break;
            if (filled < bytes) continue;

            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = kThumbWidth;
            info.bmiHeader.biHeight = -kThumbHeight;   // top-down, as ffmpeg writes it
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;

            void* pixels = nullptr;
            HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels,
                                              nullptr, 0);
            if (!bitmap) continue;
            memcpy(pixels, frame.data(), bytes);

            EnterCriticalSection(&lock_);
            thumbs_.push_back(Thumb{at, bitmap});
            LeaveCriticalSection(&lock_);
            progress_.store((i + 1) * 100 / count);
        }

        progress_.store(100);
        running_.store(false);
        done_.store(true);
    });
}

void ThumbJob::cancel() {
    stop_.store(true);
    if (worker_.joinable()) worker_.join();
    running_.store(false);
}

void ThumbJob::clear() {
    if (!lockReady_) return;
    EnterCriticalSection(&lock_);
    for (Thumb& t : thumbs_) {
        if (t.bitmap) DeleteObject(t.bitmap);
    }
    thumbs_.clear();
    LeaveCriticalSection(&lock_);
    done_.store(false);
    progress_.store(0);
}

std::vector<Thumb> ThumbJob::taken() const {
    if (!lockReady_) return {};
    EnterCriticalSection(&lock_);
    std::vector<Thumb> copy = thumbs_;
    LeaveCriticalSection(&lock_);
    return copy;
}

// ---------------------------------------------------------------- loudness

void LoudnessJob::start(const std::wstring& path, const std::vector<int>& streams) {
    const int trackCount = static_cast<int>(streams.size());
    cancel();
    if (ffmpegPath().empty() || trackCount <= 0) return;

    stop_.store(false);
    done_.store(false);
    running_.store(true);
    result_.assign(static_cast<size_t>(trackCount), -70.0f);

    worker_ = std::thread([this, path, streams, trackCount] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        for (int track = 0; track < trackCount && !stop_.load(); ++track) {
            wchar_t line[1024];
            _snwprintf_s(line, _TRUNCATE,
                         L"\"%s\" -hide_banner -nostats -nostdin -i \"%s\" -map 0:a:%d "
                         L"-af ebur128=framelog=quiet -f null -",
                         ffmpegPath().c_str(), path.c_str(),
                         streams[static_cast<size_t>(track)]);

            std::string text;
            runHidden(line, &text, 15 * 60 * 1000);

            // The summary block at the end reports "I: -18.4 LUFS"; the last
            // such figure is the integrated one for the whole track.
            size_t at = text.rfind("I:");
            if (at != std::string::npos) {
                result_[static_cast<size_t>(track)] =
                    static_cast<float>(atof(text.c_str() + at + 2));
            }
        }
        running_.store(false);
        done_.store(true);
    });
}

void LoudnessJob::cancel() {
    stop_.store(true);
    if (worker_.joinable()) worker_.join();
    running_.store(false);
}

}  // namespace vp
