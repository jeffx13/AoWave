#pragma once
#include <QString>

// Snapshot for workers, which must not touch QSettings.
struct DanmakuOptions {
    bool    enabled      = true;
    int     opacityPct   = 80;
    int     fontScalePct = 100;
    int     speedPct     = 100;
    int     areaPct      = 85;
    int     maxLines     = 0;    // 0 = derive from areaPct
    int     minWeight    = 0;    // 0 = off
    int     maxOnScreen  = 60;   // 0 = unlimited

    QString font         = QStringLiteral("Microsoft YaHei");
    bool    bold         = false;
    int     outline      = 1;    // 0 none, 1 outline, 2 outline + shadow

    bool blockScroll = false;
    bool blockTop    = false;
    bool blockBottom = false;
    bool blockColour = false;    // white instead of dropped
    bool blockRepeat = true;

    static DanmakuOptions current();
    static void set(const DanmakuOptions &options);
};
