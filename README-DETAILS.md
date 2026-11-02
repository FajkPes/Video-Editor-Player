# Video Editor Player — Windows C++

Aktuální zdrojový kód a přenosná Windows x64 sestava z 23. října 2026.

Stáhněte ZIP z [nejnovějšího vydání](https://github.com/FajkPes/Video-Editor-Player/releases/latest), rozbalte celou složku a spusťte **Start Video Editor Player.bat**. Balík obsahuje aplikaci, libmpv, FFmpeg, FFprobe a český překlad. Nepotřebuje Python ani překladač.

Přehrávač podporuje časovou osu, střihy, obrazové úpravy, titulky, zvukové stopy a export přes FFmpeg. Testované chování a omezení popisuje [TESTING.md](TESTING.md). Osobní nastavení, rozpracované projekty a testovací média nejsou součástí vydání.

## Sestavení

Na Windows s Visual Studio C++ Build Tools a Windows SDK spusťte `build.bat`. Výstup je `build/VideoPlayer.exe`. Vedle něj umístěte `libmpv-2.dll`, `ffmpeg.exe` a `ffprobe.exe` z vydání; překlady patří do `build/lang`. Spouštějte `Video Player.bat` nebo `Start Video Editor Player.bat`.

Datum označuje tento publikační snímek; původní vydání v1.0.0 zůstává dostupné. Licence aplikace je GPL-3.0-or-later, podrobnosti v [LICENSE](LICENSE) a [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
