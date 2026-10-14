#include "media/player/tracks.h"
#include <limits>

TrackPrefs TrackPrefs::parse(const QString &stored) {
    const QStringList fields = stored.split(QChar(0x1f));
    auto text = [&](int i) { return fields.value(i); };
    auto number = [&](int i, int fallback) {
        bool ok = false;
        const int value = text(i).toInt(&ok);
        return ok ? value : fallback;
    };
    TrackPrefs prefs;
    prefs.subtitleTitle          = text(0);
    prefs.audioTitle             = text(1);
    prefs.subtitlesVisible       = text(2) == QLatin1String("1");
    prefs.videoHeight            = number(3, -1);
    prefs.videoWithinHeight      = number(4, 0);
    prefs.audioIndex             = number(5, -1);
    prefs.secondarySubtitleTitle = text(6);
    prefs.subtitleIndex          = number(7, -1);
    return prefs;
}

QString TrackPrefs::toString() const {
    return QStringList{subtitleTitle,
                       audioTitle,
                       subtitlesVisible ? QStringLiteral("1") : QStringLiteral("0"),
                       QString::number(videoHeight),
                       QString::number(videoWithinHeight),
                       QString::number(audioIndex),
                       secondarySubtitleTitle,
                       QString::number(subtitleIndex)}
        .join(QChar(0x1f));
}

int TrackPrefs::pickVideo(const QList<int> &heights, int savedHeight, int savedWithin) {
    if (heights.isEmpty() || savedHeight < 0) return -1;
    int start = -1, count = 0;
    for (int i = 0; i < heights.size(); ++i)
        if (heights[i] == savedHeight) { if (start < 0) start = i; ++count; }
    if (start >= 0) return start + qMin(savedWithin, count - 1);

    int best = 0, bestDiff = std::numeric_limits<int>::max();
    for (int i = 0; i < heights.size(); ++i) {
        const int diff = qAbs(heights[i] - savedHeight);
        if (diff < bestDiff) { bestDiff = diff; best = i; }
    }
    return best;
}

int TrackPrefs::pickAudio(const QStringList &titles, const QString &savedTitle, int savedRank) {
    if (!savedTitle.isEmpty())
        if (const int exact = int(titles.indexOf(savedTitle)); exact >= 0) return exact;
    if (savedRank >= 0 && savedRank < titles.size()) return savedRank;
    return -1;
}
