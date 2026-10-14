# Third-party notices

AoWave's own source code is under the MIT licence (see LICENSE). A release also carries programs and libraries made by other people. They keep their own licences, listed here with the version in this release and where its source is. The texts of the GNU licences are in the licenses folder beside this file.

## The release as a whole

The release zip is distributed under the terms of the GNU General Public License, version 3 or later. AoWaveCore.dll loads libmpv-2.dll into its own process, and this libmpv is a GPL build: it contains GPL code (x264, x265, Rubber Band, libdvdread, libdvdnav) and the FFmpeg inside it is built as GPL version 3 or later. The zip also carries ffmpeg.exe and yt-dlp.exe (both GPL version 3 or later as built) and Qt's QtWebEngineProcess.exe (GPL version 3 only, with the Qt GPL exception).

The source code in the AoWave repository stays under the MIT licence, which is compatible with the GPL; the MIT notice in LICENSE is kept. Anyone who passes the zip on, or builds a binary with this libmpv, follows the GPL for that binary.

## Source code for the GPL and LGPL parts

The GPL and LGPL programs and libraries below are the projects' own builds; AoWave does not change them. The source of the version in this release is at the address given with each one. If an address stops working, open an issue at https://github.com/jeffx13/AoWave/issues and the source will be supplied. AoWave's own source is at https://github.com/jeffx13/AoWave .

The LGPL libraries (Qt, libiconv) are separate DLLs in app\bin and may be replaced with your own build of the same library.

## Programs

