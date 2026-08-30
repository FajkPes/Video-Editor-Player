@echo off
setlocal
cd /d "%~dp0"

set "APP=build\VideoEditorPlayer.exe"
set "ARGS=%*"
set "REBUILD="

if /i "%~1"=="/build" (
    set "REBUILD=1"
    set "ARGS="
)
if not exist "%APP%" set "REBUILD=1"

if defined REBUILD (
    echo Building Video Editor Player...
    call "%~dp0build.bat"
    if errorlevel 1 (
        echo.
        echo The build failed. See the compiler output above.
        pause
        exit /b 1
    )
)

if not exist "build\libmpv-2.dll" (
    echo Warning: libmpv-2.dll is missing. Video playback will not work.
)
if not exist "build\ffmpeg.exe" (
    where ffmpeg.exe >nul 2>nul || echo Warning: FFmpeg is missing. Previews and rendering will be unavailable.
)

start "" "%APP%" %ARGS%
exit /b 0

rem Created by FajkPes with assistance from ChatGPT Codex and Claude Code.
