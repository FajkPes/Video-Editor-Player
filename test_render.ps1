$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
& .\build.bat
if ($LASTEXITCODE -ne 0) { throw 'Player build failed' }
& .\build_render_probe.bat
if ($LASTEXITCODE -ne 0) { throw 'Render probe build failed' }
$renderFfmpeg = @('.\build\ffmpeg.exe', '.\ffmpeg.exe', '..\Video Player\ffmpeg.exe') |
    Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (!$renderFfmpeg) { $renderFfmpeg = (Get-Command ffmpeg.exe).Source }
New-Item -ItemType Directory -Force build/render-tests | Out-Null
& $renderFfmpeg -y -hide_banner -loglevel error -f lavfi -i 'testsrc2=s=640x360:r=30:d=6' -f lavfi -i 'sine=frequency=440:duration=6' -f lavfi -i 'sine=frequency=880:duration=6' -map 0:v -map 1:a -map 2:a -c:v libx264 -preset ultrafast -c:a aac build/render-tests/source.mp4
if ($LASTEXITCODE -ne 0) { throw 'Test source generation failed' }
& .\build\render_probe.exe "$PSScriptRoot\build\render-tests\source.mp4" "$PSScriptRoot\build\render-tests"
if ($LASTEXITCODE -ne 0) { throw 'Render regression failed; see build/render-tests/*.log (GPU tests require NVIDIA NVENC)' }
