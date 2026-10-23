@echo off
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSROOT="
set "WINSDK="
if exist "%VSWHERE%" for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%I"
if not defined VSROOT for /d %%D in ("%ProgramFiles%\Microsoft Visual Studio\*") do for /d %%E in ("%%~fD\*") do if exist "%%~fE\VC\Auxiliary\Build\vcvarsall.bat" set "VSROOT=%%~fE"
for /d %%D in ("%ProgramFiles(x86)%\Windows Kits\10\Lib\10.*") do if exist "%%~fD\um\x64\DbgHelp.Lib" set "WINSDK=%%~nxD"
if not defined VSROOT echo Visual Studio not found & exit /b 1
call "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" amd64 %WINSDK% >nul
if not exist build mkdir build
cl /nologo /std:c++17 /EHsc /W3 /O2 /MD /Fe:build\audio_probe.exe /Fo:build\ ^
   src\audio_engine.cpp src\audio_output.cpp src\audio_probe.cpp
exit /b %errorlevel%
