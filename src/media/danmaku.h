#pragma once
#include <QList>
#include <QString>
#include "core/danmakuoptions.h"
#include "shows/playinfo.h"

namespace DanmakuAss {

// Every line carries \\move or \\pos, so mpv treats them as signs.
QString writeFile(QList<DanmakuComment> comments, const QString &cacheKey,
                  const DanmakuOptions &options, const QString &outDir);

void pruneCache(const QString &cacheDir);

}
