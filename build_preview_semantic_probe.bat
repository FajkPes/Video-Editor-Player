@echo off
setlocal
set "VSROOT="
for /d %%D in ("%ProgramFiles%\Microsoft Visual Studio\*") do for /d %%E in ("%%~fD\*") do if exist "%%~fE\VC\Auxiliary\Build\vcvarsall.bat" set "VSROOT=%%~fE"
if not defined VSROOT exit /b 1
set "WINSDK="
for /d %%D in ("%ProgramFiles(x86)%\Windows Kits\10\Lib\10.*") do if exist "%%~fD\um\x64\DbgHelp.Lib" set "WINSDK=%%~nxD"
call "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" amd64 %WINSDK% >nul
cl /nologo /std:c++17 /EHsc /W3 /O2 /MD /utf-8 /Fe:build\preview_semantic_probe.exe /Fo:build\preview_semantic_probe.obj src\preview_semantic_probe.cpp build\audio_engine.obj build\audio_output.obj build\mpv_video.obj build\clip_window.obj build\settings_window.obj build\render.obj build\timeline.obj build\settings.obj build\media_tools.obj build\theme.obj build\lang.obj /link user32.lib gdi32.lib comdlg32.lib shell32.lib
exit /b %errorlevel%