- libmpv-2.dll - mpv v0.41.0-dev-gd6d93599d from the AnimeJaNai fork (with the animejanai video filter), release 2026-10-07-d6d93599d5 of the-database/mpv-winbuild, a Windows x64 build made with clang. Licence: GPL-3.0-or-later as built. mpv is GPL-2.0-or-later, this build was not made with -Dgpl=false, and its FFmpeg is built as GPL version 3 or later; the filter file vf_animejanai.c is LGPL-2.1-or-later. Source: mpv https://github.com/the-database/mpv/tree/d6d93599d5 ; FFmpeg (N-127231-gc0c7684d0) https://github.com/FFmpeg/FFmpeg/commit/c0c7684d0 ; libass fork (ISC) https://github.com/the-database/libass ; build recipe https://github.com/shinchiro/mpv-winbuild-cmake, run by https://github.com/the-database/mpv-winbuild ; release https://github.com/the-database/mpv-winbuild/releases/tag/2026-10-07-d6d93599d5 . Also inside it: x264 and x265 (GPL-2.0-or-later), Rubber Band, libdvdread and libdvdnav (GPL-2.0-or-later), libbluray, libssh, libplacebo and FriBidi (LGPL-2.1-or-later), OpenAL Soft (LGPL), FreeType (FTL), HarfBuzz (MIT), LuaJIT (MIT), Little CMS (MIT), libarchive (BSD-2-Clause) and others. The build recipe takes these libraries from their own projects on the build day; their source is at those projects.
- ffmpeg.exe - FFmpeg 9.0.2 "essentials" build for Windows by gyan.dev, static. Licence: GPL-3.0-or-later (built with --enable-gpl --enable-version3 and without --enable-nonfree; it includes x264, x265, Xvid, Rubber Band, vidstab, VMAF, OpenMPT and GnuTLS, among others). Source: FFmpeg commit 946fcce07b6dcd0331c8cc609192aeff5e1924f8 (https://github.com/FFmpeg/FFmpeg/tree/946fcce07b6dcd0331c8cc609192aeff5e1924f8), named in the build notes at https://github.com/GyanD/codexffmpeg/releases/tag/9.0.2 ; builds https://www.gyan.dev/ffmpeg/builds/ .
- yt-dlp.exe - yt-dlp 2026.08.19. yt-dlp itself is under the Unlicense, but this Windows executable is a PyInstaller bundle that includes GPL-3.0-or-later code, and yt-dlp states that the combined executable is GPL-3.0-or-later. It also bundles the Python runtime (PSF-2.0), GNU Readline (GPL-3.0-or-later), mutagen (GPL-2.0-or-later), libidn2 and libunistring (LGPL-3.0-or-later) and others, listed with their licences at https://github.com/yt-dlp/yt-dlp/blob/2026.08.19/THIRD_PARTY_LICENSES.txt . Source: https://github.com/yt-dlp/yt-dlp/tree/2026.08.19 ; the yt-dlp maintainers supply the source of the bundled components on request (maintainers@yt-dlp.org).
- N_m3u8DL-RE.exe - N_m3u8DL-RE v0.6.0-beta (build 20260629). MIT, Copyright (c) 2022 nilaoda. The executable also contains Spectre.Console and System.CommandLine (MIT) and .NET runtime libraries (MIT). Source: https://github.com/nilaoda/N_m3u8DL-RE/tree/v0.6.0-beta

## Qt

AoWave uses Qt 6.12.0 (open-source edition, MSVC 2022 x64) as unmodified DLLs in app\bin, app\plugins, app\qml and app\resources, under the GNU Lesser General Public License version 3 (the text is in licenses\LGPL-3.0.txt, with the GPL it builds on in licenses\GPL-3.0.txt). Qt is Copyright (C) The Qt Company Ltd. and other contributors. A few Qt parts are GPL only in the open-source edition, among them QtWebEngineProcess.exe (GPL-3.0 with the Qt GPL exception). Source: https://download.qt.io/official_releases/qt/6.12/6.12.0/single/qt-everywhere-src-6.12.0.tar.xz ; Qt WebEngine commit d505fc23640d2bd6345e6483eb73daf4e19f5829 (https://code.qt.io/cgit/qt/qtwebengine.git/commit/?id=d505fc23640d2bd6345e6483eb73daf4e19f5829).

Qt WebEngine contains Chromium 140.0.7339.225 (BSD-3-Clause, Copyright 2015 The Chromium Authors; text below) and the third-party components Chromium bundles, the most restrictive of which is LGPL-2.1 (Chromium's FFmpeg without proprietary codecs, and Speech Dispatcher; text in licenses\LGPL-2.1.txt). Qt's list of them is in the sbom folder of a Qt installation (qtwebengine-chromium-webengine-6.140.0.spdx.json) and at https://doc.qt.io/qt-6/qtwebengine-licensing.html .

Libraries built into Qt itself: PCRE2 10.48, zlib 1.3.2, libpng 1.6.58, libjpeg-turbo 3.2.0, FreeType 2.14.3 (FTL), HarfBuzz 14.5.0 (MIT), double-conversion 3.4.0, SQLite 3.53.4 (public domain), md4c 0.5.3 (MIT), libwebp 1.6.0 (BSD-3-Clause), yoga 2.0.1 (MIT). Qt's complete third-party list is at https://doc.qt.io/qt-6/licenses-used-in-qt.html .

## Libraries

- libxml2 2.15.2 - MIT, Copyright (C) 1998-2012 Daniel Veillard, Copyright (C) The Libxml2 Contributors. https://gitlab.gnome.org/GNOME/libxml2
- zlib 1.3.1 - zlib licence, (C) 1995-2022 Jean-loup Gailly and Mark Adler. https://zlib.net
- GNU libiconv 1.18 (iconv-2.dll, charset-1.dll) - LGPL-2.1-or-later. Source: https://ftp.gnu.org/pub/gnu/libiconv/libiconv-1.18.tar.gz
- QR Code generator library 1.8.0 - MIT, Copyright (c) 2022 Project Nayuki. Built into AoWaveCore.dll. https://www.nayuki.io/page/qr-code-generator-library
- mpv client API headers - ISC, Copyright (C) 2017-2018 the mpv developers. Built into AoWaveCore.dll.
- The libraries from vcpkg (libxml2, zlib, libiconv, the QR code library) are the versions the baseline in vcpkg.json (c21f78db5a402441bfc8052462233a2aff079ac0) gives: https://github.com/microsoft/vcpkg/tree/c21f78db5a402441bfc8052462233a2aff079ac0/ports

## Upscaling shaders (app\bin\mpv\shaders)

The licence header stays in each file. The precompiled shader cache that a release may carry in app\bin\shader-cache is built from these files, under the same licences.

- Anime4K 4.0.1 - Anime4K_Clamp_Highlights, Anime4K_Restore_CNN_M, Anime4K_Restore_CNN_VL, Anime4K_Upscale_CNN_x2_S, Anime4K_Upscale_CNN_x2_VL: MIT, Copyright (c) 2019-2021 bloc97. Anime4K_AutoDownscalePre_x2 and Anime4K_AutoDownscalePre_x4: Unlicense (public domain). https://github.com/bloc97/Anime4K/tree/v4.0.1
- ArtCNN 1.6.2 - ArtCNN_C4F32_DS, ArtCNN_C4F16_DS: MIT, Copyright (c) 2024 Joao Chrisostomo, Kacper Michajłow. https://github.com/Artoriuz/ArtCNN/tree/v1.6.2
- CfL_Prediction (chroma from luma prediction) - MIT, Copyright (c) 2023 João Chrisóstomo. https://github.com/Artoriuz/glsl-chroma-from-luma-prediction/tree/066dff964b83aa34e893de25d590874bf8c8e94f

## Icons, fonts and images

- Lucide icons (51 of the 56 in resources\icons) - ISC, Copyright (c) Lucide Icons and Contributors. https://lucide.dev . The icons Lucide took from Feather are MIT, Copyright (c) Cole Bemis.
- The logos of AniList, MyAnimeList, IMDb and Douban (resources\images\links) belong to their owners.
- Licence not recorded: the interface font resources\app-font.ttf (named QTxiaotu, 千图小兔体, in its name table); resources\images\periodic-table.jpg; resources\images\error_image.png; resources\app.ico; the icons arrow-down, arrow-up, download-done, heart and heart-filled in resources\icons.

## Microsoft runtime files

The Visual C++ runtime DLLs (msvcp140*.dll, vcruntime140*.dll, concrt140.dll, vccorlib140.dll) come from the Visual Studio 2022 redistributable files, and d3dcompiler_47.dll from the Windows SDK. They belong to Microsoft and are redistributed under those products' terms for distributable code.

## Not included

The AnimeJaNai package (aji.dll, the TensorRT and DirectML runtimes, the ONNX models) is downloaded by the user, not shipped, and has its own licences. NVIDIA Video Super Resolution is part of the graphics driver.

## Licence texts

MIT licence (the copyright holders are named with each component above):

    Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

ISC licence (the copyright holders are named with each component above):

    Permission to use, copy, modify, and/or distribute this software for any purpose with or without fee is hereby granted, provided that the above copyright notice and this permission notice appear in all copies.

    THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

Chromium (BSD-3-Clause):

    Copyright 2015 The Chromium Authors

    Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

       * Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
       * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.
       * Neither the name of Google LLC nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

The GNU General Public License version 3, the GNU Lesser General Public License version 3 and version 2.1 are in the licenses folder (GPL-3.0.txt, LGPL-3.0.txt, LGPL-2.1.txt).
