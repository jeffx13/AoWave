<div align="center">
  <img src="resources/app.ico" alt="" width="96" height="96"/>

  <h1>AoWave</h1>

  <p><strong>Watch anime and shows from several sites in one Windows app, with mpv as the player.</strong></p>

  <p>
    <img src="https://img.shields.io/badge/platform-Windows%2010%2B-0078D6?style=flat-square" alt="Platform: Windows 10+"/>
    <img src="https://img.shields.io/badge/Qt-6.12-41CD52?style=flat-square" alt="Qt 6.12"/>
    <img src="https://img.shields.io/badge/C%2B%2B-20-00599C?style=flat-square" alt="C++20"/>
    <img src="https://img.shields.io/badge/license-MIT-blue?style=flat-square" alt="MIT licence"/>
  </p>
</div>

## Features

- Browse and search several streaming sites, mostly anime and Chinese series
- A library with lists, notes, ratings and watch history
- A notification when a show you follow gets a new episode, plus an airing calendar
- The mpv player, with picture in picture, seek previews, A-B loops and upscaling shaders
- Bilibili danmaku drawn over the video
- Subtitle search through SubDL
- Openings and endings skipped for you (AniSkip)
- Progress synced with AniList, MyAnimeList and Trakt
- Episode downloads, several at a time
- Move a show to another site and keep your progress
- Sites behind Cloudflare load through a hidden browser
- In English, 简体中文, 繁體中文, 日本語 and 한국어

## Install

1. Download the latest release and unzip it anywhere.
2. Run `AoWave.exe`.

- There's no installer. Everything stays in the folder, apart from a small Qt cache in `%LOCALAPPDATA%\AoWave` and the "Open with" entries if you turn them on in Settings.
- You need Windows 10 or later (64-bit) and working OpenGL graphics drivers.

## Where things are

```
AoWave\
  AoWave.exe
  app\          the program; leave it as it is
  data\         made on first run
    settings.ini
    store.db              library, history, downloads
    provider-cookies.json site sign-ins, encrypted
    mpv\                  mpv.conf, input.conf, shaders
    logs\                 AoWave.log and crash reports
    browser\              the hidden browser's profile
    cache\                posters, subtitles, danmaku; safe to delete
```

- Settings > Player has buttons for the data folder, the mpv folder and settings.ini.
- To back up or move AoWave, copy the whole folder. To start over, delete `data`.
- Edits to settings.ini are picked up while the app runs.
- An older version's files, next to the exe or in `bin`, move into `data` the first time you start this one.

## Accounts

All optional, in Settings > Accounts & sync.

- **Bilibili**: scan the QR code with the Bilibili app, or paste cookies from a browser. Needed for member-only episodes and progress sync.
- **AniList**: create a client at [anilist.co/settings/developer](https://anilist.co/settings/developer) with the redirect URL `http://127.0.0.1:43217`, then paste its ID and secret.
- **MyAnimeList**: create an app at [myanimelist.net/apiconfig](https://myanimelist.net/apiconfig) with the redirect URL `http://127.0.0.1:43218`, then paste its client ID.
- **Trakt**: paste the client ID and secret, then enter the code it shows at trakt.tv.
- Tokens and cookies are encrypted with Windows DPAPI, so only your Windows account can read them.

## Good to know

- Ctrl+K opens the command palette. Every shortcut is listed, and can be changed, in Settings > Keyboard shortcuts.
- Cloudflare's "checking you're human" page usually clears by itself in the background. If it doesn't within about 15 seconds, a small window shows it. Tick the box if it asks; the window closes by itself once you're through, and the site is left alone for as long as its clearance lasts.
- If a site blocks your network outright, the app tells you. A proxy (Settings > Network) is the way around it, and it covers the sites, downloads and the player.
- If your DNS can't find a site, the app asks 1.1.1.1 and 8.8.8.8 instead.
- `bilibili/proxy` in settings.ini points at a mainland relay for region-locked Bilibili shows. See `scripts/aliyun_proxy.py`.

## Building

You need Visual Studio 2022 or later with C++, Qt 6.12 for MSVC 64-bit (with Qt WebEngine and Qt Shader Tools), [vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set, CMake 3.22+ and Ninja.

```
scripts\download-tools.ps1    yt-dlp, ffmpeg, N_m3u8DL-RE and libmpv, pinned and hash-checked
scripts\build.cmd release     or debug
scripts\package.ps1           a release zip in dist\
```

- vcpkg installs libxml2, zlib and the QR code library the first time you configure.
- Build Release before trusting QML changes: QML errors only show up there.
- `cmake --install <build folder>` puts a runnable copy in `deploy` inside it.
- CI builds every push to `main` and every pull request.

## Code

- `src/providers`: one file per site. To add one, subclass `ShowProvider` and list it in `src/app/application.cpp`.
- `src/media`: the player, playlists, danmaku and skip times
- `src/net`: HTTP, cookies, the Cloudflare check and hidden browser, the HLS proxy
- `src/library`: the library, history and SQLite store
- `src/ui`: QML and the models behind it
- `src/platform`: Windows specifics: encryption, taskbar, notifications, crash reports
- `src/launcher.cpp`: the small AoWave.exe that starts the app from `app`

## Disclaimer

AoWave doesn't host anything. It reads sites anyone can open in a browser, and what you watch is up to you.

## Licence

MIT. See [LICENSE](LICENSE).
