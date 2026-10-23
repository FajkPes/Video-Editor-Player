param([string]$ToolsDirectory = '')
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
$originalPath = $env:PATH
try {
    if ($ToolsDirectory) {
        $resolvedTools = (Resolve-Path -LiteralPath $ToolsDirectory).Path
        $env:PATH = $resolvedTools + ';' + $env:PATH
    }
    $ffmpeg = @('.\build\ffmpeg.exe', '.\ffmpeg.exe', '..\Video Player\ffmpeg.exe') |
        Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (!$ffmpeg) {
        $command = Get-Command ffmpeg.exe -ErrorAction SilentlyContinue
        if (!$command) { throw 'ffmpeg.exe was not found. Supply -ToolsDirectory with the folder containing ffmpeg.exe and ffprobe.exe.' }
        $ffmpeg = $command.Source
    }
    & .\build.bat
    if ($LASTEXITCODE -ne 0) { throw 'Player build failed' }
    & .\build_ui_probe.bat
    if ($LASTEXITCODE -ne 0) { throw 'UI probe build failed' }
    & $ffmpeg -y -hide_banner -loglevel error -f lavfi -i 'testsrc2=s=640x360:r=30:d=6' -f lavfi -i 'sine=frequency=440:duration=6' -f lavfi -i 'sine=frequency=880:duration=6' -map 0:v -map 1:a -map 2:a -c:v libx264 -preset ultrafast -c:a aac build/ui-test-source.mp4
    if ($LASTEXITCODE -ne 0) { throw 'Test source generation failed' }
    & .\build\ui_probe.exe "$PSScriptRoot\build\ui-test-source.mp4" | Tee-Object -FilePath build/ui-probe.log
    if ($LASTEXITCODE -ne 0) { throw 'UI regression failed; see build/ui-probe.log' }
} finally {
    $env:PATH = $originalPath
    Pop-Location
}
