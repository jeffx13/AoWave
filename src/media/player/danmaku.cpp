#include "media/player/danmaku.h"
#include "app/logger.h"
#include <QCryptographicHash>
#include <numeric>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutex>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <algorithm>

namespace {

constexpr int    kResX        = 1920;
constexpr int    kResY        = 1080;
constexpr int    kTopMargin   = 8;
constexpr double kBaseFont    = 48.0;
constexpr double kStaticSecs  = 5.0;
constexpr double kScrollSecs  = 10.0;
constexpr int    kMaxComments = 10000;
constexpr int    kMaxTextLen  = 100;
constexpr int    kRepeatWindowMs = 5000;

struct Layout {
    QString     key;
    QByteArray  fingerprint;
    QStringList events;
    bool        laidOut = false;
};
QMutex g_layoutMutex;
Layout g_layout;

QByteArray layoutFingerprint(const DanmakuOptions &o, int rawCount) {
    return QByteArray::number(o.fontScalePct) + o.font.toUtf8() + "|"
         + QByteArray::number(o.speedPct)     + "|" + QByteArray::number(o.areaPct)
         + "|" + QByteArray::number(o.maxLines)   + "|" + QByteArray::number(o.minWeight)
         + "|" + QByteArray::number(o.maxOnScreen)
         + "|" + (o.blockScroll ? "1" : "0") + (o.blockTop ? "1" : "0")
         + (o.blockBottom ? "1" : "0") + (o.blockColour ? "1" : "0")
         + (o.blockRepeat ? "1" : "0")
         + "|" + QByteArray::number(rawCount);
}

bool isWide(char32_t cp) {
    return (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0x303E)
        || (cp >= 0x3041 && cp <= 0x33FF) || (cp >= 0x3400 && cp <= 0x4DBF)
        || (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xA000 && cp <= 0xA4CF)
        || (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF)
        || (cp >= 0xFE30 && cp <= 0xFE6F) || (cp >= 0xFF00 && cp <= 0xFF60)
        || (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x20000 && cp <= 0x2FFFD)
        || (cp >= 0x30000 && cp <= 0x3FFFD);
}

double textWidth(const QString &text, double fontPx) {
    double em = 0.0;
    for (char32_t cp : text.toUcs4())
        em += isWide(cp) ? 1.0 : 0.55;
    return fontPx * em;
}

QString sanitize(const QString &raw) {
    QString out;
    out.reserve(raw.size());
    for (QChar ch : raw) {
        const char16_t u = ch.unicode();
        if (u == u'\n' || u == u'\r' || u == u'\t') out += u' ';
        else if (u < 0x20)                          continue;
        else if (u == u'{')                         out += QChar(0xFF5B);
        else if (u == u'}')                         out += QChar(0xFF5D);
        else if (u == u'\\')                        out += QChar(0xFF3C);
        else                                        out += ch;
    }
    return out.simplified();
}

QString assTime(double seconds) {
    if (seconds < 0) seconds = 0;
    int cs = int(qRound(seconds * 100.0));
    const int h = cs / 360000; cs %= 360000;
    const int m = cs / 6000;   cs %= 6000;
    const int s = cs / 100;    cs %= 100;
    return QStringLiteral("%1:%2:%3.%4")
        .arg(h).arg(m, 2, 10, QLatin1Char('0'))
        .arg(s, 2, 10, QLatin1Char('0')).arg(cs, 2, 10, QLatin1Char('0'));
}

struct Lane {
    double lastT        = -1e9;
    double lastW        = 0.0;
    double blockedUntil = -1e9;
};

QString colourTag(quint32 rgb) {
    rgb &= 0xFFFFFFu;
    if (rgb == 0xFFFFFFu) return {};
    const quint32 r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    const QString hex = QStringLiteral("%1%2%3")
        .arg(b, 2, 16, QLatin1Char('0')).arg(g, 2, 16, QLatin1Char('0'))
        .arg(r, 2, 16, QLatin1Char('0')).toUpper();
    QString tag = QStringLiteral("\\c&H%1&").arg(hex);
    if (0.299 * r + 0.587 * g + 0.114 * b < 60.0)
        tag += QStringLiteral("\\3c&HFFFFFF&");
    return tag;
}

struct LaneGrid {
    double fontPx;
    int    height;
    int    count;
    double scrollSecs;
};

// VIP gradient: outline #F2509E to #308BCD (sampled); libass has no gradient, so each letter steps.
QString gradientText(const QString &text) {
    const QList<uint> letters = text.toUcs4();
    if (letters.size() < 2) return QStringLiteral("{\\3c&HCD8B30&}") + text;
    constexpr int from[3] = {0xF2, 0x50, 0x9E}, to[3] = {0x30, 0x8B, 0xCD};
    QString out;
    for (qsizetype i = 0; i < letters.size(); ++i) {
        const double t = double(i) / double(letters.size() - 1);
        int c[3];
        for (int k = 0; k < 3; ++k) c[k] = int(qRound(from[k] + (to[k] - from[k]) * t));
        const QString hex = QStringLiteral("%1%2%3").arg(c[2], 2, 16, QLatin1Char('0'))
                                .arg(c[1], 2, 16, QLatin1Char('0')).arg(c[0], 2, 16, QLatin1Char('0')).toUpper();
        out += QStringLiteral("{\\3c&H") + hex + QStringLiteral("&}");
        out += QString::fromUcs4(reinterpret_cast<const char32_t *>(&letters[i]), 1);
    }
    return out;
}

QList<DanmakuComment> capByWeight(QList<DanmakuComment> kept, int &dropped) {
    dropped = 0;
    if (kept.size() <= kMaxComments) return kept;
    std::stable_sort(kept.begin(), kept.end(),
                     [](const DanmakuComment &a, const DanmakuComment &b) { return a.timeMs < b.timeMs; });
    QMap<int, int> hist;
    for (const DanmakuComment &c : std::as_const(kept)) ++hist[c.weight];
    int above = 0, threshold = 0;
    for (auto it = hist.constEnd(); it != hist.constBegin(); ) {
        --it;
        if (above + it.value() >= kMaxComments) { threshold = it.key(); break; }
        above += it.value();
    }
    const int room = kMaxComments - above;
    const int tied = hist.value(threshold);
    QList<DanmakuComment> out;
    out.reserve(kMaxComments);
    int seen = 0, taken = 0;
    for (const DanmakuComment &c : std::as_const(kept)) {
        if (c.weight > threshold) { out.append(c); continue; }
        if (c.weight < threshold) continue;
        if (taken < room && qint64(seen) * room / tied >= taken) { out.append(c); ++taken; }
        ++seen;
    }
    dropped = int(kept.size() - out.size());
    return out;
}

// Mode 7: placed on the 2014 player's 672x438 stage, as danmaku2ass draws it. Its text is a JSON array:
// [x, y, "alpha from-to", seconds, text, rotateZ, rotateY, toX, toY, move ms, delay ms, outline, font]
QString positioned(const DanmakuComment &c, double t, double fontScale, double allOpacity, double &ends) {
    const QJsonArray a = QJsonDocument::fromJson(c.text.toUtf8()).array();
    if (a.size() < 5) return {};
    constexpr double kStageW = 672.0, kStageH = 438.0;
    const double zoom = qMin(kResX / kStageW, kResY / kStageH);
    const double offX = (kResX - kStageW * zoom) / 2.0, offY = (kResY - kStageH * zoom) / 2.0;
    const auto number = [&a](int i, double fallback) {
        const QJsonValue v = a.at(i);
        bool ok = v.isDouble();
        const double d = ok ? v.toDouble() : v.toString().toDouble(&ok);
        return ok && qIsFinite(d) ? qBound(-100000.0, d, 100000.0) : fallback;
    };
    const auto place = [&](int i, double fallback, bool vertical) {
        double d = number(i, fallback);
        const QJsonValue v = a.at(i);
        if ((d > 0 && d < 1) || (v.isString() && v.toString().contains(u'.') && d <= 1))
            d *= vertical ? kStageH : kStageW;
        return d * zoom + (vertical ? offY : offX);
    };

    QStringList lines;
    static const QRegularExpression lineBreak(QStringLiteral("/n|\n"));
    for (const QString &line : a.at(4).toVariant().toString().split(lineBreak)) lines << sanitize(line);
    if (lines.join(QString()).isEmpty()) return {};

    const double life = qBound(0.1, number(3, 4.5), 60.0);
    const double x0 = place(0, 0, false), y0 = place(1, 0, true);
    const double x1 = place(7, number(0, 0), false), y1 = place(8, number(1, 0), true);
    const int delay = int(number(10, 0)), moving = int(number(9, life * 1000));
    QString tags = qAbs(x0 - x1) < 0.5 && qAbs(y0 - y1) < 0.5
        ? QStringLiteral("\\pos(%1,%2)").arg(qRound(x0)).arg(qRound(y0))
        : QStringLiteral("\\move(%1,%2,%3,%4,%5,%6)").arg(qRound(x0)).arg(qRound(y0)).arg(qRound(x1))
              .arg(qRound(y1)).arg(delay).arg(delay + moving);
    if (const int z = int(number(5, 0))) tags += QStringLiteral("\\frz%1").arg(-z);
    if (const int y = int(number(6, 0))) tags += QStringLiteral("\\fry%1").arg(-y);
    if (const QString font = sanitize(a.at(12).toString()); !font.isEmpty()) tags += QStringLiteral("\\fn") + font;
    tags += QStringLiteral("\\fs%1").arg(qMax(1, int(qRound((c.fontSize > 0 ? c.fontSize : 25) * zoom * fontScale))));
    tags += colourTag(c.color);

    const QStringList alpha = a.at(2).toVariant().toString().split(u'-');
    const auto opacity = [](const QString &value, double fallback) {
        bool ok = false;
        const double d = value.toDouble(&ok);
        return ok ? qBound(0.0, d, 1.0) : fallback;
    };
    const double from = opacity(alpha.value(0), 1.0), to = opacity(alpha.value(1), from);
    const double all = qBound(0.0, allOpacity, 1.0);
    const int fromA = 255 - int(qRound(from * all * 255)), toA = 255 - int(qRound(to * all * 255));
    const int lifeMs = int(qRound(life * 1000));
    if (fromA != toA)
        tags += QStringLiteral("\\fade(%1,%2,%2,0,%3,%3,%3)").arg(fromA).arg(toA).arg(lifeMs);
    else if (fromA > 0)
        tags += QStringLiteral("\\alpha&H%1&").arg(QStringLiteral("%1").arg(fromA, 2, 16, QLatin1Char('0')).toUpper());
    if (a.at(11).toVariant().toString() == QLatin1String("false")) tags += QStringLiteral("\\bord0");

    ends = t + life;
    return QStringLiteral("Dialogue: 2,%1,%2,Danmaku,,0,0,0,,{%3}%4")
        .arg(assTime(t), assTime(ends), tags, lines.join(QStringLiteral("\\N")));
}

QStringList layOut(QList<DanmakuComment> comments, const DanmakuOptions &opt,
                   const QString &cacheKey, const LaneGrid &grid) {
    int dropMode = 0, dropText = 0, dropDup = 0, dropWeight = 0, dropDensity = 0,
        dropScreen = 0, dropLane = 0;
    const int rawCount = int(comments.size());

    QList<DanmakuComment> kept;
    kept.reserve(comments.size());
    for (DanmakuComment &c : comments) {
        if (c.mode < 1 || c.mode > 7 || c.pool == 2) { ++dropMode; continue; }
        const bool scrolling = c.mode <= 3 || c.mode == 6;
        if ((scrolling && opt.blockScroll) || (c.mode == 5 && opt.blockTop)
            || (c.mode == 4 && opt.blockBottom)) { ++dropMode; continue; }
        if (c.mode != 7) c.text = sanitize(c.text);
        if (c.text.isEmpty() || (c.mode != 7 && c.text.size() > kMaxTextLen)) { ++dropText; continue; }
        if (c.rated && c.weight < opt.minWeight) { ++dropWeight; continue; }
        if (opt.blockColour) { c.color = 0xFFFFFFu; c.colorful = false; }
        kept.append(c);
    }

    if (opt.blockRepeat) {
        QList<int> byTime(kept.size());
        std::iota(byTime.begin(), byTime.end(), 0);
        std::stable_sort(byTime.begin(), byTime.end(), [&kept](int a, int b) { return kept[a].timeMs < kept[b].timeMs; });
        QHash<QString, int> shownAt;
        QList<bool> repeat(kept.size(), false);
        for (const int i : byTime) {
            const auto shown = shownAt.constFind(kept[i].text);
            if (shown != shownAt.constEnd() && kept[i].timeMs - shown.value() < kRepeatWindowMs) {
                repeat[i] = true;
                ++dropDup;
            } else {
                shownAt.insert(kept[i].text, kept[i].timeMs);
            }
        }
        for (int i = int(kept.size()) - 1; i >= 0; --i)
            if (repeat[i]) kept.removeAt(i);
    }
    kept = capByWeight(std::move(kept), dropDensity);
    std::stable_sort(kept.begin(), kept.end(),
                     [](const DanmakuComment &a, const DanmakuComment &b) {
                         return a.timeMs < b.timeMs;
                     });

    QList<Lane> lanes(grid.count), reverseLanes(grid.count), topLanes(grid.count), bottomLanes(grid.count);
    QList<double> active;
    QStringList events;
    events.reserve(kept.size());

    for (const DanmakuComment &c : std::as_const(kept)) {
        const double t = c.timeMs / 1000.0;
        const double sizeRatio = qBound(0.5, (c.fontSize > 0 ? c.fontSize : 25) / 25.0, 2.0);
        const double glyphPx   = grid.fontPx * sizeRatio;
        const double w = textWidth(c.text, glyphPx);
        const bool scrolling = c.mode <= 3 || c.mode == 6;
        const int span = sizeRatio > 1.15 ? 2 : 1;
        QString tags = colourTag(c.color);
        if (qAbs(sizeRatio - 1.0) > 0.001)
            tags += QStringLiteral("\\fs%1").arg(qMax(1, int(qRound(glyphPx))));
        if (c.attr & 4) tags += QStringLiteral("\\b1");
        if (c.colorful) tags += QStringLiteral("\\1c&HFFFFFF&\\bord2");
        const QString body = c.colorful ? gradientText(c.text) : c.text;

        if (opt.maxOnScreen > 0) {
            active.removeIf([t](double end) { return end <= t; });
            if (active.size() >= opt.maxOnScreen) { ++dropScreen; continue; }
        }
        if (c.mode == 7) {
            double ends = t;
            const QString event = positioned(c, t, grid.fontPx / kBaseFont, opt.opacityPct / 100.0, ends);
            if (event.isEmpty()) { ++dropText; continue; }
            events += event;
            if (opt.maxOnScreen > 0) active.append(ends);
            continue;
        }

        int placed = -1;
        if (scrolling) {
            QList<Lane> &pool = c.mode == 6 ? reverseLanes : lanes;
            for (int i = 0; i + span <= grid.count; ++i) {
                bool free = true;
                for (int k = i; k < i + span && free; ++k) {
                    const Lane &ln = pool[k];
                    const double gap = grid.scrollSecs * qMax(ln.lastW > 0 ? ln.lastW / (kResX + ln.lastW) : 0.0,
                                                              w / (kResX + w));
                    free = t >= ln.lastT + gap;
                }
                if (free) { placed = i; break; }
            }
            if (placed < 0) { ++dropLane; continue; }
            for (int k = placed; k < placed + span; ++k) { pool[k].lastT = t; pool[k].lastW = w; }
            const int y = kTopMargin + placed * grid.height;
            const int fromX = c.mode == 6 ? -int(qRound(w)) : kResX;
            const int toX   = c.mode == 6 ? kResX : -int(qRound(w));
            events += QStringLiteral("Dialogue: 0,%1,%2,Danmaku,,0,0,0,,{%3\\move(%4,%5,%6,%5)}%7")
                .arg(assTime(t), assTime(t + grid.scrollSecs), tags)
                .arg(fromX).arg(y).arg(toX).arg(body);
        } else {
            const bool top = (c.mode == 5);
            QList<Lane> &pool = top ? topLanes : bottomLanes;
            for (int n = 0; n + span <= grid.count; ++n) {
                bool free = true;
                for (int k = n; k < n + span && free; ++k) free = t >= pool[k].blockedUntil;
                if (free) { placed = n; break; }
            }
            if (placed < 0) { ++dropLane; continue; }
            for (int k = placed; k < placed + span; ++k) pool[k].blockedUntil = t + kStaticSecs;
            const int y = top ? kTopMargin + placed * grid.height
                              : kTopMargin + (grid.count - placed) * grid.height;
            events += QStringLiteral("Dialogue: 1,%1,%2,Danmaku,,0,0,0,,{%3\\an%4\\pos(%5,%6)}%7")
                .arg(assTime(t), assTime(t + kStaticSecs), tags)
                .arg(top ? 8 : 2).arg(kResX / 2).arg(y).arg(body);
        }
        if (opt.maxOnScreen > 0) active.append(t + (scrolling ? grid.scrollSecs : kStaticSecs));
    }

    const QString dropped = QStringLiteral("drop{mode=%1,text=%2,dup=%3,weight=%4,density=%5,screen=%6,lane=%7}")
                                .arg(dropMode).arg(dropText).arg(dropDup).arg(dropWeight)
                                .arg(dropDensity).arg(dropScreen).arg(dropLane);
    if (!events.isEmpty())
        logOk() << "Danmaku" << cacheKey << "raw" << rawCount << "kept" << events.size() << dropped
               << "lanes" << grid.count << "font" << int(grid.fontPx);
    else
        logInfo() << "Danmaku" << cacheKey << "none of the" << rawCount << "comments is left to show:" << dropped
                  << (dropWeight > 0 ? QStringLiteral("(Hide spam is at %1)").arg(opt.minWeight) : QString());
    return events;
}

// Every line carries \move or \pos, so mpv calls these signs and skips sub-pos.
QString assDocument(const QStringList &events, const DanmakuOptions &opt, double fontPx) {
    const int alpha = qBound(0, 255 - int(qRound(opt.opacityPct / 100.0 * 255.0)), 255);
    const QString alphaHex = QStringLiteral("%1").arg(alpha, 2, 16, QLatin1Char('0')).toUpper();
    // Not arg(): a placeholder before a digit reads as two.
    const QString textColour    = QLatin1String("&H") + alphaHex + QLatin1String("FFFFFF");
    const QString outlineColour = QLatin1String("&H") + alphaHex + QLatin1String("000000");

    QString out;
    out.reserve(events.size() * 80 + 1024);
    out += QStringLiteral(
        "[Script Info]\n"
        "ScriptType: v4.00+\n"
        "PlayResX: %1\n"
        "PlayResY: %2\n"
        "WrapStyle: 2\n"
        "ScaledBorderAndShadow: yes\n"
        "YCbCr Matrix: None\n\n"
        "[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, "
        "BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, "
        "BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Danmaku,%3,%4,%5,%5,%6,%6,%7,0,0,0,100,100,0,0,1,%8,%9,7,0,0,0,1\n\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n")
        .arg(kResX).arg(kResY).arg(opt.font).arg(qMax(1, int(qRound(fontPx))))
        .arg(textColour).arg(outlineColour)
        .arg(opt.bold ? -1 : 0)
        .arg(opt.outline == 0 ? 0 : 1)
        .arg(opt.outline == 2 ? 1 : 0);
    out += events.join(QLatin1Char('\n'));
    out += QLatin1Char('\n');
    return out;
}

}

