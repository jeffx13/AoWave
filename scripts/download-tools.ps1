$ErrorActionPreference = "Stop"
$root = Join-Path $PSScriptRoot "..\third-parties"
$bin = Join-Path $root "bin"
$lib = Join-Path $root "lib"
$tmp = Join-Path $env:TEMP "app-deps"
New-Item -ItemType Directory -Force -Path $bin, $lib, $tmp | Out-Null

function Grab([string[]]$urls, $sha256) {
    $out = Join-Path $tmp ([IO.Path]::GetFileName(([Uri]$urls[0]).AbsolutePath))
    foreach ($url in $urls) {
        Write-Host "  $url"
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
    # Windows' own tar reads zip and 7z; Git's, first on PATH in its shell, reads neither.
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

Write-Host "libmpv 2026-10-07 (the AnimeJaNai fork, the build AnimeJaNai 3.7.0 pins)..."
$z = Grab "https://github.com/the-database/mpv-winbuild/releases/download/2026-10-07-d6d93599d5/mpv-dev-x86_64-20261007-git-d6d93599d5.7z" `
    "077CB75FED47B185428F97224E1D798E2D4C2F4063FD8CDA2662B74F2892327C"
Extract $z "libmpv-2.dll" $bin
Extract $z "libmpv.dll.a" $lib

Write-Host "Shaders: Anime4K 4.0.1, ArtCNN 1.6.2 and chroma-from-luma prediction..."
$shaders = Join-Path $root "mpv-config\shaders"
New-Item -ItemType Directory -Force -Path $shaders | Out-Null
foreach ($shader in @(
    @("https://raw.githubusercontent.com/bloc97/Anime4K/v4.0.1/glsl/Restore/Anime4K_Clamp_Highlights.glsl", "A2A9BF7FBC1D75D09660CA2E701E4D7FB0CF5457B94DA47E1825032FA2B3671A"),
    @("https://raw.githubusercontent.com/bloc97/Anime4K/v4.0.1/glsl/Restore/Anime4K_Restore_CNN_VL.glsl", "35036722733305CD4D4E57660B883BBE2569BA2914033C254327107D7B77E35E"),
    @("https://raw.githubusercontent.com/bloc97/Anime4K/v4.0.1/glsl/Restore/Anime4K_Restore_CNN_M.glsl", "67EA3ED26539E8DE3B7D307688535D2FF17E8D147E11DDA0247DA7770DBECF41"),
    @("https://raw.githubusercontent.com/bloc97/Anime4K/v4.0.1/glsl/Upscale/Anime4K_Upscale_CNN_x2_VL.glsl", "5638FE31C37C151A3443FEA3451A3EF91AF073F4DBB9615F6C0D1E29DB11493D"),
    @("https://raw.githubusercontent.com/bloc97/Anime4K/v4.0.1/glsl/Upscale/Anime4K_Upscale_CNN_x2_S.glsl", "4C53EC2E287908F7EE7BCB266B0170421626D663576468B7D7DAFC62962649A4"),
    @("https://raw.githubusercontent.com/bloc97/Anime4K/v4.0.1/glsl/Upscale/Anime4K_AutoDownscalePre_x2.glsl", "8C58291740146BD766A4D73F132775A797FE80F7D07919B5D767E27A5DC85656"),
    @("https://raw.githubusercontent.com/bloc97/Anime4K/v4.0.1/glsl/Upscale/Anime4K_AutoDownscalePre_x4.glsl", "5AF62D8CD844916DC1126613E13BAD3BEAB195787F93A71200B47C6EC78F2E41"),
    @("https://raw.githubusercontent.com/Artoriuz/ArtCNN/v1.6.2/GLSL/ArtCNN_C4F32_DS.glsl", "A04C9CBA6FBB8E6DB9239D61848390208AEDF8E348EF116E12174C803D22077E"),
    @("https://raw.githubusercontent.com/Artoriuz/ArtCNN/v1.6.2/GLSL/ArtCNN_C4F16_DS.glsl", "57DF650FDDEC3969E17799F5522C9B03DD2D33B1AEACE237FEF216BF3858125A"),
    @("https://raw.githubusercontent.com/Artoriuz/glsl-chroma-from-luma-prediction/066dff964b83aa34e893de25d590874bf8c8e94f/CfL_Prediction.glsl", "F9D12D38D4BAB4D9DB266B5982C81FADCA907396212E22FD8B2C1CA2F317B716")
    )) {
    $file = Grab $shader[0] $shader[1]
    Copy-Item $file (Join-Path $shaders ([IO.Path]::GetFileName($shader[0]))) -Force
}

Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "Done -> third-parties/bin/, lib/ and mpv-config/shaders/"
