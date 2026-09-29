#include "core/settings.h"
#include "core/danmakuoptions.h"
#include "core/appshell.h"
#include "platform/platform.h"
#include <QNetworkProxyFactory>
#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>
#include <QNetworkProxy>
#include <QUrl>
#include <QRegularExpression>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QFont>
#include <cmath>

static const QLatin1String kDownloadDirKey("download/dir");

void Settings::syncDanmakuOptions() const {
    DanmakuOptions o;
    o.enabled      = danmakuEnabled();
    o.opacityPct   = danmakuOpacity();
    o.fontScalePct = danmakuFontScale();
    o.speedPct     = danmakuSpeed();
    o.areaPct      = danmakuArea();
    o.maxLines     = get(Config::DanmakuMaxLines);
    o.minWeight    = danmakuMinWeight();
    o.maxOnScreen  = danmakuMaxOnScreen();

    o.font         = get(Config::DanmakuFont);
    o.bold         = danmakuBold();
    o.outline      = danmakuOutline();
    o.blockScroll  = danmakuBlockScroll();
    o.blockTop     = danmakuBlockTop();
    o.blockBottom  = danmakuBlockBottom();
    o.blockColour  = danmakuBlockColour();
    o.blockRepeat  = danmakuBlockRepeat();
    DanmakuOptions::set(o);
}

Settings::Settings() : m_settings(iniPath(), QSettings::IniFormat) {
    s_preferDub.store(get(Config::PreferDub), std::memory_order_relaxed);
    s_watchedPercent.store(get(Config::WatchedPercent), std::memory_order_relaxed);
    syncDanmakuOptions();

    applyProxySettings(activeProxy());

    m_syncTimer.setSingleShot(true);
    m_syncTimer.setInterval(400);
    connect(&m_syncTimer, &QTimer::timeout, this, [this]() { m_settings.sync(); });

    // Watched from the start, which needs the file there.
    if (QFile file(iniPath()); !file.exists()) (void)file.open(QIODevice::WriteOnly);
    m_fileWatcher.addPath(iniPath());
    connect(&m_fileWatcher, &QFileSystemWatcher::fileChanged, this, [this](const QString &path) {
        // Its own writes come back here too; only a value that changed is news.
        const auto values = [this] {
            QVariantMap all;
            for (const QString &key : m_settings.allKeys()) all.insert(key, m_settings.value(key));
            return all;
        };
        const QVariantMap before = values();
        m_settings.sync();
        if (values() != before) {
            // Only the setters refresh these, so a hand-edited file leaves them stale.
            s_preferDub.store(get(Config::PreferDub), std::memory_order_relaxed);
            s_watchedPercent.store(get(Config::WatchedPercent), std::memory_order_relaxed);
            syncDanmakuOptions();
            applyProxySettings(activeProxy());
            emit appFontChanged();
            emit uiLanguageChanged();
            emit proxyEnabledChanged();
            emit proxyChanged();
            emit seekPreviewsEnabledChanged();
            emit reduceMotionChanged();
            emit settingsChanged();
        }
        // Editors that replace-on-save drop the watch.
        if (!m_fileWatcher.files().contains(path) && QFileInfo::exists(path))
            m_fileWatcher.addPath(path);
    });
}

Settings &Settings::instance() {
    static Settings s_instance;
    return s_instance;
}

static QString madeDir(const QString &path) {
    QDir().mkpath(path);
    return path;
}

QString Settings::dataDir() {
    static const QString dir = madeDir(Platform::exeDir() + QStringLiteral("/data"));
    return dir;
}

QString Settings::logDir() {
    static const QString dir = madeDir(dataDir() + QStringLiteral("/logs"));
    return dir;
}

QString Settings::iniPath() { return dataDir() + QStringLiteral("/settings.ini"); }

QString Settings::toolPath(const QString &name) { return Platform::moduleDir() + QLatin1Char('/') + name; }

