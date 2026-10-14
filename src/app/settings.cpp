#include "app/settings.h"
#include "media/player/danmaku.h"
#include "app/mainwindow.h"
#include "system/platform.h"
#include <QNetworkProxyFactory>
#include <QCryptographicHash>
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

QString Config::linkKey(const char *prefix, const QString &link) {
    const QByteArray digest = QCryptographicHash::hash(link.toUtf8(), QCryptographicHash::Sha1).toHex();
    return QLatin1String(prefix) + QLatin1String(digest.first(16));
}

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

static std::pair<qint64, qint64> fileStamp() {
    const QFileInfo file(Settings::iniPath());
    return {file.size(), file.lastModified().toMSecsSinceEpoch()};
}

Settings::Settings() : m_settings(iniPath(), QSettings::IniFormat) {
    s_preferDub.store(get(Config::PreferDub), std::memory_order_relaxed);
    s_watchedPercent.store(get(Config::WatchedPercent), std::memory_order_relaxed);
    s_lenientTls.store(get(Config::LenientTls), std::memory_order_relaxed);
    syncDanmakuOptions();

    applyProxySettings(activeProxy());

    m_syncTimer.setSingleShot(true);
    m_syncTimer.setInterval(400);
    connect(&m_syncTimer, &QTimer::timeout, this, &Settings::syncFile);

    if (QFile file(iniPath()); !file.exists()) (void)file.open(QIODevice::WriteOnly);
    m_fileStamp = fileStamp();
    m_settings.installEventFilter(this);
    m_fileWatcher.addPath(iniPath());
    // an editor saves in steps, so the file is read once it settles
    m_reloadTimer.setSingleShot(true);
    m_reloadTimer.setInterval(300);
    connect(&m_fileWatcher, &QFileSystemWatcher::fileChanged, &m_reloadTimer, qOverload<>(&QTimer::start));
    connect(&m_reloadTimer, &QTimer::timeout, this, [this] {
        syncFile();
        if (const QString path = iniPath(); !m_fileWatcher.files().contains(path) && QFileInfo::exists(path))
            m_fileWatcher.addPath(path);
    });
}

// QSettings also syncs by itself on the event loop's next pass after a change
bool Settings::eventFilter(QObject *object, QEvent *event) {
    if (object == &m_settings && event->type() == QEvent::UpdateRequest) syncFile();
    return QObject::eventFilter(object, event);
}

// whichever sync comes first reads in another writer's edit, so each one looks for it
void Settings::syncFile() {
    const auto values = [this] {
        QVariantMap all;
        for (const QString &key : m_settings.allKeys()) all.insert(key, m_settings.value(key));
        return all;
    };
    const bool edited = fileStamp() != m_fileStamp;
    const QVariantMap before = edited ? values() : QVariantMap();
    m_settings.sync();
    m_fileStamp = fileStamp();
    if (!edited || values() == before) return;
    s_preferDub.store(get(Config::PreferDub), std::memory_order_relaxed);
    s_watchedPercent.store(get(Config::WatchedPercent), std::memory_order_relaxed);
    s_lenientTls.store(get(Config::LenientTls), std::memory_order_relaxed);
    syncDanmakuOptions();
    applyProxySettings(activeProxy());
    emit appFontChanged();
    emit uiLanguageChanged();
    emit proxyEnabledChanged();
    emit proxyChanged();
    emit lenientTlsChanged();
    emit seekPreviewsEnabledChanged();
    emit reduceMotionChanged();
    emit settingsChanged();
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
    static const QString dir = madeDir(qEnvironmentVariableIsSet("AOWAVE_DATA_DIR") ? qEnvironmentVariable("AOWAVE_DATA_DIR")
                                                                                      : Platform::exeDir() + QStringLiteral("/data"));
    return dir;
}

QString Settings::logDir() {
    static const QString dir = madeDir(dataDir() + QStringLiteral("/logs"));
    return dir;
}

QString Settings::iniPath() { return dataDir() + QStringLiteral("/settings.ini"); }

QMap<QString, QString> Settings::section(const Config::Section &path) const {
    const QString prefix = QLatin1String(path.path) + QLatin1Char('/');
    QMap<QString, QString> values;
    for (const QString &key : m_settings.allKeys()) {
        if (!key.startsWith(prefix) || key.indexOf(QLatin1Char('/'), prefix.size()) >= 0) continue;
        const QVariant value = m_settings.value(key);
        const QString text = value.metaType().id() == QMetaType::QStringList ? value.toStringList().join(QLatin1Char(',')) : value.toString();
        if (!text.isEmpty()) values.insert(key.mid(prefix.size()), text);
    }
    return values;
}

