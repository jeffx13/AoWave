#pragma once
#include <QMap>
#include <QString>
#include <QStringList>
#include "app/async.h"

namespace YtDlp {

QString executable();

QString version();
void forgetVersion();

// YouTube needs a JS runtime; yt-dlp finds only Deno by itself, and `--js-runtimes` names another.

QString findJsRuntime();

QString runtimeVersion(const QString &runtime);

QStringList runtimeArguments(const QString &runtime);

struct Verdict {
    bool playable = false;
    QString error;
    QUrl providerUrl;
};
Verdict check(const QString &url, const QMap<QString, QString> &headers, const QStringList &arguments,
              const CancelToken &cancel);

}
