# Third-party components

The downloadable Windows package uses the following third-party projects:

- **mpv / libmpv** — video decoding and rendering. Project: <https://mpv.io/>. Source: <https://github.com/mpv-player/mpv>.
- **FFmpeg** — media probing, previews, waveform analysis, filtering, encoding, and rendering. Project and source: <https://ffmpeg.org/>.

## Included versions

### libmpv

- Version: `mpv v0.41.0-920-gdd5d17d32`
- Architecture: Windows x86-64-v3
- Build: shared libmpv, GPL features enabled
- SHA-256: `8DC675425BE5E4FB21A576232CDDFD6D0C9FB5DEC9467C5CA9412387DFD7AC7E`
- Exact mpv source: <https://github.com/mpv-player/mpv/tree/dd5d17d32>
- Windows build system: <https://github.com/shinchiro/mpv-winbuild-cmake>
- License and copyright files: `third_party/licenses/mpv-GPL-2.0.txt` and `third_party/licenses/mpv-Copyright.txt`

### FFmpeg and FFprobe

- Version: `N-126313-g1ae4048218-20260829`
- Variant: BtbN Windows x86-64 GPL static build
- FFmpeg SHA-256: `773ED53B6287FE7B7224818786C42B9B0EB734F8B2692E77DF688B8910407EDA`
- FFprobe SHA-256: `513D533A883FEBB1AA94893C00092283F28ACD127D4BFA8C35BB3B82343B7324`
- Exact FFmpeg source: <https://github.com/FFmpeg/FFmpeg/tree/1ae4048218>
- Reproducible build scripts and downloads: <https://github.com/BtbN/FFmpeg-Builds>
- License file: `third_party/licenses/FFmpeg-GPL-3.0.txt`

These notices apply only to the third-party components. The original Video Editor Player source was created by FajkPes and is released under GPL-3.0-or-later so the complete package can be redistributed consistently with these dependencies.
