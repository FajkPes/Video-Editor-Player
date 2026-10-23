@echo off
rem ---------------------------------------------------------------------------
rem  Video Player - spousteci davka
rem
rem  Spusti prehravac z build\VideoPlayer.exe. Prekladat se nic nebude - jen
rem  kdyz program jeste neexistuje, nebo kdyz o to reknes prepinacem /build.
rem  Soubor predany na prikazove radce (nebo pretazeny na tuhle davku) se
rem  rovnou otevre.
rem
rem    "Video Player.bat"                 spusti prehravac
rem    "Video Player.bat" film.mkv        spusti ho a otevre film
rem    "Video Player.bat" /build          prelozi znovu, i kdyz to neni treba
rem ---------------------------------------------------------------------------
setlocal
cd /d "%~dp0"

set "EXE=build\VideoPlayer.exe"
set "NEEDBUILD="
set "ARGS=%*"

if exist "build\VideoPlayer.next.exe" (
    copy /y "build\VideoPlayer.next.exe" "%EXE%" >nul 2>nul
    if errorlevel 1 (
        echo Nova verze je pripravena. Zavrete bezici prehravac a spustte tuto davku znovu.
        pause
        exit /b 1
    )
    del /q "build\VideoPlayer.next.exe"
)

if /i "%~1"=="/build" ( set "NEEDBUILD=1" & set "ARGS=" )
if not exist "%EXE%" set "NEEDBUILD=1"

if defined NEEDBUILD (
    echo Prekladam Video Player...
    echo.
    call "%~dp0build.bat"
    if errorlevel 1 (
        echo.
        echo === Preklad se nepovedl, program se nespousti. ===
        echo.
        pause
        exit /b 1
    )
    echo.
)

rem libmpv-2.dll musi lezet vedle programu, jinak nebude obraz.
if not exist "build\libmpv-2.dll" (
    echo UPOZORNENI: build\libmpv-2.dll chybi - pojede jen zvuk.
    echo.
)
rem ffmpeg.exe je potreba na nahledy v editoru a na export.
if not exist "build\ffmpeg.exe" (
    where ffmpeg.exe >nul 2>nul || (
        echo UPOZORNENI: ffmpeg.exe nenalezen - nepujdou nahledy ani render.
        echo.
    )
)

start "" "%EXE%" %ARGS%
exit /b 0
