# Video Editor Player

A fast Windows video player that combines everyday playback with practical editing tools. It was created to make detailed inspection and adjustment of videos possible without constantly switching between a media player and a full editing suite—especially when zooming, panning, comparing audio tracks, or correcting track loudness.

## Highlights

- Smooth video playback powered by libmpv
- Precise zoom and pan controls for inspecting any part of a frame
- Multiple audio-track playback, muting, mixing, gain, EQ, pitch, and per-track loudness normalization
- Adjustable target loudness in LUFS
- Playback speed control with optional pitch preservation
- Brightness, contrast, saturation, gamma, hue, sharpening, and denoising controls
- Rotate, flip, crop, opacity, and framing tools
- Editor timeline with clips, thumbnails, waveforms, and captions
- Landscape and portrait output, output-size and frame-rate controls
- GPU export through NVIDIA NVENC and CPU export through x264/x265 when supported by the included FFmpeg build
- Reusable audio, video, and render presets
- English interface by default, with an optional Czech translation
- Portable settings stored beside the executable

## Download and start

1. Open the latest GitHub release and download the ZIP file.
2. Extract the complete ZIP to a writable folder.
3. Run `Start Video Editor Player.bat` or `VideoEditorPlayer.exe`.
4. Open a video from the application, pass a file to the launcher, or drag a video onto the launcher.

The release package includes the runtime components required for playback, previews, and rendering. No separate Visual C++ Redistributable installation is required.

## Basic use

- Use the transport controls or keyboard shortcuts shown inside the application for playback and seeking.
- Open the editor to generate the waveform and thumbnail strip, split clips, adjust framing, add captions, and prepare an export.
- Configure each audio track independently. Enable normalization and choose a target loudness when tracks have very different levels.
- Save frequently used audio, picture, or export combinations as presets.
- Select a different encoder if an export fails. NVENC options require a compatible NVIDIA GPU and driver; CPU options require the relevant encoder in FFmpeg.

## Language and settings

English is the default on a fresh installation. To switch languages, open the application settings and select the Czech translation. The application creates `vp_settings.json` beside the executable and stores preferences, presets, recent positions, and the selected language there.

No personal settings or playback history are included in the public release.

## Build from source

Requirements:

- Windows 10 or Windows 11, 64-bit
- Visual Studio 2022 or newer with the Desktop development with C++ workload
- A Windows SDK
- `libmpv-2.dll` in `vendor` for video playback
- `ffmpeg.exe` and `ffprobe.exe` in `vendor` for previews, analysis, and rendering

Run `build.bat`. The program is built with the static MSVC runtime and placed in `build`.

## Important notes

- Hardware decoding and GPU rendering depend on the installed graphics driver.
- NVENC export requires supported NVIDIA hardware.
- Very large videos, waveform generation, thumbnails, and complex filters can require substantial CPU, GPU, memory, and temporary storage.
- Always keep the original media files until the rendered result has been checked.

## Credits

Created by **FajkPes** with assistance from **ChatGPT Codex** and **Claude Code**.

## License

Video Editor Player is distributed under the **GNU General Public License v3.0 or later**. This GPL-compatible license is required for a complete package that includes the GPL builds of libmpv and FFmpeg used by the application. See `LICENSE` and `THIRD_PARTY_NOTICES.md`.
