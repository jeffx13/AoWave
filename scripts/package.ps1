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

$stage = Join-Path $root "build\package\$name"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
cmake --install build\release --prefix $stage
if ($LASTEXITCODE -ne 0) { throw "cmake --install failed" }
Copy-Item LICENSE $stage
# The notices go in as .txt (same text) so any machine opens them.
Copy-Item THIRD-PARTY-NOTICES.md (Join-Path $stage "THIRD-PARTY-NOTICES.txt")
Copy-Item licenses $stage -Recurse

New-Item -ItemType Directory -Force -Path dist | Out-Null
$zip = Join-Path $root "dist\$name-$version-win64.zip"
if (Test-Path $zip) { Remove-Item $zip }
# Not Compress-Archive: Windows PowerShell's writes backslashes into entry names.
& "$env:SystemRoot\System32\tar.exe" -a -cf $zip -C (Join-Path $root "build\package") $name
if ($LASTEXITCODE -ne 0) { throw "could not zip $stage" }
Remove-Item -Recurse -Force (Join-Path $root "build\package")
Write-Host "-> $zip"
