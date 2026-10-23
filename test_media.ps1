param([string]$ToolsDirectory = '')
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
$originalPath = $env:PATH
try {
    if ($ToolsDirectory) { $env:PATH = (Resolve-Path -LiteralPath $ToolsDirectory).Path + ';' + $env:PATH }
    elseif (Test-Path -LiteralPath build/ffmpeg.exe) { $env:PATH = "$PSScriptRoot\build;" + $env:PATH }
    $ffmpeg = (Get-Command ffmpeg.exe -ErrorAction Stop).Source
    Get-Command ffprobe.exe -ErrorAction Stop | Out-Null
    foreach ($build in @('build.bat','build_semantic_probe.bat','build_preview_semantic_probe.bat',
                         'build_audio_semantic_probe.bat','build_ui_probe.bat','build_render_probe.bat')) {
        & ".\$build"
        if ($LASTEXITCODE -ne 0) { throw "$build failed" }
    }
    $pythonArgs = @('test_semantics.py')
    if ($ToolsDirectory) { $pythonArgs += @('--tools-directory', (Resolve-Path -LiteralPath $ToolsDirectory).Path) }
    & python @pythonArgs | Tee-Object -FilePath build/semantic-probe.log
    if ($LASTEXITCODE -ne 0) { throw 'Content export regression failed' }
    & .\build\preview_semantic_probe.exe "$PSScriptRoot\build\semantic-tests\source.mp4" "$PSScriptRoot\build\semantic-tests" |
        Tee-Object -FilePath build/preview-semantic-probe.log
    if ($LASTEXITCODE -ne 0) { throw 'Native preview capture failed' }
    & python @pythonArgs --verify-only | Tee-Object -FilePath build/semantic-probe.log
    if ($LASTEXITCODE -ne 0) { throw 'Preview/export comparison failed' }
    & $ffmpeg -y -v error -i build/semantic-tests/source.mp4 -map 0 -vf scale=640:360 -c:v libx264 -preset ultrafast -c:a copy build/ui-test-source.mp4
    if ($LASTEXITCODE -ne 0) { throw 'UI source generation failed' }
    & $ffmpeg -y -v error -i build/ui-test-source.mp4 -map 0 -c:v copy -c:a aac -filter:a:0 atrim=duration=1 build/semantic-tests/unequal-tracks.mp4
    if ($LASTEXITCODE -ne 0) { throw 'Unequal audio track source generation failed' }
    & .\build\audio_semantic_probe.exe "$PSScriptRoot\build\ui-test-source.mp4" "$PSScriptRoot\build\semantic-tests\no_audio.mp4" "$PSScriptRoot\build\semantic-tests\unequal-tracks.mp4" |
        Tee-Object -FilePath build/audio-semantic-probe.log
    if ($LASTEXITCODE -ne 0) { throw 'Audio regression failed' }
    & .\build\ui_probe.exe "$PSScriptRoot\build\ui-test-source.mp4" | Tee-Object -FilePath build/ui-probe.log
    if ($LASTEXITCODE -ne 0) { throw 'UI regression failed' }
    New-Item -ItemType Directory -Force build/render-tests | Out-Null
    & .\build\render_probe.exe "$PSScriptRoot\build\ui-test-source.mp4" "$PSScriptRoot\build\render-tests" |
        Tee-Object -FilePath build/render-probe.log
    if ($LASTEXITCODE -ne 0) { throw 'Basic export/GPU regression failed' }
} finally {
    $env:PATH = $originalPath
    Pop-Location
}
