#pragma once
#include <QObject>
#include <QSettings>
#include <QFileSystemWatcher>
#include <QTimer>
#include <QVariant>
#include <QMap>
#include <QString>
#include <QCoreApplication>
#include <atomic>
#include "core/qmlsingleton.h"
#include "core/config.h"

class Settings : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool    mpvLogEnabled   READ mpvLogEnabled   WRITE setMpvLogEnabled   NOTIFY mpvLogEnabledChanged)
    Q_PROPERTY(bool    mpvYtdlEnabled  READ mpvYtdlEnabled  WRITE setMpvYtdlEnabled  NOTIFY mpvYtdlEnabledChanged)
    Q_PROPERTY(QString proxy           READ proxy           WRITE setProxy           NOTIFY proxyChanged)
    Q_PROPERTY(bool    proxyEnabled    READ proxyEnabled    WRITE setProxyEnabled    NOTIFY proxyEnabledChanged)
    Q_PROPERTY(QString downloadDir     READ downloadDir     WRITE setDownloadDir     NOTIFY downloadDirChanged)
    Q_PROPERTY(QString maxSpeed        READ maxSpeed        WRITE setMaxSpeed        NOTIFY maxSpeedChanged)
    Q_PROPERTY(int downloadMaxHeight READ downloadMaxHeight WRITE setDownloadMaxHeight NOTIFY downloadMaxHeightChanged)
    Q_PROPERTY(int     subFontSize     READ subFontSize     WRITE setSubFontSize     NOTIFY subFontSizeChanged)
    Q_PROPERTY(int     subPos          READ subPos          WRITE setSubPos          NOTIFY subPosChanged)
    Q_PROPERTY(QString subFont         READ subFont         WRITE setSubFont         NOTIFY subStyleChanged)
    Q_PROPERTY(QString subColor        READ subColor        WRITE setSubColor        NOTIFY subStyleChanged)
    Q_PROPERTY(bool    subBold         READ subBold         WRITE setSubBold         NOTIFY subStyleChanged)
    Q_PROPERTY(double  subOutline      READ subOutline      WRITE setSubOutline      NOTIFY subStyleChanged)
    Q_PROPERTY(double  subShadow       READ subShadow       WRITE setSubShadow       NOTIFY subStyleChanged)
    Q_PROPERTY(int     subBackground   READ subBackground   WRITE setSubBackground   NOTIFY subStyleChanged)
    Q_PROPERTY(bool    subOverrideStyled READ subOverrideStyled WRITE setSubOverrideStyled NOTIFY subStyleChanged)
    // brightness, contrast, saturation, gamma (ints), sharpen (real), deband (bool)
    Q_PROPERTY(QVariantMap picture     READ picture         WRITE setPicture         NOTIFY pictureChanged)
    Q_PROPERTY(bool    pictureTab      READ pictureTab      WRITE setPictureTab      NOTIFY pictureChanged)
    Q_PROPERTY(bool    preferDub       READ preferDub       WRITE setPreferDub       NOTIFY preferDubChanged)
    Q_PROPERTY(bool    aniskipEnabled  READ aniskipEnabled  WRITE setAniskipEnabled  NOTIFY aniskipEnabledChanged)
    Q_PROPERTY(bool    aniskipAuto     READ aniskipAuto     WRITE setAniskipAuto     NOTIFY aniskipAutoChanged)
    Q_PROPERTY(bool    normalizeAudio  READ normalizeAudio  WRITE setNormalizeAudio  NOTIFY normalizeAudioChanged)
    Q_PROPERTY(int     watchedPercent  READ watchedPercent  WRITE setWatchedPercent  NOTIFY watchedPercentChanged)
    Q_PROPERTY(bool    danmakuEnabled     READ danmakuEnabled     WRITE setDanmakuEnabled     NOTIFY danmakuEnabledChanged)
    Q_PROPERTY(int     danmakuOpacity     READ danmakuOpacity     WRITE setDanmakuOpacity     NOTIFY danmakuOpacityChanged)
    Q_PROPERTY(int     danmakuFontScale   READ danmakuFontScale   WRITE setDanmakuFontScale   NOTIFY danmakuFontScaleChanged)
    Q_PROPERTY(int     danmakuSpeed       READ danmakuSpeed       WRITE setDanmakuSpeed       NOTIFY danmakuSpeedChanged)
    Q_PROPERTY(int     danmakuArea        READ danmakuArea        WRITE setDanmakuArea        NOTIFY danmakuAreaChanged)
    Q_PROPERTY(int     danmakuMinWeight   READ danmakuMinWeight   WRITE setDanmakuMinWeight   NOTIFY danmakuMinWeightChanged)
    Q_PROPERTY(int     danmakuMaxOnScreen READ danmakuMaxOnScreen WRITE setDanmakuMaxOnScreen NOTIFY danmakuMaxOnScreenChanged)
    Q_PROPERTY(bool    danmakuBold        READ danmakuBold        WRITE setDanmakuBold        NOTIFY danmakuBoldChanged)
    Q_PROPERTY(int     danmakuOutline     READ danmakuOutline     WRITE setDanmakuOutline     NOTIFY danmakuOutlineChanged)
    Q_PROPERTY(bool    danmakuBlockScroll READ danmakuBlockScroll WRITE setDanmakuBlockScroll NOTIFY danmakuBlockScrollChanged)
    Q_PROPERTY(bool    danmakuBlockTop    READ danmakuBlockTop    WRITE setDanmakuBlockTop    NOTIFY danmakuBlockTopChanged)
    Q_PROPERTY(bool    danmakuBlockBottom READ danmakuBlockBottom WRITE setDanmakuBlockBottom NOTIFY danmakuBlockBottomChanged)
    Q_PROPERTY(bool    danmakuBlockColour READ danmakuBlockColour WRITE setDanmakuBlockColour NOTIFY danmakuBlockColourChanged)
    Q_PROPERTY(bool    danmakuBlockRepeat READ danmakuBlockRepeat WRITE setDanmakuBlockRepeat NOTIFY danmakuBlockRepeatChanged)
    Q_PROPERTY(bool    discordEnabled  READ discordEnabled  WRITE setDiscordEnabled  NOTIFY discordEnabledChanged)
    Q_PROPERTY(bool    episodeNotifications READ episodeNotifications WRITE setEpisodeNotifications NOTIFY episodeNotificationsChanged)
    Q_PROPERTY(bool    bilibiliSync    READ bilibiliSync    WRITE setBilibiliSync    NOTIFY bilibiliSyncChanged)
    Q_PROPERTY(bool    trackerAutoPush READ trackerAutoPush WRITE setTrackerAutoPush NOTIFY trackerAutoPushChanged)
    Q_PROPERTY(QString themeName       READ themeName       WRITE setThemeName       NOTIFY themeNameChanged)
    Q_PROPERTY(QString accentColor     READ accentColor     WRITE setAccentColor     NOTIFY accentColorChanged)
    Q_PROPERTY(double  uiScale         READ uiScale         WRITE setUiScale         NOTIFY uiScaleChanged)
    Q_PROPERTY(int     cardSize        READ cardSize        WRITE setCardSize        NOTIFY cardSizeChanged)
    Q_PROPERTY(bool    libraryRows     READ libraryRows     WRITE setLibraryRows     NOTIFY cardSizeChanged)
    Q_PROPERTY(bool    miniPlayer      READ miniPlayer      WRITE setMiniPlayer      NOTIFY miniPlayerChanged)
    Q_PROPERTY(int     miniCorner      READ miniCorner      WRITE setMiniCorner      NOTIFY miniPlayerChanged)
    Q_PROPERTY(int     miniWidth       READ miniWidth       WRITE setMiniWidth       NOTIFY miniPlayerChanged)
    Q_PROPERTY(bool    autoPip         READ autoPip         WRITE setAutoPip         NOTIFY miniPlayerChanged)
    Q_PROPERTY(QString appFont READ appFont WRITE setAppFont NOTIFY appFontChanged)
    Q_PROPERTY(int  windowWidth  READ windowWidth  WRITE setWindowWidth  NOTIFY windowSizeChanged)
    Q_PROPERTY(int  windowHeight READ windowHeight WRITE setWindowHeight NOTIFY windowSizeChanged)
    Q_PROPERTY(QStringList fontFamilies READ fontFamilies NOTIFY uiLanguageChanged)
    Q_PROPERTY(QString uiLanguage READ uiLanguage WRITE setUiLanguage NOTIFY uiLanguageChanged)
    Q_PROPERTY(bool seekPreviewsEnabled READ seekPreviewsEnabled WRITE setSeekPreviewsEnabled NOTIFY seekPreviewsEnabledChanged)
    Q_PROPERTY(bool reduceMotion READ reduceMotion WRITE setReduceMotion NOTIFY reduceMotionChanged)
    Q_PROPERTY(bool accessibility READ accessibility WRITE setAccessibility NOTIFY accessibilityChanged)
    Q_PROPERTY(QString subdlApiKey READ subdlApiKey WRITE setSubdlApiKey NOTIFY subdlApiKeyChanged)
    Q_PROPERTY(QString subdlLanguages READ subdlLanguages WRITE setSubdlLanguages NOTIFY subdlLanguagesChanged)
    Q_PROPERTY(QString path            READ iniPath         CONSTANT)
    Q_PROPERTY(QString dataDir         READ dataDir         CONSTANT)

