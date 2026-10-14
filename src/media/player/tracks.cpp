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
    prefs.subDelay               = text(9).toDouble();
    prefs.audioDelay             = text(10).toDouble();
    return prefs;
}

QString TrackPrefs::toString() const {
    QStringList fields{subtitleTitle,
                       audioTitle,
                       subtitlesVisible ? QStringLiteral("1") : QStringLiteral("0"),
                       QString::number(videoHeight),
                       QString::number(videoWithinHeight),
                       QString::number(audioIndex),
                       secondarySubtitleTitle,
                       QString::number(subtitleIndex)};
    if (subDelay != 0 || audioDelay != 0)
        fields << QString() << QString::number(subDelay) << QString::number(audioDelay);
    return fields.join(QChar(0x1f));
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

namespace {
struct Language { const char *code, *codes, *name; };
constexpr Language kLanguages[] = {
    {"en", "en,eng", "English"},
    {"ja", "ja,jpn", "Japanese"},
    {"zh-Hans", "zh-Hans,zh-CN,zh-SG,chs,zh,chi,zho", "Chinese"},
    {"zh-Hant", "zh-Hant,zh-TW,zh-HK,cht,zh,chi,zho", "Chinese"},
    {"ko", "ko,kor", "Korean"},
    {"es", "es,spa", "Spanish"},
    {"pt", "pt,por", "Portuguese"},
    {"fr", "fr,fre,fra", "French"},
    {"de", "de,ger,deu", "German"},
    {"it", "it,ita", "Italian"},
    {"ru", "ru,rus", "Russian"},
    {"ar", "ar,ara", "Arabic"},
    {"id", "id,ind", "Indonesian"},
    {"th", "th,tha", "Thai"},
    {"vi", "vi,vie", "Vietnamese"},
};
}

QString TrackLanguage::codes(const QString &language) {
    for (const Language &entry : kLanguages)
        if (language == QLatin1String(entry.code)) return QLatin1String(entry.codes);
    return {};
}

namespace {
int chineseScript(const QString &code, const QString &title = QString()) {
    static const QStringList simplified{"zh-hans", "zh-cn", "zh-sg", "chs"}, traditional{"zh-hant", "zh-tw", "zh-hk", "zh-mo", "cht"};
    const QString lower = code.trimmed().toLower();
    const auto in = [&lower](const QStringList &list) {
        return std::any_of(list.cbegin(), list.cend(), [&lower](const QString &c) {
            return lower == c || lower.startsWith(c + QLatin1Char('-'));
        });
    };
    if (in(simplified)) return 1;
    if (in(traditional)) return 2;
    if (title.contains(QLatin1String("Simplified"), Qt::CaseInsensitive) || title.contains(QChar(0x7B80))) return 1;
    if (title.contains(QLatin1String("Traditional"), Qt::CaseInsensitive) || title.contains(QChar(0x7E41))) return 2;
    return 0;
}
}

bool TrackLanguage::matches(const QString &trackLang, const QString &trackTitle, const QStringList &codes) {
    // "zh" alone would take both scripts
    if (const int script = chineseScript(trackLang, trackTitle)) {
        for (const QString &code : codes)
            if (const int wanted = chineseScript(code); wanted != 0 && wanted != script) return false;
    }
    const QString lang = trackLang.trimmed().toLower();
    for (const QString &code : codes) {
        const QString wanted = code.trimmed().toLower();
        if (wanted.isEmpty()) continue;
        if (lang == wanted || lang.startsWith(wanted + QLatin1Char('-')) || lang.startsWith(wanted + QLatin1Char('_')))
            return true;
    }
    if (!lang.isEmpty() || trackTitle.isEmpty()) return false;
    for (const Language &entry : kLanguages) {
        const QStringList entryCodes = QString::fromLatin1(entry.codes).toLower().split(QLatin1Char(','));
        const bool wanted = std::any_of(codes.cbegin(), codes.cend(), [&](const QString &code) {
            return entryCodes.contains(code.trimmed().toLower());
        });
        if (wanted && trackTitle.contains(QLatin1String(entry.name), Qt::CaseInsensitive)) return true;
    }
    return false;
}

int TrackPrefs::pickAudio(const QStringList &titles, const QString &savedTitle, int savedRank) {
    if (!savedTitle.isEmpty())
        if (const int exact = int(titles.indexOf(savedTitle)); exact >= 0) return exact;
    if (savedRank >= 0 && savedRank < titles.size()) return savedRank;
    return -1;
}
