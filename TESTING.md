# Ověření přehrávače

`build.bat` sestaví aplikaci do `build/VideoPlayer.exe`. Spouštěcí dávka
`Video Player.bat` používá tento soubor; existující běžící proces je potřeba
ukončit a spustit znovu, aby používal nový kód.
Pokud je EXE zamčené běžícím přehrávačem, sestavení vytvoří
`build/VideoPlayer.next.exe`. Po zavření přehrávače ho `Video Player.bat`
při dalším spuštění převezme. Běžící aplikace ani její rozpracovaný projekt
se kvůli sestavení neukončují.

## Kompletní kontrola médií

`powershell -ExecutionPolicy Bypass -File .\test_media.ps1 -ToolsDirectory "C:\nastroje\ffmpeg"`
sestaví aplikaci i všechny sondy a provede kontrolu obsahu exportu, nativního
náhledu mpv, mixéru a UI. Vyžaduje Python 3 bez dalších balíčků, FFmpeg,
FFprobe, libmpv a pro závěrečné GPU testy NVIDIA NVENC.

Testovací obraz má čtyři barevné plochy a rozpoznatelnou intenzitu podle sekundy
zdroje. Dvě zvukové stopy mají 440 a 880 Hz a časové značky. Kontroly dekódují
hotové MP4 a měří barvy, spektrální složky, zesílení, délky a polohu značek:

- střihy a IN/OUT, přesná návaznost vybraných úseků a souběh obrazu a zvuku;
- efekty pouze pro zvolený klip, ztlumení jednotlivé stopy a lokální hlasitost;
- lokální a globální rychlost, pitch a zachování výšky tónu;
- zoom, posun po zvětšení, crop bez roztažení obrazu, padding a rotaci;
- fade ve zdrojovém čase přes odstraněné úseky, včetně plynulé křivky;
- skutečné umístění titulků v čase po střihu;
- normalizační zesílení a video bez zvukové stopy;
- shodu vzorkovaných míst nativních snímků mpv s exportem;
- přepnutí lokální hlasitosti a mute na hranici vzorku v živém mixéru;
- zvukové hodiny při 0,1×, 0,5×, 2× a 4× a seek při pauze;
- pokračování delší zvukové stopy po konci kratší stopy;
- návaznost dvaceti krátkých zrychlených střihů bez hromadění posunu obrazu;
- uložení a načtení časové osy, lokálních efektů, mute, fade a titulků včetně emoji;
- dostupnost a hit testing posouvaného okna Render s mnoha stopami.

Výsledky jsou v `build/semantic-tests/results.json` a v protokolech
`build/semantic-probe.log`, `build/preview-semantic-probe.log`,
`build/audio-semantic-probe.log`, `build/ui-probe.log` a `build/render-probe.log`.

Kontroly rozlišují zaokrouhlení na výstupní snímky, ztrátovou kompresi a rozdílné
škálování mpv/FFmpeg. Neprokazují fyzickou latenci reproduktorů či Bluetooth.
Změna pitch a časové roztažení používají v živém přehrávači a FFmpeg odlišné DSP;
shoduje se nastavení a kontrolovaná výška tónu, nikoli jednotlivé audio vzorky.
Kolečko a `Z` ovládají přiblížení náhledu; **Zoom / Across / Down v panelu obrazu**
jsou úpravy, které vstupují do exportu.

## UI a náhled videa

Spusťte `powershell -ExecutionPolicy Bypass -File .\test_ui.ps1`.
FFmpeg lze dodat parametrem `-ToolsDirectory "C:\cesta\k\nastrojum"`.
Vedle výsledného programu musí být `libmpv-2.dll`.

Test otevře vlastní okno přehrávače, vytvoří krátké testovací video a ověří:

- oddělený viewport videa a ořezání při zoomu 25–400 % a krajních posunech;
- rozložení tlačítek při šířkách klientské plochy 1240, 1366, 1500, 1800 a 2200 px;
- režimy Fit, Fill a Original, zoom v původní velikosti a reset klávesou Z;
- skutečný výsledek hit testu Windows nad vloženým videem a doručení zprávy kolečka;
- přesný seek, otočení obrazu, tažení a ztrátu zachycení myši;
- přechody do celé obrazovky a zpět;
- posouvání dlouhé časové osy a zachování vnořených ořezávacích oblastí GDI;
- opakované zavření Nastavení a Renderu bez ukončení hlavní smyčky zpráv.

Protokol je v `build/ui-probe.log`. Test neukládá své nastavení ani pozici
testovacího videa. Doručení zpráv testuje přímo proti oknům Win32; fyzické
kolečko a chování ovladače myši je vhodné ověřit ručně.

## Export

`test_render.ps1` sestaví a spustí regresní testy exportu. FFmpeg a FFprobe musí
být dostupné vedle programu nebo v PATH. GPU varianty vyžadují NVIDIA NVENC.
Protokoly jednotlivých případů jsou v `build/render-tests/`.

## Krátká ruční kontrola

1. Otevřete video a vyzkoušejte kolečko i klávesy `+`/`-` ve všech třech
   režimech Fit/Fill/Original. `Z` vrací přiblížení a posun náhledu.
2. Při 400 % táhněte obraz přes hranice panelu a spodního ovládání. Obraz musí
   zůstat uvnitř video viewportu, ovládání musí zůstat čitelné a klikatelné.
3. Během tažení přepněte okno a vraťte se. Pohyb bez stisknutého tlačítka již
   nesmí obraz posouvat.
4. Dvojklikem přejděte do celé obrazovky a zpět; zkontrolujte obraz i ovládání.
5. V okně vlastností s mnoha řádky posuňte obsah kolečkem až dolů a zpět.
   Zkontrolujte polohu řádků, editaci textu a pevná tlačítka Uložit/Zrušit.
