@echo off
setlocal
cd /d "%~dp0"

if not exist "VideoEditorPlayer.exe" (
    echo VideoEditorPlayer.exe is missing. Extract the complete release ZIP and try again.
    pause
    exit /b 1
)
if not exist "libmpv-2.dll" (
    echo libmpv-2.dll is missing. Extract the complete release ZIP and try again.
    pause
    exit /b 1
)
if not exist "ffmpeg.exe" echo Warning: FFmpeg is missing. Previews and rendering will be unavailable.
if not exist "ffprobe.exe" echo Warning: FFprobe is missing. Some media analysis will be unavailable.

start "" "%~dp0VideoEditorPlayer.exe" %*
exit /b 0

rem Created by FajkPes with assistance from ChatGPT Codex and Claude Code.
