# Downloads the pinned yt-dlp, ffmpeg, N_m3u8DL-RE and libmpv into third-parties/, each checked
# against its SHA-256. Bump a version by changing its URL and hash together.
# Usage: powershell -ExecutionPolicy Bypass -File scripts/download-tools.ps1

$ErrorActionPreference = "Stop"
$root = Join-Path $PSScriptRoot "..\third-parties"
$bin = Join-Path $root "bin"
$lib = Join-Path $root "lib"
$tmp = Join-Path $env:TEMP "app-deps"
New-Item -ItemType Directory -Force -Path $bin, $lib, $tmp | Out-Null

# Tries each url in turn, each a few times: SourceForge hands out a random mirror per request,
# and one that is down failed a whole CI run.
function Grab([string[]]$urls, $sha256) {
    $out = Join-Path $tmp ([IO.Path]::GetFileName(([Uri]$urls[0]).AbsolutePath))
    foreach ($url in $urls) {
        Write-Host "  $url"
        # curl, not Invoke-WebRequest: SourceForge answers a browser-like client with an HTML page.
        curl.exe -sSfL --connect-timeout 20 --retry 3 --retry-all-errors --retry-delay 5 -o $out $url
        if ($LASTEXITCODE -ne 0) { Write-Host "    failed, $(if ($url -ne $urls[-1]) { 'trying the next' } else { 'no more to try' })"; continue }
        $actual = (Get-FileHash -Algorithm SHA256 $out).Hash
        if ($actual -eq $sha256) { return $out }
        Write-Host "    SHA-256 $actual, expected $sha256"
    }
    throw "download failed: $($urls -join ', ')"
}

function Extract($archive, $name, $dest) {
    $into = Join-Path $tmp ([IO.Path]::GetFileNameWithoutExtension($archive))
    New-Item -ItemType Directory -Force -Path $into | Out-Null
    # Windows' own tar reads zip and 7z alike; Git's, first on PATH in its shell, reads neither.
    & "$env:SystemRoot\System32\tar.exe" -xf $archive -C $into
    if ($LASTEXITCODE -ne 0) { throw "could not extract $archive" }
    $file = Get-ChildItem -Path $into -Recurse -Filter $name | Select-Object -First 1
    Copy-Item $file.FullName (Join-Path $dest $name) -Force
}

Write-Host "yt-dlp 2026.08.19..."
Copy-Item (Grab "https://github.com/yt-dlp/yt-dlp/releases/download/2026.08.19/yt-dlp.exe" `
    "66674953FE251B89F4D08C5F0E35E0728679BD67AB3D7D05C0562AF101DD3E7A") (Join-Path $bin "yt-dlp.exe") -Force

Write-Host "ffmpeg 9.0.2..."
$z = Grab "https://github.com/GyanD/codexffmpeg/releases/download/9.0.2/ffmpeg-9.0.2-essentials_build.zip" `
    "60F467265B1E312373DBCD92200C2618A74850F98D3D078E94296BB3FA2047BA"
Extract $z "ffmpeg.exe" $bin

Write-Host "N_m3u8DL-RE 0.6.0..."
$z = Grab "https://github.com/nilaoda/N_m3u8DL-RE/releases/download/v0.6.0-beta/N_m3u8DL-RE_v0.6.0-beta_win-x64_20260629.zip" `
    "3825FD42EE502F98A9378F6FDDDB2F7822709F521806214F466DB6935C950F1A"
Extract $z "N_m3u8DL-RE.exe" $bin

Write-Host "libmpv 2026-09-27..."
$mpv = "project/mpv-player-windows/libmpv/mpv-dev-x86_64-v3-20260927-git-a1bf4b6559.7z"
$z = Grab @("https://downloads.sourceforge.net/$mpv", "https://master.dl.sourceforge.net/$mpv") `
    "DFD6974D207AF85AC1FEEF08FF73A23D1818A498EB8571BFD571B4B82DF0486B"
Extract $z "libmpv-2.dll" $bin
Extract $z "libmpv.dll.a" $lib

Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "Done -> third-parties/bin/ and lib/"
Write-Host "Optional: Anime4K shaders -> third-parties/mpv-config/shaders/ (https://github.com/bloc97/Anime4K)"
