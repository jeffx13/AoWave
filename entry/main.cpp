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
#include <QFileInfo>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTranslator>
#include <QThreadPool>
#include <QtQml/qqmlextensionplugin.h>
#include "app/application.h"
#include "media/player/mpvplayer.h"
#include "media/render/videooutput.h"
#include "media/playlist/servers.h"
#include "system/platform.h"
#include "app/settings.h"
#include "app/logger.h"
#include "net/cloudflare.h"
#include "net/client.h"
#include <windows.h>

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

class ImageAccessManager : public QNetworkAccessManager {
public:
    using QNetworkAccessManager::QNetworkAccessManager;

protected:
    QNetworkReply *createRequest(Operation op, const QNetworkRequest &req, QIODevice *outgoingData) override {
        if (op == GetOperation && Cloudflare::browserBound(req.url().host()))
            return Cloudflare::createBrowserReply(req, this);
        return QNetworkAccessManager::createRequest(op, imageRequest(req), outgoingData);
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

Q_IMPORT_QML_PLUGIN(AppPlugin)

extern "C" __declspec(dllexport) int aowaveMain(int argc, char *argv[]) {
    CrashHandler::install();
    Platform::keepStateInAppFolder(Settings::tempDir());
    VideoOutput::loadShaderCompiler();
    QCoreApplication::setAttribute(Qt::AA_DisableShaderDiskCache);
    Log::forwardQtMessages();

    if (argc > 1 && qstrcmp(argv[1], "--warm-shaders") == 0) {
        QCoreApplication app(argc, argv);
        app.setApplicationName(QStringLiteral(APP_NAME "Warmup"));
        return ShaderCache::warm(QCoreApplication::arguments().mid(2));
    }

    // Chromium logs each console line of a hidden page at INFO too; warnings and errors are what matter.
    if (const QByteArray flags = qgetenv("QTWEBENGINE_CHROMIUM_FLAGS"); !flags.contains("--log-level"))
        qputenv("QTWEBENGINE_CHROMIUM_FLAGS", (flags + " --log-level=1").trimmed());

    // Must precede QGuiApplication: without it the solver's page never commits a navigation.
    QtWebEngineQuick::initialize();

    // With an alpha channel Qt presents via DirectComposition, so mpv's swapchain can go under the content.
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
    QQuickWindow::setDefaultAlphaBuffer(true);
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral(APP_NAME));
    app.setApplicationVersion(QStringLiteral(APP_VERSION));
    app.setWindowIcon(QIcon(":/" MAIN_MODULE_URI "/resources/app.ico"));

    // arguments(), not argv: argv is in the ANSI code page, which mangles a Japanese file name.
    const QStringList arguments = QCoreApplication::arguments();
    const QString launchArgument = arguments.size() > 1 ? arguments.at(1) : QString();
    // In quiet mode each data folder is its own instance, so it never hands its file to another copy.
    SingleInstance instance(QCoreApplication::applicationDirPath()
                            + (Platform::quiet() ? QStringLiteral("|quiet|") + Settings::dataDir() : QString()));
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
        engine.addImportPath("qrc:/" MAIN_MODULE_URI "/src/ui");
        engine.setNetworkAccessManagerFactory(new ImageAccessManagerFactory);

        const QUrl url(QStringLiteral("qrc:/" MAIN_MODULE_URI "/src/ui/main.qml"));
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
            if (!window || Platform::quiet()) return;
            window->setWindowStates(window->windowStates() & ~Qt::WindowMinimized);
            window->show();
            window->raise();
            window->requestActivate();
        });
        exitCode = app.exec();
        Cloudflare::shutdown();
    }

    // Drain every pool, idle ones too: Windows ends their waiting threads at exit and the destructor warns.
    Cloudflare::pageFetchPool().waitForDone();
    ServerSelector::probePool().waitForDone();
    DnsOverHttps::waitForLookups();
    QThreadPool::globalInstance()->waitForDone();
    Log::flushLog();
    return exitCode;
}