void Settings::setSection(const Config::Section &path, const QMap<QString, QString> &values) {
    const QString prefix = QLatin1String(path.path) + QLatin1Char('/');
    for (const QString &key : m_settings.allKeys())
        if (key.startsWith(prefix) && key.indexOf(QLatin1Char('/'), prefix.size()) < 0 && !values.contains(key.mid(prefix.size())))
            m_settings.remove(key);
    for (auto it = values.constBegin(); it != values.constEnd(); ++it)
        if (!it.value().isEmpty()) m_settings.setValue(prefix + it.key(), it.value());
    scheduleSync();
}

QString Settings::toolPath(const QString &name) { return Platform::moduleDir() + QLatin1Char('/') + name; }

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
    syncFile();
}

void Settings::removeFromHistory(const QString &key, const QString &value) {
    QStringList list = m_settings.value(key).toStringList();
    list.removeAll(value);
    m_settings.setValue(key, list);
    syncFile();
}

QString Settings::subdlLanguages() const {
    if (m_settings.contains(QLatin1String(Config::SubdlLanguages.path))) return get(Config::SubdlLanguages);
    static const QStringList offered{"EN", "ES", "FR", "DE", "PT", "AR", "ZH", "JA", "KO"};
    const QString code = subtitleLanguage().section(QLatin1Char('-'), 0, 0).toUpper();
    return offered.contains(code) ? code : Config::SubdlLanguages.defaultValue;
}

void Settings::setSubdlLanguages(const QString &v) {
    const QString value = v.trimmed().toUpper();
    if (value == subdlLanguages()) return;
    set(Config::SubdlLanguages, value);
    emit subdlLanguagesChanged();
    emit settingsChanged();
}

QString Settings::downloadDir() const {
    return m_settings.value(kDownloadDirKey,
                            QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)).toString();
}

void Settings::setDownloadDir(const QString &dir) {
    if (downloadDir() == dir) return;
    QFileInfo outputDir(dir);
    if (!outputDir.exists() || !outputDir.isDir() || !outputDir.isWritable()) {
        MainWindow::instance().reportError(tr("Could not use %1. Pick a folder you can write to.").arg(outputDir.absoluteFilePath()),
                                           tr("Download folder"));
        return;
    }
    m_settings.setValue(kDownloadDirKey, dir);
    scheduleSync();
    emit downloadDirChanged();
    emit settingsChanged();
}

void Settings::setProxy(const QString &v) {
    const bool wasEmpty = get(Config::Proxy).isEmpty();
    if (!apply(Config::Proxy, v.trimmed(), &Settings::proxyChanged)) return;
    if (wasEmpty && !v.trimmed().isEmpty()) apply(Config::ProxyEnabled, true, &Settings::proxyEnabledChanged);
    applyProxySettings(activeProxy());
}

void Settings::setLenientTls(bool v) {
    if (apply(Config::LenientTls, v, &Settings::lenientTlsChanged)) s_lenientTls.store(v, std::memory_order_relaxed);
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
    if (proxyString.isEmpty()) {
        QNetworkProxyFactory::setUseSystemConfiguration(true);
        return;
    }
    // a bare host:port has no scheme, which QUrl reads as an empty host
    const QUrl url(proxyString.contains(QLatin1String("://")) ? proxyString : QStringLiteral("http://") + proxyString);
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
    MainWindow::instance().reportInfo(tr("%1 does not support this language. Switched to the default font.").arg(font),
                                    tr("Font"));
}

bool Settings::commitMaxSpeed(const QString &text) {
    const QString typed = text.trimmed().toUpper();
    static const QRegularExpression pattern(QStringLiteral(R"(^([0-9]+(?:\.[0-9]+)?)\s*([KMG])(?:I?B)?(?:/S)?$)"));
    const auto match = pattern.match(typed);
    if (!typed.isEmpty() && (!match.hasMatch() || !std::isfinite(match.captured(1).toDouble())
                             || match.captured(1).toDouble() <= 0)) return false;
    apply(Config::MaxSpeed, typed.isEmpty() ? QString() : match.captured(1) + match.captured(2), &Settings::maxSpeedChanged);
    return true;
}

bool Settings::commitDownloadDir(const QString &dir) {
    const QFileInfo outputDir(dir.trimmed());
    if (!outputDir.exists() || !outputDir.isDir() || !outputDir.isWritable()) return false;
    setDownloadDir(dir.trimmed());
    return true;
}

void Settings::setMaxSpeed(const QString &value) {
    commitMaxSpeed(value);
}

QString Settings::tempDir() {
    static const QString dir = madeDir(dataDir() + QStringLiteral("/cache"));
    return dir;
}