public:
    static Settings &instance();

    template <typename T>
    T get(const Config::Key<T> &key) const {
        return m_settings.value(QLatin1String(key.path), QVariant::fromValue(key.defaultValue)).template value<T>();
    }
    template <typename T>
    void set(const Config::Key<T> &key, const T &value) {
        m_settings.setValue(QLatin1String(key.path), QVariant::fromValue(value));
        scheduleSync();
    }

    // What the app keeps (settings, library, cookies, mpv config, logs, cache) is in data\ beside
    // AoWave.exe, where people look for it. Each is created on first use.
    static QString dataDir();
    static QString logDir();
    static QString iniPath();
    // One of the app's own programs (yt-dlp.exe), which sit beside its binaries.
    static QString toolPath(const QString &name);
    // An older version kept its state beside the exe, in bin\ for a release; moves it into
    // dataDir(). Before anything reads it.
    static void adoptLegacyState();

    Q_INVOKABLE QVariant value(const QString &key, const QVariant &defaultValue = {}) const {
        return m_settings.value(key, defaultValue);
    }
    Q_INVOKABLE void setValue(const QString &key, const QVariant &value) {
        m_settings.setValue(key, value);
        scheduleSync();
    }
    void remove(const QString &key) {
        m_settings.remove(key);
        scheduleSync();
    }

    Q_INVOKABLE void prependToHistory(const QString &key, const QString &value, int maxCount = 10);
    Q_INVOKABLE void removeFromHistory(const QString &key, const QString &value);
    Q_INVOKABLE void clearHistory(const QString &key);

    bool mpvLogEnabled() const  { return get(Config::MpvLog); }
    bool mpvYtdlEnabled() const { return get(Config::Ytdl); }
    QString proxy() const       { return get(Config::Proxy); }
    bool proxyEnabled() const   { return get(Config::ProxyEnabled); }
    // Empty unless a url is set and the switch is on.
    QString activeProxy() const { return proxyEnabled() ? proxy().trimmed() : QString(); }
    QString maxSpeed() const    { return get(Config::MaxSpeed); }
    int downloadMaxHeight() const { return get(Config::DownloadMaxHeight); }
    int subFontSize() const     { return get(Config::SubFontSize); }
    int subPos() const          { return get(Config::SubPos); }
    QString subFont() const     { return get(Config::SubFont); }
    QString subColor() const    { return get(Config::SubColor); }
    bool subBold() const        { return get(Config::SubBold); }
    double subOutline() const   { return get(Config::SubOutline); }
    double subShadow() const    { return get(Config::SubShadow); }
    int subBackground() const   { return get(Config::SubBackground); }
    bool subOverrideStyled() const { return get(Config::SubOverrideStyled); }
    void setSubFont(const QString &v) { apply(Config::SubFont, v, &Settings::subStyleChanged); }
    void setSubColor(const QString &v) { apply(Config::SubColor, v, &Settings::subStyleChanged); }
    void setSubBold(bool v)     { apply(Config::SubBold, v, &Settings::subStyleChanged); }
    void setSubOutline(double v) { apply(Config::SubOutline, qBound(0.0, v, 6.0), &Settings::subStyleChanged); }
    void setSubShadow(double v) { apply(Config::SubShadow, qBound(0.0, v, 6.0), &Settings::subStyleChanged); }
    void setSubBackground(int v) { apply(Config::SubBackground, qBound(0, v, 100), &Settings::subStyleChanged); }
    void setSubOverrideStyled(bool v) { apply(Config::SubOverrideStyled, v, &Settings::subStyleChanged); }
    bool pictureTab() const     { return get(Config::PictureTab); }
    void setPictureTab(bool v)  { apply(Config::PictureTab, v, &Settings::pictureChanged); }
    QVariantMap picture() const {
        return {{"brightness", get(Config::PictureBrightness)}, {"contrast", get(Config::PictureContrast)},
                {"saturation", get(Config::PictureSaturation)}, {"gamma", get(Config::PictureGamma)},
                {"sharpen", get(Config::PictureSharpen)}, {"deband", get(Config::PictureDeband)}};
    }
    // Only the keys given change, so a preset and a single slider both write through here.
    void setPicture(const QVariantMap &values) {
        const auto level = [&](const char *name, const Config::Key<int> &key) {
            if (values.contains(QLatin1String(name))) set(key, qBound(-100, values.value(QLatin1String(name)).toInt(), 100));
        };
        level("brightness", Config::PictureBrightness);
        level("contrast", Config::PictureContrast);
        level("saturation", Config::PictureSaturation);
        level("gamma", Config::PictureGamma);
        if (values.contains(QStringLiteral("sharpen")))
            set(Config::PictureSharpen, qBound(0.0, values.value(QStringLiteral("sharpen")).toDouble(), 1.0));
        if (values.contains(QStringLiteral("deband")))
            set(Config::PictureDeband, values.value(QStringLiteral("deband")).toBool());
        emit pictureChanged();
    }
    Q_INVOKABLE void resetSubStyle() {
        for (const char *key : {Config::SubFont.path, Config::SubColor.path, Config::SubBold.path, Config::SubOutline.path,
                                Config::SubShadow.path, Config::SubBackground.path, Config::SubOverrideStyled.path})
            remove(QLatin1String(key));
        emit subStyleChanged();
    }
    bool aniskipEnabled() const { return get(Config::AniSkip); }
    bool aniskipAuto() const    { return get(Config::AniSkipAuto); }
    bool normalizeAudio() const { return get(Config::NormalizeAudio); }
    int watchedPercent() const  { return s_watchedPercent.load(std::memory_order_relaxed); }
    bool discordEnabled() const { return get(Config::DiscordEnabled); }
    bool episodeNotifications() const { return get(Config::EpisodeNotifications); }
    bool bilibiliSync() const   { return get(Config::BilibiliSync); }
    bool trackerAutoPush() const { return get(Config::TrackerAutoPush); }
    QString themeName() const   { return get(Config::ThemeName); }
    QString accentColor() const { return get(Config::AccentColor); }
    double uiScale() const      { return get(Config::UiScale); }
    int cardSize() const        { return get(Config::CardSize); }
    void setCardSize(int v)     { apply(Config::CardSize, qBound(60, v, 200), &Settings::cardSizeChanged); }
    bool libraryRows() const    { return get(Config::LibraryRows); }
    void setLibraryRows(bool v) { apply(Config::LibraryRows, v, &Settings::cardSizeChanged); }
    bool miniPlayer() const     { return get(Config::MiniPlayer); }
    void setMiniPlayer(bool v)  { apply(Config::MiniPlayer, v, &Settings::miniPlayerChanged); }
    int miniCorner() const      { return get(Config::MiniCorner); }
    void setMiniCorner(int v)   { apply(Config::MiniCorner, qBound(0, v, 3), &Settings::miniPlayerChanged); }
    int miniWidth() const       { return get(Config::MiniWidth); }
    void setMiniWidth(int v)    { apply(Config::MiniWidth, qBound(200, v, 1600), &Settings::miniPlayerChanged); }
    bool autoPip() const        { return get(Config::AutoPip); }
    void setAutoPip(bool v)     { apply(Config::AutoPip, v, &Settings::miniPlayerChanged); }
    QString appFont() const { return get(Config::AppFont); }
    QStringList fontFamilies() const;
    // "en" is the source language.
    static QStringList uiLanguages() { return {"en", "zh_CN", "zh_TW", "ja", "ko"}; }
    QString uiLanguage() const {
        const QString value = get(Config::UiLanguage);
        return uiLanguages().contains(value) ? value : QStringLiteral("en");
    }
    bool seekPreviewsEnabled() const { return get(Config::SeekPreviews); }
    // Clamped on read too: a preset from a large monitor would open off-screen on a laptop.
    int  windowWidth() const  { return qMax(kMinWindowWidth,  get(Config::WindowWidth)); }
    int  windowHeight() const { return qMax(kMinWindowHeight, get(Config::WindowHeight)); }
    static constexpr int kMinWindowWidth  = 960;
    static constexpr int kMinWindowHeight = 600;
    bool reduceMotion() const { return get(Config::ReduceMotion); }
    // Also read off the ini in main(), before this singleton exists.
    bool accessibility() const { return get(Config::Accessibility); }
    QString downloadDir() const;

    // Atomic: the server selector worker reads it off the main thread.
    bool preferDub() const      { return s_preferDub.load(std::memory_order_relaxed); }

    double watchedFraction() const { return qBound(1, watchedPercent(), 100) / 100.0; }

    QString subdlApiKey() const    { return get(Config::SubdlApiKey); }
    QString subdlLanguages() const { return get(Config::SubdlLanguages); }

    bool danmakuEnabled() const     { return get(Config::DanmakuEnabled); }
    int  danmakuOpacity() const     { return get(Config::DanmakuOpacity); }
    int  danmakuFontScale() const   { return get(Config::DanmakuFontScale); }
    int  danmakuSpeed() const       { return get(Config::DanmakuSpeed); }
    int  danmakuArea() const        { return get(Config::DanmakuArea); }
    int  danmakuMinWeight() const   { return get(Config::DanmakuMinWeight); }
    int  danmakuMaxOnScreen() const { return get(Config::DanmakuMaxOnScreen); }
    bool danmakuBold() const        { return get(Config::DanmakuBold); }
    int  danmakuOutline() const     { return get(Config::DanmakuOutline); }
    bool danmakuBlockScroll() const { return get(Config::DanmakuBlockScroll); }
    bool danmakuBlockTop() const    { return get(Config::DanmakuBlockTop); }
    bool danmakuBlockBottom() const { return get(Config::DanmakuBlockBottom); }
    bool danmakuBlockColour() const { return get(Config::DanmakuBlockColour); }
    bool danmakuBlockRepeat() const { return get(Config::DanmakuBlockRepeat); }

    void setMpvLogEnabled(bool v)  { apply(Config::MpvLog, v, &Settings::mpvLogEnabledChanged); }
    void setMpvYtdlEnabled(bool v) { apply(Config::Ytdl, v, &Settings::mpvYtdlEnabledChanged); }
    void setMaxSpeed(const QString &v);
    void setDownloadMaxHeight(int v) { apply(Config::DownloadMaxHeight, v, &Settings::downloadMaxHeightChanged); }
    Q_INVOKABLE bool commitMaxSpeed(const QString &text);
    void setAppFont(const QString &v);
    void setUiLanguage(const QString &v);
    void setSeekPreviewsEnabled(bool v) { apply(Config::SeekPreviews, v, &Settings::seekPreviewsEnabledChanged); }
    void setWindowWidth(int v)  { apply(Config::WindowWidth,  qMax(kMinWindowWidth,  v), &Settings::windowSizeChanged); }
    void setWindowHeight(int v) { apply(Config::WindowHeight, qMax(kMinWindowHeight, v), &Settings::windowSizeChanged); }
    void setReduceMotion(bool v) { apply(Config::ReduceMotion, v, &Settings::reduceMotionChanged); }
    void setAccessibility(bool v) { apply(Config::Accessibility, v, &Settings::accessibilityChanged); }
    void setSubdlApiKey(const QString &v) { apply(Config::SubdlApiKey, v.trimmed(), &Settings::subdlApiKeyChanged); }
    void setSubdlLanguages(const QString &v) { apply(Config::SubdlLanguages, v.trimmed().toUpper(), &Settings::subdlLanguagesChanged); }
    void setSubPos(int v)          { apply(Config::SubPos, v, &Settings::subPosChanged); }
    void setAniskipEnabled(bool v) { apply(Config::AniSkip, v, &Settings::aniskipEnabledChanged); }
    void setAniskipAuto(bool v)    { apply(Config::AniSkipAuto, v, &Settings::aniskipAutoChanged); }
    void setNormalizeAudio(bool v) { apply(Config::NormalizeAudio, v, &Settings::normalizeAudioChanged); }
    void setWatchedPercent(int v) {
        if (apply(Config::WatchedPercent, qBound(0, v, 100), &Settings::watchedPercentChanged))
            s_watchedPercent.store(get(Config::WatchedPercent), std::memory_order_relaxed);
    }
    void setDiscordEnabled(bool v) { apply(Config::DiscordEnabled, v, &Settings::discordEnabledChanged); }
    void setEpisodeNotifications(bool v) { apply(Config::EpisodeNotifications, v, &Settings::episodeNotificationsChanged); }
    void setBilibiliSync(bool v)   { apply(Config::BilibiliSync, v, &Settings::bilibiliSyncChanged); }
    void setTrackerAutoPush(bool v) { apply(Config::TrackerAutoPush, v, &Settings::trackerAutoPushChanged); }
    void setThemeName(const QString &v)   { apply(Config::ThemeName, v, &Settings::themeNameChanged); }
    void setAccentColor(const QString &v) { apply(Config::AccentColor, v, &Settings::accentColorChanged); }
    void setProxy(const QString &v);
    void setProxyEnabled(bool v);
    void setDownloadDir(const QString &dir);
    void setUiScale(double v);
    void setPreferDub(bool v);

    void setSubFontSize(int v)        { applyDanmakuStyle(Config::SubFontSize, v, &Settings::subFontSizeChanged); }
    void setDanmakuOpacity(int v)     { applyDanmakuStyle(Config::DanmakuOpacity, qBound(10, v, 100), &Settings::danmakuOpacityChanged); }
    void setDanmakuFontScale(int v)   { applyDanmakuStyle(Config::DanmakuFontScale, qBound(50, v, 200), &Settings::danmakuFontScaleChanged); }
    void setDanmakuSpeed(int v)       { applyDanmakuStyle(Config::DanmakuSpeed, qBound(25, v, 400), &Settings::danmakuSpeedChanged); }
    void setDanmakuArea(int v)        { applyDanmakuStyle(Config::DanmakuArea, qBound(10, v, 100), &Settings::danmakuAreaChanged); }
    void setDanmakuMinWeight(int v)   { applyDanmakuStyle(Config::DanmakuMinWeight, qBound(0, v, 11), &Settings::danmakuMinWeightChanged); }
    void setDanmakuMaxOnScreen(int v) { applyDanmakuStyle(Config::DanmakuMaxOnScreen, qBound(0, v, 500), &Settings::danmakuMaxOnScreenChanged); }
    void setDanmakuBold(bool v)       { applyDanmakuStyle(Config::DanmakuBold, v, &Settings::danmakuBoldChanged); }
    void setDanmakuOutline(int v)     { applyDanmakuStyle(Config::DanmakuOutline, qBound(0, v, 2), &Settings::danmakuOutlineChanged); }
    void setDanmakuBlockScroll(bool v){ applyDanmakuStyle(Config::DanmakuBlockScroll, v, &Settings::danmakuBlockScrollChanged); }
    void setDanmakuBlockTop(bool v)   { applyDanmakuStyle(Config::DanmakuBlockTop, v, &Settings::danmakuBlockTopChanged); }
    void setDanmakuBlockBottom(bool v){ applyDanmakuStyle(Config::DanmakuBlockBottom, v, &Settings::danmakuBlockBottomChanged); }
    void setDanmakuBlockColour(bool v){ applyDanmakuStyle(Config::DanmakuBlockColour, v, &Settings::danmakuBlockColourChanged); }
    void setDanmakuBlockRepeat(bool v){ applyDanmakuStyle(Config::DanmakuBlockRepeat, v, &Settings::danmakuBlockRepeatChanged); }

    // Not in the appearance group: MpvPlayer swaps the track on it.
    void setDanmakuEnabled(bool v) {
        if (apply(Config::DanmakuEnabled, v, &Settings::danmakuEnabledChanged)) syncDanmakuOptions();
    }

    Q_INVOKABLE void resetDanmakuAppearance();

    static QString tempDir();
    QMap<QString, QString> groupValues(const QString &group) const;