QString DanmakuAss::writeFile(QList<DanmakuComment> comments, const QString &cacheKey,
                              const DanmakuOptions &opt, const QString &outDir) {
    if (!opt.enabled || comments.isEmpty() || outDir.isEmpty()) return {};
    QDir().mkpath(outDir);

    LaneGrid grid;
    grid.fontPx = kBaseFont * opt.fontScalePct / 100.0;
    grid.height = qMax(1, int(qRound(grid.fontPx * 1.15)));
    grid.count  = int((kResY * opt.areaPct / 100.0 - kTopMargin) / grid.height);
    if (opt.maxLines > 0) grid.count = qMin(grid.count, opt.maxLines);
    grid.count = qBound(1, grid.count, 64);
    grid.scrollSecs = kScrollSecs * 100.0 / qMax(1, opt.speedPct);

    const QByteArray fingerprint = layoutFingerprint(opt, int(comments.size()));
    QStringList events;
    bool laidOut = false;
    {
        QMutexLocker lock(&g_layoutMutex);
        if (g_layout.key == cacheKey && g_layout.fingerprint == fingerprint) {
            events = g_layout.events;
            laidOut = g_layout.laidOut;
        }
    }
    if (!laidOut) {
        events = layOut(std::move(comments), opt, cacheKey, grid);
        QMutexLocker lock(&g_layoutMutex);
        g_layout = {cacheKey, fingerprint, events, true};
    }
    if (events.isEmpty()) return {};

    const QString path = QStringLiteral("%1/danmaku_%2.ass").arg(outDir, cacheKey);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        logWarn() << "Danmaku" << "Could not write" << path;
        return {};
    }
    file.write("\xEF\xBB\xBF");
    file.write(assDocument(events, opt, grid.fontPx).toUtf8());
    if (!file.commit()) {
        logWarn() << "Danmaku" << "Could not commit" << path;
        return {};
    }
    return path;
}

