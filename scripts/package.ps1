# Builds Release and zips a clean install into dist/AoWave-<version>-win64.zip: an AoWave folder
# holding AoWave.exe and app/, where the app keeps its data/ once it runs.
# Usage: powershell -ExecutionPolicy Bypass -File scripts/package.ps1
# Run scripts/download-tools.ps1 first: the tools and libmpv in third-parties/ go into the zip.

$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..")
Set-Location $root

$version = [regex]::Match((Get-Content CMakeLists.txt -Raw), 'project\(\w+ VERSION ([\d.]+)').Groups[1].Value
$name = [regex]::Match((Get-Content CMakeLists.txt -Raw), 'set\(APP_DISPLAY_NAME "([^"]+)"').Groups[1].Value
if (-not $version -or -not $name) { throw "no version or name in CMakeLists.txt" }
foreach ($tool in "ffmpeg.exe", "N_m3u8DL-RE.exe", "yt-dlp.exe", "libmpv-2.dll") {
    if (-not (Test-Path "third-parties\bin\$tool")) { throw "third-parties\bin\$tool is missing: run scripts\download-tools.ps1" }
}

cmd /c "scripts\build.cmd release"
if ($LASTEXITCODE -ne 0) { throw "the release build failed" }

# A fresh folder, not build\release\deploy: a copy run from there keeps its settings, library
# and cookies beside the exe, and none of that belongs in a release.
$stage = Join-Path $root "build\package\$name"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
cmake --install build\release --prefix $stage
if ($LASTEXITCODE -ne 0) { throw "cmake --install failed" }
Copy-Item LICENSE $stage

New-Item -ItemType Directory -Force -Path dist | Out-Null
$zip = Join-Path $root "dist\$name-$version-win64.zip"
if (Test-Path $zip) { Remove-Item $zip }
# Not Compress-Archive: Windows PowerShell's writes backslashes into entry names, which other
# unzip tools read as file names.
& "$env:SystemRoot\System32\tar.exe" -a -cf $zip -C (Join-Path $root "build\package") $name
if ($LASTEXITCODE -ne 0) { throw "could not zip $stage" }
Remove-Item -Recurse -Force (Join-Path $root "build\package")
Write-Host "-> $zip"
