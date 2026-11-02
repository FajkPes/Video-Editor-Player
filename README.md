# Video Editor Player

A Windows video player and editor built to make working with multiple audio tracks easier.

## Why use it?

Video Editor Player combines playback with a timeline for quick editing. It is useful when a video contains several audio tracks and you want to control them individually before exporting.

## What you can do

- Preview video with zoom, pan, rotation and crop controls.
- Work with multiple audio tracks: adjust their volume, mute them and mix them for export.
- Trim and join sections, change speed and pitch, and add timed captions.
- Use fades, picture adjustments and loudness normalization.
- Export the edited result using the bundled FFmpeg tools.

## Install and start

1. Download the Windows x64 ZIP from [Releases](https://github.com/FajkPes/Video-Editor-Player/releases/latest).
2. Extract the entire archive to a folder.
3. Run `Start Video Editor Player.bat` and open a video.

The package includes libmpv, FFmpeg and FFprobe. Keep them with the application. Use the timeline to select sections, adjust each track in the audio controls, then open the render window to choose export settings. Preview and export use different audio processing paths, so their individual samples need not match exactly.

The source builds with Visual Studio C++ tools and the Windows SDK; see [TESTING.md](TESTING.md) for build and regression-test details.

[Detailed documentation](README-DETAILS.md) · [All releases](https://github.com/FajkPes/Video-Editor-Player/releases)

## Credits

**Idea, concept and design:** FajkPes  
**Coding:** ChatGPT Codex, Claude and Google Antigravity