namespace {
QString decodeEntities(const QString &text) {
    static const QRegularExpression entity(QStringLiteral("&(#[xX][0-9a-fA-F]{1,6}|#[0-9]{1,7}|lt|gt|quot|apos|amp);"));
    QString out;
    qsizetype last = 0;
    for (auto it = entity.globalMatch(text); it.hasNext();) {
        const auto match = it.next();
        out += QStringView(text).mid(last, match.capturedStart() - last);
        last = match.capturedEnd();
        const QString name = match.captured(1);
        if (name.startsWith(QLatin1Char('#'))) {
            bool ok = false;
            const bool hex = name.size() > 1 && (name[1] == QLatin1Char('x') || name[1] == QLatin1Char('X'));
            const char32_t code = hex ? name.mid(2).toUInt(&ok, 16) : name.mid(1).toUInt(&ok);
            if (ok && code > 0 && code <= 0x10FFFF) out += QString::fromUcs4(&code, 1);
            else out += match.captured();
            continue;
        }
        out += name == QLatin1String("lt") ? QLatin1String("<") : name == QLatin1String("gt") ? QLatin1String(">")
             : name == QLatin1String("quot") ? QLatin1String("\"") : name == QLatin1String("apos") ? QLatin1String("'")
             : QLatin1String("&");
    }
    return out + QStringView(text).mid(last);
}
}

