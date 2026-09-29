#include <QQuickWindow>
#include <QtWebEngineQuick>
#include <QApplication>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QIcon>
#include <QQmlNetworkAccessManagerFactory>
#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <QNetworkRequest>
#include <QSettings>
#include <QFileInfo>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTranslator>
#include <QThreadPool>
#include <QtQml/qqmlextensionplugin.h>
#include "app/application.h"
#include "app/singleinstance.h"
#include "core/settings.h"
#include "platform/crashhandler.h"
#include "net/cloudflare.h"
#include "net/dnsoverhttps.h"

namespace {

class UiTranslator final : public QTranslator {
public:
    using QTranslator::QTranslator;

    bool setLanguage(const QString &language) {
        m_messages = {};
        if (language == QLatin1String("en")) return true;
        QFile file(QStringLiteral(":/" MAIN_MODULE_URI "/resources/translations/%1.json").arg(language));
        if (!file.open(QIODevice::ReadOnly)) return false;
        const QJsonDocument catalog = QJsonDocument::fromJson(file.readAll());
        if (!catalog.isObject()) return false;
        m_messages = catalog.object();
        return true;
    }

    QString translate(const char *, const char *sourceText, const char *, int n) const override {
        QString translated = m_messages.value(QString::fromUtf8(sourceText)).toString();
        if (n >= 0) translated.replace(QStringLiteral("%n"), QString::number(n));
        return translated;
    }

    bool isEmpty() const override { return m_messages.isEmpty(); }

private:
    QJsonObject m_messages;
};

// Clearance, User-Agent and the DNS-over-HTTPS fallback for QML images, which bypass Client.
class ImageAccessManager : public QNetworkAccessManager {
public:
    using QNetworkAccessManager::QNetworkAccessManager;

protected:
    QNetworkReply *createRequest(Operation op, const QNetworkRequest &req, QIODevice *outgoingData) override {
        const QString host = req.url().host();
        if (op == GetOperation && Cloudflare::browserBound(host))
            return Cloudflare::createBrowserReply(req, this);

        QNetworkRequest r(req);
        bool modified = false;

        // AllAnime posters 403 without one.
        if (host.endsWith("youtube-anime.com") && !req.hasRawHeader("Referer")) {
            r.setRawHeader("Referer", "https://youtu-chan.com/");
            modified = true;
        }

        if (const QString hostUa = Cloudflare::hostUserAgent(host); !hostUa.isEmpty()) {
            r.setRawHeader("User-Agent", hostUa.toUtf8());
            modified = true;
        }

        // Runs on QML's image thread, where waiting on the first lookup of a host is fine.
        if (const QString address = DnsOverHttps::addressIfSystemFails(host); !address.isEmpty()) {
            DnsOverHttps::pin(r, address);
            modified = true;
        }

        return QNetworkAccessManager::createRequest(op, modified ? r : req, outgoingData);
    }
};

class ImageAccessManagerFactory : public QQmlNetworkAccessManagerFactory {
public:
    QNetworkAccessManager *create(QObject *parent) override {
        auto *manager = new ImageAccessManager(parent);
        manager->setCookieJar(new Cloudflare::ProxyCookieJar(manager));

        const QString cacheDir = Settings::tempDir() + QStringLiteral("/images");
        QDir().mkpath(cacheDir);
        auto *diskCache = new QNetworkDiskCache(manager);
        diskCache->setCacheDirectory(cacheDir);
        diskCache->setMaximumCacheSize(100LL * 1024 * 1024);
        manager->setCache(diskCache);
        return manager;
    }
};

}

// The App module is a static library; this pulls its types and resources into the link.
Q_IMPORT_QML_PLUGIN(AppPlugin)

// AoWave.exe, the launcher, calls this.
extern "C" __declspec(dllexport) int aowaveMain(int argc, char *argv[]) {
    CrashHandler::install();
    Settings::adoptLegacyState();

    // Decided before QGuiApplication exists, so read straight off the ini.
    {
        const bool enabled = QSettings(Settings::iniPath(), QSettings::IniFormat)
                                 .value(QStringLiteral("ui/accessibility"), false).toBool();
        qputenv("QT_ENABLE_ACCESSIBILITY", enabled ? "1" : "0");
    }

    // Qt defaults to the basic single-thread loop on Windows+GL.
    qputenv("QSG_RENDER_LOOP", "threaded");

    QGuiApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

    // Must precede QGuiApplication: without it the solver's page never commits a navigation.
    QtWebEngineQuick::initialize();

    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGLRhi);
    // Widgets as well as Quick: a site's "are you human" check is shown in a QWebEngineView.
    QApplication app(argc, argv);
    // APP_NAME is CMake's APP_DISPLAY_NAME.
    app.setApplicationName(QStringLiteral(APP_NAME));
    app.setApplicationVersion(QStringLiteral(APP_VERSION));
    app.setWindowIcon(QIcon(":/" MAIN_MODULE_URI "/resources/app.ico"));

    // arguments(), not argv: argv is in the ANSI code page, which mangles a Japanese file name.
    const QStringList arguments = QCoreApplication::arguments();
    const QString launchArgument = arguments.size() > 1 ? arguments.at(1) : QString();
    SingleInstance instance(QCoreApplication::applicationDirPath());
    if (instance.handOff(launchArgument)) return 0;
    instance.listen();

    int exitCode = 0;
    {
        Application application(launchArgument);
        CrashHandler::reportPending();
        application.setFont(":/" MAIN_MODULE_URI "/resources/app-font.ttf");

        QQmlApplicationEngine engine;
        UiTranslator translator;
        const auto updateLanguage = [&]() {
            app.removeTranslator(&translator);
            translator.setLanguage(Settings::instance().uiLanguage());
            app.installTranslator(&translator);
            engine.retranslate();
        };
        QObject::connect(&Settings::instance(), &Settings::uiLanguageChanged, &engine, updateLanguage);
        updateLanguage();
        engine.addImportPath("qrc:/" MAIN_MODULE_URI "/src/ui/qml");
        engine.setNetworkAccessManagerFactory(new ImageAccessManagerFactory);

        const QUrl url(QStringLiteral("qrc:/" MAIN_MODULE_URI "/src/ui/qml/main.qml"));
        QObject::connect(&engine, &QQmlApplicationEngine::objectCreated,
                         &app, [&url](QObject *obj, const QUrl &objUrl) {
                             if (!obj && url == objUrl)
                                 QCoreApplication::exit(-1);
                         }, Qt::QueuedConnection);
        engine.load(url);
        QWindow *window = engine.rootObjects().isEmpty()
                              ? nullptr : qobject_cast<QWindow *>(engine.rootObjects().first());
        if (window) application.attachWindow(window);
        QObject::connect(&instance, &SingleInstance::argumentReceived, &application,
                         [&application, window](const QString &argument) {
            if (!argument.isEmpty()) application.openArgument(argument);
            if (!window) return;
            window->setWindowStates(window->windowStates() & ~Qt::WindowMinimized);
            window->show();
            window->raise();
            window->requestActivate();
        });
        exitCode = app.exec();
        Cloudflare::shutdown();
    }

    // Drain while QGuiApplication still owns Qt's event infrastructure.
    Cloudflare::pageFetchPool().waitForDone();
    QThreadPool::globalInstance()->waitForDone();
    QLog::flushLog();
    return exitCode;
}