void Settings::adoptLegacyState() {
    const QString root = Platform::exeDir();
    for (const QString &from : {root, root + QStringLiteral("/bin")}) {
        const QDir old(from);
        if (!old.exists(QStringLiteral("settings.ini")) && !old.exists(QStringLiteral("store.db"))) continue;
        // rename never overwrites, so whatever data\ already has stays.
        for (const char *name : {"settings.ini", "store.db", "store.db-wal", "store.db-shm", "provider-cookies.json"})
            QFile::rename(old.filePath(QLatin1String(name)), dataDir() + QLatin1Char('/') + QLatin1String(name));
        const QDir crashes(old.filePath(QStringLiteral("crashes")));
        for (const QFileInfo &file : old.entryInfoList({QStringLiteral("*.log"), QStringLiteral("*.log.1")}, QDir::Files)
                                         + crashes.entryInfoList(QDir::Files))
            QFile::rename(file.absoluteFilePath(), logDir() + QLatin1Char('/') + file.fileName());
        old.rmdir(QStringLiteral("crashes"));
        QDir(old.filePath(QStringLiteral(".tmp"))).removeRecursively();
        if (!QFileInfo::exists(dataDir() + QStringLiteral("/mpv")))
            QDir().rename(old.filePath(QStringLiteral("mpv")), dataDir() + QStringLiteral("/mpv"));
        return;
    }
}

void Settings::scheduleSync() {
    m_syncTimer.start();
}

void Settings::prependToHistory(const QString &key, const QString &value, int maxCount) {
    if (value.trimmed().isEmpty()) return;
    QStringList list = m_settings.value(key).toStringList();
    list.removeAll(value);
    list.prepend(value);
    while (list.size() > maxCount)
        list.removeLast();
    m_settings.setValue(key, list);
    m_settings.sync();
}

void Settings::removeFromHistory(const QString &key, const QString &value) {
    QStringList list = m_settings.value(key).toStringList();
    list.removeAll(value);
    m_settings.setValue(key, list);
    m_settings.sync();
}

void Settings::clearHistory(const QString &key) {
    m_settings.remove(key);
    m_settings.sync();
}

QString Settings::downloadDir() const {
    return m_settings.value(kDownloadDirKey,
                            QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)).toString();
}

void Settings::setDownloadDir(const QString &dir) {
    if (downloadDir() == dir) return;
    QFileInfo outputDir(dir);
    if (!outputDir.exists() || !outputDir.isDir() || !outputDir.isWritable()) {
        AppShell::instance().reportError(tr("Invalid output directory: %1").arg(outputDir.absoluteFilePath()));
        return;
    }
    m_settings.setValue(kDownloadDirKey, dir);
    scheduleSync();
    emit downloadDirChanged();
    emit settingsChanged();
}

void Settings::setProxy(const QString &v) {
    if (apply(Config::Proxy, v.trimmed(), &Settings::proxyChanged))
        applyProxySettings(activeProxy());
}

void Settings::setProxyEnabled(bool v) {
    if (apply(Config::ProxyEnabled, v, &Settings::proxyEnabledChanged))
        applyProxySettings(activeProxy());
}

void Settings::setPreferDub(bool v) {
    if (apply(Config::PreferDub, v, &Settings::preferDubChanged))
        s_preferDub.store(v, std::memory_order_relaxed);
}

void Settings::setUiScale(double v) {
    if (!std::isfinite(v)) return;
    v = qBound(0.8, v, 1.4);
    if (qFuzzyCompare(uiScale(), v)) return;
    set(Config::UiScale, v);
    emit uiScaleChanged();
    emit settingsChanged();
}

void Settings::resetDanmakuAppearance() {
    setDanmakuOpacity(Config::DanmakuOpacity.defaultValue);
    setDanmakuFontScale(Config::DanmakuFontScale.defaultValue);
    setDanmakuSpeed(Config::DanmakuSpeed.defaultValue);
    setDanmakuArea(Config::DanmakuArea.defaultValue);
    setDanmakuMinWeight(Config::DanmakuMinWeight.defaultValue);
    setDanmakuMaxOnScreen(Config::DanmakuMaxOnScreen.defaultValue);
    setDanmakuBold(Config::DanmakuBold.defaultValue);
    setDanmakuOutline(Config::DanmakuOutline.defaultValue);
    setDanmakuBlockScroll(Config::DanmakuBlockScroll.defaultValue);
    setDanmakuBlockTop(Config::DanmakuBlockTop.defaultValue);
    setDanmakuBlockBottom(Config::DanmakuBlockBottom.defaultValue);
    setDanmakuBlockColour(Config::DanmakuBlockColour.defaultValue);
    setDanmakuBlockRepeat(Config::DanmakuBlockRepeat.defaultValue);
}