QByteArray DanmakuAss::toXml(const QList<DanmakuComment> &comments) {
    static const QRegularExpression notXml(QStringLiteral("[\\x00-\\x08\\x0B\\x0C\\x0E-\\x1F]"));
    QByteArray out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<i>\n";
    for (const DanmakuComment &c : comments) {
        out += QStringLiteral("<d p=\"%1,%2,%3,%4,0,%5,0,0,%6\"")
                   .arg(c.timeMs / 1000.0, 0, 'f', 3).arg(c.mode).arg(c.fontSize).arg(c.color)
                   .arg(c.pool).arg(c.rated ? c.weight : 0).toUtf8();
        if (c.attr) out += " attr=\"" + QByteArray::number(c.attr) + '"';
        if (c.colorful) out += " colorful=\"1\"";
        out += '>' + QString(c.text).remove(notXml).toHtmlEscaped().toUtf8() + "</d>\n";
    }
    out += "</i>\n";
    return out;
}

QList<DanmakuComment> DanmakuAss::fromXml(const QByteArray &xml) {
    QList<DanmakuComment> out;
    if (!xml.contains("<d ")) return out;
    static const QRegularExpression entry(
        QStringLiteral(R"RX(<d([^>]*)\bp="([^"]*)"([^>]*)>(.*?)</d>)RX"),
        QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression attrRe(QStringLiteral(R"RX(\battr="(\d+)")RX"));
    static const QRegularExpression colorfulRe(QStringLiteral(R"RX(\bcolorful="1")RX"));
    auto it = entry.globalMatch(QString::fromUtf8(xml));
    while (it.hasNext()) {
        const auto match = it.next();
        const auto fields = match.captured(2).split(QLatin1Char(','));
        if (fields.size() < 5) continue;
        const QString extras = match.captured(1) + match.captured(3);
        DanmakuComment comment;
        comment.timeMs   = int(fields.at(0).toDouble() * 1000.0);
        comment.mode     = fields.at(1).toInt();
        comment.fontSize = fields.at(2).toInt();
        comment.color    = quint32(fields.at(3).toUInt()) & 0xFFFFFFu;
        comment.pool     = fields.size() > 5 ? fields.at(5).toInt() : 0;
        comment.weight   = fields.size() > 8 ? fields.at(8).toInt() : 0;
        comment.attr     = attrRe.match(extras).captured(1).toInt();
        comment.colorful = colorfulRe.match(extras).hasMatch();
        comment.text     = decodeEntities(match.captured(4));
        if (comment.fontSize <= 0) comment.fontSize = 25;
        comment.rated = comment.weight > 0;
        if (comment.weight   <= 0) comment.weight   = 10;
        if (!comment.text.isEmpty()) out.append(std::move(comment));
    }
    return out;
}

QList<DanmakuComment> DanmakuAss::fromVideoFile(const QString &path, const QString &ffmpeg, const QString &cacheDir) {
    const QFileInfo info(path);
    if (!info.isFile() || ffmpeg.isEmpty()) return {};
    const QByteArray version = info.absoluteFilePath().toUtf8() + '|' + QByteArray::number(info.size())
                               + '|' + QByteArray::number(info.lastModified().toMSecsSinceEpoch());
    QDir().mkpath(cacheDir);
    const QString cached = cacheDir + QLatin1Char('/')
                           + QString::fromLatin1(QCryptographicHash::hash(version, QCryptographicHash::Md5).toHex())
                           + QStringLiteral(".xml");
    if (!QFile::exists(cached)) {
        // ffmpeg complains that there is no output and dumps the attachment anyway.
        QProcess dump;
        dump.start(ffmpeg, {QStringLiteral("-hide_banner"), QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-y"),
                            QStringLiteral("-dump_attachment:m:filename:danmaku.xml"), QDir::toNativeSeparators(cached),
                            QStringLiteral("-i"), QDir::toNativeSeparators(info.absoluteFilePath())});
        if (!dump.waitForFinished(20000) || dump.exitStatus() != QProcess::NormalExit) {
            dump.kill();
            dump.waitForFinished(1000);
            QFile::remove(cached);
            return {};
        }
        if (!QFile::exists(cached)) {
            QFile none(cached);
            if (none.open(QIODevice::WriteOnly)) none.close();
        }
    }
    QFile file(cached);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QList<DanmakuComment> comments = fromXml(file.readAll());
    if (!comments.isEmpty())
        logInfo() << "Danmaku" << comments.size() << "comments kept in" << info.fileName();
    return comments;
}

void DanmakuAss::pruneCache(const QString &cacheDir) {
    constexpr int kMaxAgeDays = 7, kMaxFiles = 100;
    QDir dir(cacheDir);
    if (!dir.exists()) return;
    const auto files = dir.entryInfoList(QDir::Files, QDir::Time);
    const QDateTime cutoff = QDateTime::currentDateTime().addDays(-kMaxAgeDays);
    int kept = 0;
    for (const QFileInfo &fi : files) {
        if (++kept > kMaxFiles || fi.lastModified() < cutoff)
            QFile::remove(fi.absoluteFilePath());
    }
}

namespace {

QMutex         g_optionsMutex;
DanmakuOptions g_options;

}

DanmakuOptions DanmakuOptions::current() {
    QMutexLocker lock(&g_optionsMutex);
    return g_options;
}

void DanmakuOptions::set(const DanmakuOptions &options) {
    QMutexLocker lock(&g_optionsMutex);
    g_options = options;
}
