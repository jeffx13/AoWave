#pragma once
#include <QMap>
#include <QString>
#include <QStringList>
#include "net/canceltoken.h"

// yt-dlp beside the app, which mpv also runs for page links. The blocking calls run processes:
// keep them off the GUI thread.
namespace YtDlp {

// Empty when it is missing.
QString executable();

// What `--version` prints ("2026.08.19"), asked once and kept; empty when it will not run.
// Blocks the first time.
QString version();
void forgetVersion();   // after an update

// YouTube needs a JavaScript runtime to solve its challenges. yt-dlp looks only for Deno by
// itself; `--js-runtimes` (from 2025.11.12, which an older yt-dlp rejects) points it at another.
bool takesJsRuntimes(const QString &version);

// Deno 2.3+, which yt-dlp prefers, else Node 22+: on PATH or where installers and version managers
// put them. "deno:<path>" or "node:<path>", or empty. Blocks.
QString findJsRuntime();

// The runtime's own version ("24.15.0"), empty when it does not run or is too old for yt-dlp. Blocks.
QString runtimeVersion(const QString &runtime);

// The arguments that hand yt-dlp `runtime`, given its `version`; none it could not take.
QStringList runtimeArguments(const QString &runtime, const QString &version);

// Whether yt-dlp finds a video at `url`, the extraction mpv would do. `error` is yt-dlp's reason.
struct Verdict {
    bool playable = false;
    QString error;
};
Verdict check(const QString &url, const QMap<QString, QString> &headers, const QStringList &arguments,
              const CancelToken &cancel);

}