void Settings::applyProxySettings(const QString &proxyString) {
    QByteArray proxyBytes = proxyString.toUtf8();
    qputenv("http_proxy", proxyBytes);
    qputenv("https_proxy", proxyBytes);
    qputenv("HTTP_PROXY", proxyBytes);
    qputenv("HTTPS_PROXY", proxyBytes);
    // Off means the system proxy, not "no proxy".
    if (proxyString.isEmpty()) {
        QNetworkProxyFactory::setUseSystemConfiguration(true);
        return;
    }
    const QUrl url(proxyString);
    const bool socks = url.scheme().startsWith(QLatin1String("socks"));
    QNetworkProxy::setApplicationProxy(QNetworkProxy(socks ? QNetworkProxy::Socks5Proxy : QNetworkProxy::HttpProxy,
        url.host(), quint16(url.port(socks ? 1080 : 8080)), url.userName(), url.password()));
}

QStringList Settings::fontFamilies() const {
    static const QHash<QString, QFontDatabase::WritingSystem> scripts{
        {QStringLiteral("zh_CN"), QFontDatabase::SimplifiedChinese},
        {QStringLiteral("zh_TW"), QFontDatabase::TraditionalChinese},
        {QStringLiteral("ja"),    QFontDatabase::Japanese},
        {QStringLiteral("ko"),    QFontDatabase::Korean}};
    QStringList families = QFontDatabase::families(scripts.value(uiLanguage(), QFontDatabase::Latin));
    families.removeIf([](const QString &family) {
        return family.startsWith(u'@') || QFontDatabase::isPrivateFamily(family);
    });
    return families;
}

void Settings::setAppFont(const QString &value) {
    const QString family = value.trimmed();
    if (!family.isEmpty() && !fontFamilies().contains(family)) return;
    apply(Config::AppFont, family, &Settings::appFontChanged);
}

void Settings::setUiLanguage(const QString &value) {
    if (!uiLanguages().contains(value) || !apply(Config::UiLanguage, value, &Settings::uiLanguageChanged)) return;
    const QString font = appFont();
    if (font.isEmpty() || fontFamilies().contains(font)) return;
    apply(Config::AppFont, QString(), &Settings::appFontChanged);
    AppShell::instance().reportInfo(tr("%1 cannot write this language, so the default font is back in use.").arg(font),
                                    tr("Font"));
}

bool Settings::commitMaxSpeed(const QString &text) {
    const QString normalized = text.trimmed().toUpper();
    static const QRegularExpression pattern(QStringLiteral(R"(^([0-9]+(?:\.[0-9]+)?)([KMG])$)"));
    const auto match = pattern.match(normalized);
    if (!normalized.isEmpty() && (!match.hasMatch() || !std::isfinite(match.captured(1).toDouble())
                                 || match.captured(1).toDouble() <= 0)) return false;
    apply(Config::MaxSpeed, normalized, &Settings::maxSpeedChanged);
    return true;
}

void Settings::setMaxSpeed(const QString &value) {
    commitMaxSpeed(value);
}

QString Settings::tempDir() {
    static const QString dir = madeDir(dataDir() + QStringLiteral("/cache"));
    return dir;
}

QMap<QString, QString> Settings::groupValues(const QString &group) const {
    QMap<QString, QString> map;
    m_settings.beginGroup(group);
    const auto keys = m_settings.childKeys();
    for (const auto &k : keys)
        map.insert(k, m_settings.value(k).toString());
    m_settings.endGroup();
    return map;
}
