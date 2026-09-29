#pragma once
#include <QList>
#include <QString>
#include <QStringList>

// A show's remembered tracks, speed and danmaku choice, stored under tracks/<show link>.
// Append-only, and every reader has a default, so an older string still loads.
struct TrackPrefs {
    QString subtitleTitle;             // empty for a derived label or a fetched external subtitle
    QString audioTitle;
    bool    subtitlesVisible = false;
    int     videoHeight = -1;
    int     videoWithinHeight = 0;
    int     audioIndex = -1;
    QString secondarySubtitleTitle;
    int     subtitleIndex = -1;
    double  speed = 0.0;               // 0 = never had one, so the global speed stands
    int     danmaku = -1;              // -1 never recorded, 0 off, 1 on
    bool    hasSubtitles = false;

    static TrackPrefs parse(const QString &stored);
    QString toString() const;

    // The same height and rank among equal heights, else the nearest height; -1 for none.
    static int pickVideo(const QList<int> &heights, int savedHeight, int savedWithin);
    // Exact title match first, then the saved rank; -1 for none.
    static int pickAudio(const QStringList &titles, const QString &savedTitle, int savedRank);
};
