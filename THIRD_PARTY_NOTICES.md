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

- Version: `8.1.2-full_build-www.gyan.dev` (verified with `-version`)
- Windows x64 GPL static full build by gyan.dev
- FFmpeg SHA-256: `AD8F211BC894755E0061C55AB280AE00E8D3D4F15A8CC4372B24CFA247B5942E`
- FFprobe SHA-256: `9DF3B0B5275E830961DF6D94E1F7A71121A7ABD5FF708E9FEC8A0B6084A55015`
- Source: <https://github.com/FFmpeg/FFmpeg/tree/n8.1.2>
- Build distribution: <https://www.gyan.dev/ffmpeg/builds/>
- License file: `third_party/licenses/FFmpeg-GPL-3.0.txt`

These notices apply only to the third-party components. The original Video Editor Player source was created by FajkPes and is released under GPL-3.0-or-later so the complete package can be redistributed consistently with these dependencies.
