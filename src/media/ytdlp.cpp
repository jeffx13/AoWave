#include "media/ytdlp.h"
#include "core/settings.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMutex>
#include <QObject>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QVersionNumber>

namespace {

QMutex g_versionMutex;
QString g_version;
bool g_versionKnown = false;

// A program's output, or empty when it does not finish in time.
QString run(const QString &program, const QStringList &arguments, int timeoutMs = 15000) {
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(program, arguments);
    if (!process.waitForStarted(5000) || !process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(2000);
        return {};
    }
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0
               ? QString::fromUtf8(process.readAll()).trimmed() : QString();
}

// yt-dlp's own floors, from its release notes (2026.06.09).
bool recentEnough(const QString &name, const QVersionNumber &version) {
    if (name == QLatin1String("deno")) return version >= QVersionNumber(2, 3);
    if (name == QLatin1String("node")) return version.majorVersion() >= 22;
    return false;
}

QString env(const char *name) { return qEnvironmentVariable(name); }

// Where a runtime lives when PATH does not say: installers, winget, scoop and version managers.
QStringList candidates(const QString &name) {
    const QString exe = name + QStringLiteral(".exe");
    const QString home = QDir::homePath();
    QStringList found;
    if (const QString onPath = QStandardPaths::findExecutable(name); !onPath.isEmpty()) found << onPath;
    QStringList dirs;
    if (name == QLatin1String("deno")) {
        dirs << home + "/.deno/bin" << env("LOCALAPPDATA") + "/Microsoft/WinGet/Links"
             << home + "/scoop/apps/deno/current";
    } else {
        dirs << env("NVM_SYMLINK") << QStringLiteral("C:/nvm4w/nodejs") << env("ProgramFiles") + "/nodejs"
             << env("LOCALAPPDATA") + "/Programs/nodejs" << home + "/scoop/apps/nodejs/current"
             << home + "/scoop/apps/nodejs-lts/current" << env("LOCALAPPDATA") + "/Volta/bin";
        // nvm-windows keeps every installed version: the newest is worth a try too.
        const QString nvmHome = !env("NVM_HOME").isEmpty() ? env("NVM_HOME") : env("APPDATA") + "/nvm";
        QStringList versions = QDir(nvmHome).entryList({QStringLiteral("v*")}, QDir::Dirs);
        std::sort(versions.begin(), versions.end(), [](const QString &a, const QString &b) {
            return QVersionNumber::fromString(a.mid(1)) > QVersionNumber::fromString(b.mid(1));
        });
        for (const QString &version : std::as_const(versions)) dirs << nvmHome + '/' + version;
    }
    for (const QString &dir : std::as_const(dirs))
        if (!dir.isEmpty() && QFileInfo::exists(dir + '/' + exe)) found << QDir::cleanPath(dir + '/' + exe);
    found.removeDuplicates();
    return found;
}

}

namespace YtDlp {

QString executable() {
    const QString beside = Settings::toolPath(QStringLiteral("yt-dlp.exe"));
    return QFileInfo::exists(beside) ? beside : QString();
}

QString version() {
    QMutexLocker lock(&g_versionMutex);
    if (!g_versionKnown) {
        const QString exe = executable();
        g_version = exe.isEmpty() ? QString() : run(exe, {QStringLiteral("--version")}).section('\n', 0, 0);
        g_versionKnown = true;
    }
    return g_version;
}

void forgetVersion() {
    QMutexLocker lock(&g_versionMutex);
    g_versionKnown = false;
}

bool takesJsRuntimes(const QString &version) {
    // Dated versions compare as text; a nightly only adds a time after the date.
    return version.left(10) >= QLatin1String("2025.11.12");
}

QString runtimeVersion(const QString &runtime) {
    const QString name = runtime.section(':', 0, 0);
    const QString path = runtime.section(':', 1);
    if (path.isEmpty()) return {};
    // "v24.15.0" from node; "deno 2.3.1 (stable, ...)" and more from deno.
    static const QRegularExpression number(QStringLiteral("(\\d+\\.\\d+\\.\\d+)"));
    const QString text = number.match(run(path, {QStringLiteral("--version")}, 5000)).captured(1);
    return recentEnough(name, QVersionNumber::fromString(text)) ? text : QString();
}

QString findJsRuntime() {
    for (const QString &name : {QStringLiteral("deno"), QStringLiteral("node")})
        for (const QString &path : candidates(name))
            if (const QString runtime = name + ':' + QDir::toNativeSeparators(path); !runtimeVersion(runtime).isEmpty())
                return runtime;
    return {};
}

QStringList runtimeArguments(const QString &runtime, const QString &version) {
    if (runtime.isEmpty() || !takesJsRuntimes(version)) return {};
    return {QStringLiteral("--js-runtimes"), runtime};
}

Verdict check(const QString &url, const QMap<QString, QString> &headers, const QStringList &arguments,
              const CancelToken &cancel) {
    const QString exe = executable();
    if (exe.isEmpty()) return {false, QObject::tr("yt-dlp.exe isn't next to the app.")};

    QStringList args{QStringLiteral("--no-warnings"), QStringLiteral("--no-playlist"), QStringLiteral("--get-url")};
    for (auto it = headers.constBegin(); it != headers.constEnd(); ++it)
        args << QStringLiteral("--add-headers") << it.key() + ':' + it.value();
    args << arguments << QStringLiteral("--") << url;   // so no link reads as an option

    QProcess process;
    process.start(exe, args);
    if (!process.waitForStarted(10000))
        return {false, QObject::tr("yt-dlp did not start.")};
    QElapsedTimer clock;
    clock.start();
    while (!process.waitForFinished(200)) {
        if (!cancel.isCancelled() && clock.elapsed() < 60000) continue;
        process.kill();
        process.waitForFinished(2000);
        return {false, cancel.isCancelled() ? QString()
                                            : QObject::tr("yt-dlp took over a minute.")};
    }
    const QString out = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    if (process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0 && !out.isEmpty()) return {true, {}};

    // Its last "ERROR: [youtube] id: why", as just the why.
    QString error;
    const QStringList lines = QString::fromUtf8(process.readAllStandardError()).split('\n');
    for (const QString &line : lines)
        if (line.startsWith(QLatin1String("ERROR:"))) error = line.mid(6).trimmed();
    static const QRegularExpression where(QStringLiteral("^\\[[^\\]]+\\]\\s*[^:\\s]+:\\s*"));
    error.remove(where);
    return {false, error.isEmpty() ? QObject::tr("yt-dlp found no video there.") : error};
}

}
