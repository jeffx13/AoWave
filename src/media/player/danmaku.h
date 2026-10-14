#pragma once
#include "providers/showdata.h"
#include <QList>
#include <QString>

// Snapshot for workers, which must not touch QSettings.
struct DanmakuOptions {
    bool    enabled      = true;
    int     opacityPct   = 80;
    int     fontScalePct = 100;
    int     speedPct     = 100;
    int     areaPct      = 85;
    int     maxLines     = 0;
    int     minWeight    = 0;
    int     maxOnScreen  = 60;

    QString font         = QStringLiteral("Microsoft YaHei");
    bool    bold         = false;
    int     outline      = 1;

    bool blockScroll = false;
    bool blockTop    = false;
    bool blockBottom = false;
    bool blockColour = false;
    bool blockRepeat = true;

    static DanmakuOptions current();
    static void set(const DanmakuOptions &options);
};

namespace DanmakuAss {

QString writeFile(QList<DanmakuComment> comments, const QString &cacheKey,
                  const DanmakuOptions &options, const QString &outDir);

void pruneCache(const QString &cacheDir);

QByteArray toXml(const QList<DanmakuComment> &comments);
QList<DanmakuComment> fromXml(const QByteArray &xml);

QList<DanmakuComment> fromVideoFile(const QString &path, const QString &ffmpeg, const QString &cacheDir);

}
