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
- Bilibili and iyf danmaku drawn over the video
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
  licenses\     the GNU licence texts
  LICENSE, THIRD-PARTY-NOTICES.txt    the licences, see Licence below
  data\         made on first run
    settings.ini
    store.db              library, history, downloads
    provider-cookies.json site cookies and Cloudflare clearances, encrypted
    mpv\                  mpv.conf, input.conf, shaders
    logs\                 AoWave.log and crash reports
    browser\              the hidden browser's profile
    cache\                posters, subtitles, danmaku; safe to delete
```

- Settings > Player has buttons for the data folder, the mpv folder, mpv.conf, settings.ini and the licences.
- To back up or move AoWave, copy the whole folder. To start over, delete `data`.
- Edits to settings.ini are picked up while the app runs.

## Accounts

All optional, in Settings > Accounts & sync.

- **Bilibili**: scan the QR code with the Bilibili app, or paste the cookies from a browser. Needed for member-only episodes and progress sync. The card shows the membership and when the session ends.
- **爱壹帆 (iyf)**: Sign in opens iyf's own login page in a window, so the password goes to the site and not to AoWave. Needed for the higher qualities and member videos; the card shows the membership, how many days are left and when it was last checked, and renews a lapsed session by itself.
- **AniList**: create a client at [anilist.co/settings/developer](https://anilist.co/settings/developer) with the redirect URL `http://127.0.0.1:43217`, then paste its ID and secret.
- **MyAnimeList**: create an app at [myanimelist.net/apiconfig](https://myanimelist.net/apiconfig) with the redirect URL `http://127.0.0.1:43218`, then paste its client ID.
- **Trakt**: paste the client ID and secret, then enter the code it shows at trakt.tv.
- The Bilibili and iyf sessions are plain lines in settings.ini, one for each cookie or token, so you can paste or edit them there (or paste into the card, in any shape: a cookie header, `a=b; c=d`, or the JSON a site keeps). They are picked up while the app runs:
  ```
  [bilibili]
  cookies\SESSDATA=...
  cookies\bili_jct=...
  cookies\DedeUserID=...
  [iyf]
  auth\uid=...
  auth\sign=...
  auth\token=...
  ```
- AniList, MyAnimeList and Trakt tokens and the cookies sites set, Cloudflare clearances included (`provider-cookies.json`), are encrypted with Windows DPAPI, so only your Windows account can read them.

## Good to know

- Ctrl+K opens the command palette. Every shortcut is listed, and can be changed, in Settings > Keyboard shortcuts.
- Cloudflare's "checking you're human" page usually clears by itself in the background. If it doesn't within about 15 seconds, a small window shows it. Tick the box if it asks; the window closes by itself once you're through, and the site is left alone for as long as its clearance lasts.
- If a site blocks your network outright, the app tells you. A proxy (Settings > Network) is the way around it, and it covers the sites, downloads and the player.
- If your DNS can't find a site, the app asks 1.1.1.1 and 8.8.8.8 instead.
- `bilibili/proxy` in settings.ini points at a mainland relay for region-locked Bilibili shows: `http://relay:<secret>@host:9000`. See `scripts/aliyun_proxy.py`, which refuses requests without the secret.
- Settings > Network > Accept Untrusted Certificates is on by default: some stream hosts use certificates Windows does not trust. Off, every connection is verified. Anything carrying a sign-in is verified either way.

## AnimeJaNai

AI upscaling with [AnimeJaNai](https://github.com/the-database/mpv-AnimeJaNai), for an NVIDIA RTX card (DirectML on others).

1. Unpack an AnimeJaNai 3.7.x package anywhere (3.6.x works, without frame interpolation), then pick its folder in Settings > Player > AnimeJaNai. The libmpv that `scripts\download-tools.ps1` fetches is the build with its filter.
2. Pick AnimeJaNai in the Picture tab or with Ctrl+2. The first time, the filter builds its TensorRT engines (about a minute, in the background); the video plays as it is until they are ready.
3. It steps down to a lighter model by itself when frames drop, and plays a video as it is when this PC cannot keep up, at faster speeds too.
4. Picture > Frame interpolation (off by default) doubles 24 to 30 fps video with RIFE where the graphics card has the time for it (small pictures, 480p to PAL at 24 or 25 fps on an RTX 5080 Laptop), from the `animejanai\rife` models of the 3.7.x package. The player says so when it skips a video.

The chains it uses are worked out from what a frame costs on this PC, not read from a table (`src/media/render/animejanai.cpp`).

## Building

You need Visual Studio 2022 or later with C++, Qt 6.12 for MSVC 64-bit (with Qt WebEngine and Qt Shader Tools), [vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set, CMake 3.22+ and Ninja.

```
scripts\download-tools.ps1    yt-dlp, ffmpeg, N_m3u8DL-RE, libmpv and the upscaling shaders, pinned and hash-checked
scripts\build.cmd release     or debug
scripts\package.ps1           a release zip in dist\
```

- vcpkg installs libxml2, zlib and the QR code library the first time you configure.
- Build Release before trusting QML changes: QML errors only show up there.
- `cmake --install <build folder>` puts a runnable copy in `deploy` inside it.
- CI builds every push to `main` and every pull request.

## Code

- `src/providers`: the site layer. `provider.h` is the base every site implements, with the optional parts a site hands out (null when it has none): `account()` (sign-in, shown on the settings page), `filters()` (genre, year and status browsing), `watchHistory()` (the site's own record of where you are in a video) and `danmaku()` (its comments on an episode). `sites/` has a `.h` and a `.cpp` for each site (`allanime`, `anikoto`, `animepahe`, `bilibili`, `duboku`, `iyf`, `miruro`, `olevod`, `pstream`); to add one, subclass `Provider` and list it in `src/app/application.cpp`. `providerlist` is the list of them, and `showdata.h` the show and episode data and `Source`, what a site hands the player.
- `src/media`: `player/` the mpv player item, its tracks, skip times, subtitle search and danmaku (comments to ASS); `playlist/` what plays next, the choice of server, local files and yt-dlp; `render/` how it reaches the screen (`upscaler`, `animejanai`, and `videooutput` for the Direct3D composition and the shader cache).
- `src/library`: the library, history and SQLite store; `downloads/` the download queue; `trackers/` the watch-list trackers (AniList, MyAnimeList, Trakt).
- `src/net`: HTTP, cookies, the Cloudflare check and hidden browser, the HLS proxy.
- `src/ui`: the QML (`main.qml`, the `Ui` and `Theme` singletons, `Components/`, `Pages/`, `Player/`, `shaders/`) and the models behind it (`CurrentShow`, search results, shortcuts).
- `src/app`: `Application`, `MainWindow` (pages, notifications), settings, the log and the small shared headers.
- `src/system`: Windows specifics: encryption, taskbar, media keys, notifications, crash reports, Discord, the single-instance guard.
- `entry/`: the small AoWave.exe that starts the app from `app`, and the entry point of the app itself.

## Disclaimer

AoWave doesn't host anything. It reads sites anyone can open in a browser, and what you watch is up to you.

## Licence

MIT for AoWave's own source. See [LICENSE](LICENSE).

A release also carries programs and libraries made by others, each under its own licence. libmpv, ffmpeg.exe, yt-dlp.exe and Qt's QtWebEngineProcess.exe are GPL, so the release zip as a whole is distributed under the terms of the GNU General Public License, version 3 or later; the source in this repository stays MIT. [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) lists everything the release carries, with its licence and where its source is. The release has it as `THIRD-PARTY-NOTICES.txt`, and the app opens it from Settings > Player > Licences.