signals:
    void mpvLogEnabledChanged();
    void mpvYtdlEnabledChanged();
    void proxyChanged();
    void proxyEnabledChanged();
    void downloadDirChanged();
    void maxSpeedChanged();
    void downloadMaxHeightChanged();
    void subFontSizeChanged();
    void subPosChanged();
    void subStyleChanged();
    void pictureChanged();
    void preferDubChanged();
    void aniskipEnabledChanged();
    void aniskipAutoChanged();
    void normalizeAudioChanged();
    void watchedPercentChanged();
    void danmakuEnabledChanged();
    void danmakuOpacityChanged();
    void danmakuFontScaleChanged();
    void danmakuSpeedChanged();
    void danmakuAreaChanged();
    void danmakuMinWeightChanged();
    void danmakuMaxOnScreenChanged();
    void danmakuBoldChanged();
    void danmakuOutlineChanged();
    void danmakuBlockScrollChanged();
    void danmakuBlockTopChanged();
    void danmakuBlockBottomChanged();
    void danmakuBlockColourChanged();
    void danmakuBlockRepeatChanged();
    void discordEnabledChanged();
    void episodeNotificationsChanged();
    void bilibiliSyncChanged();
    void trackerAutoPushChanged();
    void themeNameChanged();
    void accentColorChanged();
    void uiScaleChanged();
    void cardSizeChanged();
    void miniPlayerChanged();
    void appFontChanged();
    void uiLanguageChanged();
    void seekPreviewsEnabledChanged();
    void windowSizeChanged();
    void reduceMotionChanged();
    void accessibilityChanged();
    void subdlApiKeyChanged();
    void subdlLanguagesChanged();

    void danmakuStyleChanged();
    void settingsChanged();

private:
    template <typename T, typename Signal>
    bool apply(const Config::Key<T> &key, const T &value, Signal changed) {
        if (get(key) == value) return false;
        set(key, value);
        emit (this->*changed)();
        emit settingsChanged();
        return true;
    }

    template <typename T, typename Signal>
    void applyDanmakuStyle(const Config::Key<T> &key, const T &value, Signal changed) {
        if (!apply(key, value, changed)) return;
        syncDanmakuOptions();
        emit danmakuStyleChanged();
    }

    // Danmaku extraction runs off the GUI thread.
    void syncDanmakuOptions() const;
    void applyProxySettings(const QString &proxyString);
    void scheduleSync();

    Settings();

    mutable QSettings m_settings;
    QFileSystemWatcher m_fileWatcher;
    QTimer m_syncTimer;
    static inline std::atomic<bool> s_preferDub{false};
    static inline std::atomic<int>  s_watchedPercent{Config::WatchedPercent.defaultValue};
};

DECLARE_QML_SINGLETON(Settings);
