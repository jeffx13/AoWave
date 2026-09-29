#pragma once
#include <QString>

namespace Config {

template <typename T>
struct Key {
    const char *path;
    T defaultValue;
};

inline const Key<int>     Volume      {"player/volume", 100};
inline const Key<double>  Speed       {"player/speed", 1.0};
inline const Key<bool>    Ytdl        {"player/ytdl", false};
inline const Key<int>     SubFontSize {"player/subFontSize", 40};
inline const Key<int>     SubPos      {"player/subPos", 100};
// How plain-text subtitles look; styled (ASS) ones keep their own look unless overridden.
inline const Key<QString> SubFont           {"subtitles/font", QString()};   // empty: mpv's own
inline const Key<QString> SubColor          {"subtitles/color", QStringLiteral("#FFFFFF")};
inline const Key<bool>    SubBold           {"subtitles/bold", false};
inline const Key<double>  SubOutline        {"subtitles/outline", 1.5};
inline const Key<double>  SubShadow         {"subtitles/shadow", 0.0};
inline const Key<int>     SubBackground     {"subtitles/background", 0};   // box opacity in %, 0 for none
inline const Key<bool>    SubOverrideStyled {"subtitles/overrideStyled", false};
// Picture adjustments for every video; mpv's scales, 0 is unchanged.
inline const Key<int>     PictureBrightness {"picture/brightness", 0};   // -100..100
inline const Key<int>     PictureContrast   {"picture/contrast", 0};
inline const Key<int>     PictureSaturation {"picture/saturation", 0};
inline const Key<int>     PictureGamma      {"picture/gamma", 0};
inline const Key<double>  PictureSharpen    {"picture/sharpen", 0.0};    // 0..1
inline const Key<bool>    PictureDeband     {"picture/deband", false};
inline const Key<bool>    PictureTab        {"picture/tab", false};   // off: no tab, and no adjustments
inline const Key<bool>    PreferDub   {"player/preferDub", false};
inline const Key<bool>    AniSkip     {"player/aniskip", true};
inline const Key<bool>    AniSkipAuto {"player/aniskipAuto", false};
inline const Key<bool>    NormalizeAudio {"player/normalizeAudio", false};
inline const Key<int>     WatchedPercent {"player/watchedPercent", 80};

inline const Key<bool>    DanmakuEnabled     {"danmaku/enabled", true};
inline const Key<int>     DanmakuOpacity     {"danmaku/opacity", 80};        // %
inline const Key<int>     DanmakuFontScale   {"danmaku/fontScale", 100};     // % of the 48px@1080p base
inline const Key<int>     DanmakuSpeed       {"danmaku/speed", 100};         // % - higher crosses faster
inline const Key<int>     DanmakuArea        {"danmaku/area", 85};           // % of frame height usable
inline const Key<int>     DanmakuMaxLines    {"danmaku/maxLines", 0};        // 0 = derive from area
inline const Key<int>     DanmakuMinWeight   {"danmaku/minWeight", 0};       // 0 = off; bilibili weights run 1..11
inline const Key<int>     DanmakuMaxOnScreen {"danmaku/maxOnScreen", 60};    // 0 = unlimited
inline const Key<QString> DanmakuFont        {"danmaku/font", QStringLiteral("Microsoft YaHei")};
inline const Key<bool>    DanmakuBold        {"danmaku/bold", false};
inline const Key<int>     DanmakuOutline     {"danmaku/outline", 1};         // 0 none, 1 outline, 2 outline + shadow
inline const Key<bool>    DanmakuBlockScroll {"danmaku/blockScroll", false};
inline const Key<bool>    DanmakuBlockTop    {"danmaku/blockTop", false};
inline const Key<bool>    DanmakuBlockBottom {"danmaku/blockBottom", false};
inline const Key<bool>    DanmakuBlockColour {"danmaku/blockColour", false}; // force coloured comments to white
inline const Key<bool>    DanmakuBlockRepeat {"danmaku/blockRepeat", true};

inline const Key<int>     SkipOPStart  {"skip/fallbackOPStart", 0};
inline const Key<int>     SkipOPLength {"skip/fallbackOPLength", 90};
inline const Key<int>     SkipEDLength {"skip/fallbackEDLength", 90};

inline const Key<QString> ThemeName   {"ui/theme", QStringLiteral("nightfall")};
inline const Key<QString> AccentColor {"ui/accent", QString()};   // empty = use the theme's own accent
inline const Key<double>  UiScale     {"ui/scale", 1.0};
inline const Key<int>     CardSize    {"ui/cardSize", 100};   // % of the default cover width
inline const Key<bool>    LibraryRows {"ui/libraryRows", false};   // the library as a list, not a grid
inline const Key<bool>    UnwatchedOnly {"library/unwatchedOnly", false};
inline const Key<bool>    MiniPlayer  {"player/miniPlayer", true};   // the video in a corner of other pages
inline const Key<int>     MiniCorner  {"player/miniCorner", 3};      // 0 top left, 1 top right, 2 bottom left, 3 bottom right
inline const Key<int>     MiniWidth   {"player/miniWidth", 400};
inline const Key<bool>    AutoPip     {"player/autoPip", true};   // minimising while playing goes to PiP
inline const Key<QString> AppFont     {"ui/font", QString()};
// Always written back on close.
inline const Key<int>     WindowWidth  {"ui/windowWidth", 1280};
inline const Key<int>     WindowHeight {"ui/windowHeight", 800};
inline const Key<QString> UiLanguage  {"ui/language", QStringLiteral("en")};
inline const Key<bool>    SeekPreviews {"player/seekPreviews", true};
inline const Key<bool>    ReduceMotion {"ui/reduceMotion", false};
// Off by default: the accessibility bridge costs startup time.
inline const Key<bool>    Accessibility {"ui/accessibility", false};

inline const Key<bool>    MpvLog      {"logging/mpv", true};

inline const Key<QString> Proxy         {"network/proxy", QString()};
inline const Key<bool>    ProxyEnabled  {"network/proxyEnabled", false};   // the url is kept while off
inline const Key<bool>    LimitCache    {"network/limit_cache", false};
inline const Key<qint64>  ForwardCache  {"network/forward_cache", 0};
inline const Key<qint64>  BackwardCache {"network/backward_cache", 0};

inline const Key<QString> MaxSpeed      {"download/maxSpeed", QString()};
inline const Key<int>     MaxDownloads  {"download/maxConcurrent", 4};
inline const Key<int>     DownloadMaxHeight {"download/maxHeight", 0};   // 0: the tallest there is

inline const Key<QString> UpdateDismissed {"update/dismissed", QString()};   // a release not to offer again

inline const Key<QString> SubdlApiKey {"subtitles/subdlApiKey", QString()};
inline const Key<QString> SubdlLanguages {"subtitles/subdlLanguages", QStringLiteral("EN")};

// A public app identifier, safe to ship.
inline const Key<bool>    DiscordEnabled  {"discord/enabled", true};
inline const Key<QString> DiscordClientId {"discord/clientId", QStringLiteral("1518260245552566283")};

inline const Key<bool>    BilibiliSync    {"bilibili/syncProgress", false};

// A Windows notification when a library show gets a new episode; the bell has it either way.
inline const Key<bool>    EpisodeNotifications {"notifications/newEpisodes", true};

// Off by default: pushing to a public watch list unasked is a surprise.
inline const Key<bool>    TrackerAutoPush {"trackers/autoPush", false};

inline QString skipProfile(const QString &showLink) {
    return QStringLiteral("skip/") + QString::number(qHash(showLink));
}

inline QString episodeSub(const QString &episodeLink) {
    return QStringLiteral("subtitles/ep") + QString::number(qHash(episodeLink));
}

inline QString skipMal(const QString &showLink) {
    return QStringLiteral("skipmal/") + QString::number(qHash(showLink));
}

}
