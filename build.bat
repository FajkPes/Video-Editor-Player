@echo off
setlocal
set "VSROOT="
set "WINSDK="
for /d %%D in ("%ProgramFiles%\Microsoft Visual Studio\*") do for /d %%E in ("%%~fD\*") do if exist "%%~fE\VC\Auxiliary\Build\vcvarsall.bat" set "VSROOT=%%~fE"
if not defined VSROOT for /d %%D in ("%ProgramFiles(x86)%\Microsoft Visual Studio\*") do for /d %%E in ("%%~fD\*") do if exist "%%~fE\VC\Auxiliary\Build\vcvarsall.bat" set "VSROOT=%%~fE"
for /d %%D in ("%ProgramFiles(x86)%\Windows Kits\10\Lib\10.*") do if exist "%%~fD\um\x64\DbgHelp.Lib" set "WINSDK=%%~nxD"
if not defined VSROOT (
    echo Visual Studio was not found.
    exit /b 1
)
call "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" amd64 %WINSDK% >nul
if not exist build mkdir build
if not exist build\probe mkdir build\probe
echo === player ===
cl /nologo /std:c++17 /EHsc /W3 /O2 /MD /utf-8 /DUNICODE /D_UNICODE /Fe:build\VideoPlayer.next.exe /Fo:build\ src\audio_engine.cpp src\audio_output.cpp src\mpv_video.cpp src\lang.cpp src\theme.cpp src\settings.cpp src\media_tools.cpp src\timeline.cpp src\render.cpp src\clip_window.cpp src\settings_window.cpp src\main.cpp /link /SUBSYSTEM:WINDOWS
if errorlevel 1 exit /b 1
copy /y build\VideoPlayer.next.exe build\VideoPlayer.exe >nul 2>nul
if errorlevel 1 (
    echo Player is running. Update is ready in build\VideoPlayer.next.exe.
    echo Close the player and start Video Player.bat to apply it.
) else (
    del /q build\VideoPlayer.next.exe
)
if not exist build\lang mkdir build\lang
copy /y lang\*.lang build\lang\ >nul 2>nul
echo === probe ===
cl /nologo /std:c++17 /EHsc /W3 /O2 /MD /utf-8 /Fe:build\audio_probe.exe /Fo:build\probe\ src\audio_engine.cpp src\audio_output.cpp src\audio_probe.cpp
if errorlevel 1 exit /b 1
echo Built.
exit /b 0
