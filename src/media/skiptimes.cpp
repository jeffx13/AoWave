#include "media/skiptimes.h"
#include "media/mpvplayer.h"
#include "shows/playlistitem.h"
#include "core/async.h"
#include "core/settings.h"
#include "core/logger.h"
#include "core/exception.h"
#include "net/client.h"
#include <QtConcurrent/QtConcurrentRun>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QMetaObject>
#include <QRegularExpression>
#include <QThread>
#include <cmath>

namespace {
// Strip provider qualifiers ("(Dub)", "[1080p]", trailing "Subbed") that hurt the match.
QString cleanSearchTitle(QString t) {
    t.remove(QRegularExpression(QStringLiteral("\\((?:dub|sub|subbed|dubbed)\\)"),
                                QRegularExpression::CaseInsensitiveOption));
    t.remove(QRegularExpression(QStringLiteral("\\[[^\\]]*\\]")));
    t.remove(QRegularExpression(QStringLiteral("\\b(?:subbed|dubbed)\\b"),
                                QRegularExpression::CaseInsensitiveOption));
    return t.simplified();
}
}

SkipTimes::SkipTimes(QObject *parent) : QObject(parent) {}

// The workers capture `this`.
SkipTimes::~SkipTimes() {
    m_searchCancel.cancel();
    m_skipCancel.cancel();
    m_runs.waitAll("SkipTimes query");
}

bool SkipTimes::aniskipEnabled() const {
    return Settings::instance().get(Config::AniSkip);
}

int SkipTimes::currentMalId() const {
    return (m_selectedShowIndex >= 0 && m_selectedShowIndex < m_candidates.size())
        ? m_candidates[m_selectedShowIndex].malId : 0;
}

void SkipTimes::onCurrentItemChanged(PlaylistItem *item) {
    ensureConnections();

    QString newTitle, newLink;
    int newEpisode = -1, newPlaylistEps = 0;
    bool newOnline = false;
    if (item && !item->isList()) {
        auto parent = item->parent();
        newTitle   = parent ? parent->name : QString();
        newLink    = parent ? parent->link : QString();
        newPlaylistEps = parent ? parent->episodeCount() : 0;
        newEpisode = item->number > 0 ? static_cast<int>(std::lround(item->number)) : -1;
        newOnline  = (item->type & PlaylistItem::Online) != 0;
    }

    if (newLink == m_showLink && newEpisode == m_episode && newOnline == m_isOnline)
        return;

    m_skipCancel.cancel();

    // Keep the query and match, so a manual correction survives.
    const bool sameShow = newOnline && !newLink.isEmpty()
                          && newLink == m_showLink && !m_candidates.isEmpty();
    if (sameShow) {
        m_episode          = newEpisode;
        m_playlistEpisodes = newPlaylistEps;
        m_duration         = MpvPlayer::instance() ? MpvPlayer::instance()->duration() : 0;
        m_selectedEpisodeIndex  = newEpisode > 0 ? newEpisode : 1;
        rebuildEpisodeCount();
        emit selectedEpisodeIndexChanged();
        loadProfile(m_showLink);
        queryAniSkip();
        return;
    }

    m_searchCancel.cancel();

    m_showTitle = newTitle;
    m_showLink  = newLink;
    m_episode   = newEpisode;
    m_playlistEpisodes = newPlaylistEps;
    m_isOnline  = newOnline;
    m_duration  = 0;

    m_selectedEpisodeIndex = newEpisode > 0 ? newEpisode : 1;
    rebuildEpisodeCount();
    emit selectedEpisodeIndexChanged();

    if (m_isOnline && !m_showLink.isEmpty() && !m_malIdCache.contains(m_showLink)) {
        int saved = Settings::instance().value(Config::skipMal(m_showLink)).toInt();
        if (saved > 0) m_malIdCache.insert(m_showLink, saved);
    }

    if (m_isOnline && !m_showLink.isEmpty()) {
        loadProfile(m_showLink);
    } else {
        if (auto *mpv = MpvPlayer::instance()) {
            m_applying = true;
            mpv->setSkipOP(false);
            mpv->setSkipED(false);
            m_applying = false;
        }
        applyReset();
    }

    const QString q = cleanSearchTitle(newTitle);
    if (q != m_searchQuery) { m_searchQuery = q; emit searchQueryChanged(); }

    clearCandidates();

    if (m_isOnline && !m_showTitle.isEmpty() && aniskipEnabled())
        runSearch();
    else
        setStatus(QString(), false);
    onDurationChanged();
}

void SkipTimes::ensureConnections() {
    if (m_connected) return;
    auto *mpv = MpvPlayer::instance();
    if (!mpv) return;

    connect(mpv, &MpvPlayer::durationChanged, this, &SkipTimes::onDurationChanged);

    auto save = [this]() {
        if (m_applying) return;
        // Don't leak these times into the global fallback.
        if (m_isOnline && !m_showLink.isEmpty()) saveProfile();
        else                                     saveFallback();
    };
    connect(mpv, &MpvPlayer::skipOPChanged,       this, save);
    connect(mpv, &MpvPlayer::skipEDChanged,       this, save);
    connect(mpv, &MpvPlayer::skipOPStartChanged,  this, save);
    connect(mpv, &MpvPlayer::skipOPLengthChanged, this, save);
    connect(mpv, &MpvPlayer::skipEDLengthChanged, this, save);

    connect(&Settings::instance(), &Settings::aniskipEnabledChanged,
            this, &SkipTimes::onAniskipToggled);
    m_connected = true;
}

void SkipTimes::onDurationChanged() {
    auto *mpv = MpvPlayer::instance();
    if (!mpv) return;
    int duration = static_cast<int>(mpv->duration());
    if (duration <= 0 || duration == m_duration) return;
    m_duration = duration;
    // Duration is the last piece AniSkip needs.
    if (m_isOnline && currentMalId() > 0 && aniskipEnabled())
        queryAniSkip();
}

void SkipTimes::onAniskipToggled() {
    if (aniskipEnabled()) {
        if (m_isOnline && !m_showTitle.isEmpty()) runSearch();
    } else {
        m_searchCancel.cancel();
        m_skipCancel.cancel();
        applyReset();
        clearCandidates();
        setStatus(QString(), false);
    }
}

QList<SkipTimes::Candidate> SkipTimes::searchCandidates(Client &client, const QString &title) {
    QList<Candidate> out;
    if (title.isEmpty()) return out;
    QJsonObject vars{ {"s", title} };
    QJsonObject body{
        {"query", "query($s:String){Page(perPage:12){media(search:$s,type:ANIME,sort:SEARCH_MATCH){idMal title{romaji english native} episodes}}}"},
        {"variables", vars}
    };
    QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    auto resp = client.post("https://graphql.anilist.co", payload,
                            {{"Content-Type", "application/json"}, {"Accept", "application/json"}});
    if (client.isCancelled()) return {};
    const QJsonObject json = resp.toJsonObject();
    if (resp.code != 200 || json.isEmpty() || !json.value("errors").toArray().isEmpty()) {
        const QString message = json.value("errors").toArray().isEmpty() ? resp.error
            : json.value("errors").toArray().first().toObject().value("message").toString();
        throw AppException(tr("AniList lookup failed (HTTP %1): %2").arg(resp.code).arg(message), "AniList");
    }
    const auto media = json.value("data").toObject()
                           .value("Page").toObject().value("media").toArray();
    for (const auto &v : media) {
        const auto m = v.toObject();
        const int mal = m.value("idMal").toInt(0);
        if (mal <= 0) continue;
        const auto t = m.value("title").toObject();
        QString name = t.value("romaji").toString();
        if (name.isEmpty()) name = t.value("english").toString();
        if (name.isEmpty()) name = t.value("native").toString();
        if (name.isEmpty()) name = QStringLiteral("MAL %1").arg(mal);
        out.append({ mal, name, m.value("episodes").toInt(0) });
    }
    return out;
}

void SkipTimes::runSearch() {
    if (m_searchQuery.isEmpty()) { setStatus(tr("No title to search"), false); return; }
    setStatus(tr("Searching AniList..."), true);
    // A fresh token, not reset(): the flag is shared with a worker still in flight.
    m_searchCancel.cancel();
    m_searchCancel = CancelToken{};

    const QString query = m_searchQuery, showLink = m_showLink;
    const int preferMal = m_malIdCache.value(showLink, 0);

    m_runs.add(QtConcurrent::run([this, query, preferMal, cancel = m_searchCancel]() {
        Client client = Client(cancel, false).withSession(QStringLiteral("anilist"));
        try {
            auto list = searchCandidates(client, cleanSearchTitle(query));
            if (cancel.isCancelled()) return;
            QMetaObject::invokeMethod(this, [this, list, preferMal, cancel]() {
                if (!cancel.isCancelled()) setCandidates(list, preferMal);
            }, Qt::QueuedConnection);
        } catch (const std::exception &error) {
            if (cancel.isCancelled()) return;
            logWarn() << "AniList" << error.what();
            QMetaObject::invokeMethod(this, [this, cancel]() {
                if (cancel.isCancelled()) return;
                clearCandidates();
                applyReset();
                setStatus(tr("AniList unavailable. Try again."), false);
            }, Qt::QueuedConnection);
        }
    }));
}

void SkipTimes::rematch() {
    if (m_isOnline) runSearch();
}

void SkipTimes::setCandidates(const QList<Candidate> &list, int preferMalId) {
    m_candidates = list;
    m_showTitles.clear();
    for (const auto &c : list) m_showTitles << c.title;

    int sel = list.isEmpty() ? -1 : 0;
    if (preferMalId > 0)
        for (int i = 0; i < list.size(); ++i)
            if (list[i].malId == preferMalId) { sel = i; break; }
    m_selectedShowIndex = sel;

    emit candidatesChanged();
    emit selectedShowIndexChanged();
    rebuildEpisodeCount();

    if (list.isEmpty()) {
        setStatus(tr("No AniList match for \u201c%1\u201d").arg(m_searchQuery), false);
        applyReset();
        return;
    }
    queryAniSkip();
}

void SkipTimes::setSearchQuery(const QString &q) {
    if (q == m_searchQuery) return;
    m_searchQuery = q;
    emit searchQueryChanged();
    if (m_isOnline && aniskipEnabled()) runSearch();
}

void SkipTimes::setSelectedShowIndex(int i) {
    if (i == m_selectedShowIndex || i < 0 || i >= m_candidates.size()) return;
    m_selectedShowIndex = i;
    emit selectedShowIndexChanged();
    if (!m_showLink.isEmpty()) {
        m_malIdCache.insert(m_showLink, m_candidates[i].malId);
        Settings::instance().setValue(Config::skipMal(m_showLink),
                                       QString::number(m_candidates[i].malId));
    }
    rebuildEpisodeCount();
    queryAniSkip();
}

void SkipTimes::setSelectedEpisodeIndex(int e) {
    if (m_candidates.isEmpty() || e == m_selectedEpisodeIndex || e <= 0 || e > m_episodeCount) return;
    m_selectedEpisodeIndex = e;
    emit selectedEpisodeIndexChanged();
    queryAniSkip();
}

void SkipTimes::rebuildEpisodeCount() {
    int n = 0;
    if (m_candidates.isEmpty()) {
        if (m_episodeCount != 0) { m_episodeCount = 0; emit episodeCountChanged(); }
        return;
    }
    if (m_selectedShowIndex >= 0 && m_selectedShowIndex < m_candidates.size())
        n = m_candidates[m_selectedShowIndex].episodes;
    if (n <= 0) n = m_playlistEpisodes;
    n = qMax(n, m_selectedEpisodeIndex);
    if (n <= 0) n = 1;
    if (n != m_episodeCount) { m_episodeCount = n; emit episodeCountChanged(); }
}

void SkipTimes::clearCandidates() {
    if (m_candidates.isEmpty() && m_showTitles.isEmpty() && m_selectedShowIndex == -1) return;
    m_candidates.clear();
    m_showTitles.clear();
    m_selectedShowIndex = -1;
    emit candidatesChanged();
    emit selectedShowIndexChanged();
    rebuildEpisodeCount();
}

void SkipTimes::queryAniSkip() {
    const int malId = currentMalId();
    if (malId <= 0) { setStatus(tr("No show selected"), false); return; }
    if (!aniskipEnabled()) return;
    if (m_duration <= 0) { setStatus(tr("Waiting for video..."), true); return; }

    setStatus(tr("Querying AniSkip..."), true);
    m_skipCancel.cancel();
    m_skipCancel = CancelToken{};

    const int episode = m_selectedEpisodeIndex, duration = m_duration;
    m_runs.add(QtConcurrent::run([this, malId, episode, duration, cancel = m_skipCancel]() {
        Client client = Client(cancel, false).withSession(QStringLiteral("aniskip"));
        const QString url = QString("https://api.aniskip.com/v2/skip-times/%1/%2?types=op&types=ed&episodeLength=%3")
                                .arg(malId).arg(episode).arg(duration);
        const auto resp = client.get(url, {{"Accept", "application/json"}});
        if (cancel.isCancelled()) return;

        const bool apiOk = resp.code == 200 || resp.code == 404;
        const auto obj = resp.toJsonObject();
        const bool found = resp.code == 200 && obj.value("found").toBool();
        int opStart = -1, opEnd = -1, edStart = -1, edEnd = -1;
        if (found) {
            for (const auto &v : obj.value("results").toArray()) {
                const auto r = v.toObject();
                const auto interval = r.value("interval").toObject();
                const double start = interval.value("startTime").toDouble(-1);
                const double end = interval.value("endTime").toDouble(-1);
                if (!std::isfinite(start) || !std::isfinite(end) || start < 0 || end <= start || end > duration) continue;
                int st = static_cast<int>(std::lround(start));
                int et = static_cast<int>(std::lround(end));
                const QString type = r.value("skipType").toString();
                if (type == "op")      { opStart = st; opEnd = et; }
                else if (type == "ed") { edStart = st; edEnd = et; }
            }
        }

        QMetaObject::invokeMethod(this, [this, apiOk, found, opStart, opEnd, edStart, edEnd, duration, episode, malId, cancel]() {
            if (cancel.isCancelled() || m_selectedEpisodeIndex != episode || currentMalId() != malId) return;
            applyTimes(opStart, opEnd, edStart, edEnd, duration);
            setSkipTimes(opStart, opEnd, edStart, edEnd);
            const QString show = m_selectedShowIndex >= 0 && m_selectedShowIndex < m_candidates.size()
                                     ? m_candidates[m_selectedShowIndex].title : m_searchQuery;
            logOk() << "Skip" << QStringLiteral("AniSkip \"%1\" · MAL %2 · ep %3 -> %4")
                .arg(show).arg(malId).arg(episode)
                .arg(!apiOk ? QStringLiteral("API unavailable")
                            : found ? QStringLiteral("OP[%1s-%2s] ED[%3s-%4s]").arg(opStart).arg(opEnd).arg(edStart).arg(edEnd)
                                    : QStringLiteral("no timestamps"));
            if (!apiOk) {
                setStatus(tr("AniSkip unavailable · MAL %1 · ep %2").arg(malId).arg(episode), false);
                return;
            }
            if (!found) {
                setStatus(tr("No AniSkip data · MAL %1 · ep %2").arg(malId).arg(episode), false);
                return;
            }
            QStringList parts;
            if (opStart >= 0 && opEnd > opStart) parts << QStringLiteral("intro");
            if (edStart > 0 && edEnd > edStart)  parts << QStringLiteral("outro");
            setStatus(parts.isEmpty()
                ? QStringLiteral("MAL %1 · ep %2 · no timestamps").arg(malId).arg(episode)
                : QStringLiteral("%1 found · MAL %2 · ep %3").arg(parts.join(QStringLiteral(" & "))).arg(malId).arg(episode),
                false);
        }, Qt::QueuedConnection);
    }));
}

void SkipTimes::applyTimes(int opStart, int opEnd, int edStart, int edEnd, int duration) {
    auto *mpv = MpvPlayer::instance();
    if (!mpv) return;
    m_applying = true;
    const bool hasOP = opStart >= 0 && opEnd > opStart;
    const bool hasED = edStart > 0 && edEnd > edStart && duration > edStart;
    mpv->setAniOPStart (hasOP ? opStart : 0);
    mpv->setAniOPLength(hasOP ? opEnd - opStart : 0);
    mpv->setAniEDLength(hasED ? duration - edStart : 0);   // mpv treats ED as seconds-from-end
    m_applying = false;
}

void SkipTimes::applyReset() {
    auto *mpv = MpvPlayer::instance();
    if (!mpv) return;
    m_applying = true;
    mpv->setAniOPStart(0);
    mpv->setAniOPLength(0);
    mpv->setAniEDLength(0);
    m_applying = false;
    setSkipTimes(-1, -1, -1, -1);
}

QString SkipTimes::formatTime(int seconds) {
    if (seconds < 0) return {};
    if (seconds >= 3600)
        return QStringLiteral("%1:%2:%3").arg(seconds / 3600)
                                         .arg((seconds % 3600) / 60, 2, 10, QChar('0'))
                                         .arg(seconds % 60, 2, 10, QChar('0'));
    return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QChar('0'));
}

void SkipTimes::setSkipTimes(int opStart, int opEnd, int edStart, int edEnd) {
    const bool hasOP = opStart >= 0 && opEnd > opStart;
    const bool hasED = edStart > 0 && edEnd > edStart;
    QString intro = hasOP ? QStringLiteral("%1 - %2").arg(formatTime(opStart), formatTime(opEnd)) : QString();
    QString outro = hasED ? QStringLiteral("%1 - %2").arg(formatTime(edStart), formatTime(edEnd)) : QString();
    if (intro == m_introRange && outro == m_outroRange) return;
    m_introRange = intro;
    m_outroRange = outro;
    emit skipTimesChanged();
}

void SkipTimes::setStatus(const QString &text, bool busy) {
    bool changed = false;
    if (m_status != text) { m_status = text; changed = true; }
    if (m_busy != busy)   { m_busy = busy;   changed = true; }
    if (changed) emit statusChanged();
}

void SkipTimes::loadProfile(const QString &showLink) {
    auto *mpv = MpvPlayer::instance();
    if (!mpv) return;
    const QString val = Settings::instance().value(Config::skipProfile(showLink)).toString();
    m_applying = true;
    if (val.isEmpty()) {
        loadFallback();
        mpv->setSkipOP(false);
        mpv->setSkipED(false);
    } else if (const auto p = val.split(','); p.size() == 5) {
        mpv->setOPStart(p[0].toInt());
        mpv->setOPLength(p[1].toInt());
        mpv->setEDLength(p[2].toInt());
        mpv->setSkipOP(p[3].toInt() != 0);
        mpv->setSkipED(p[4].toInt() != 0);
    } else {
        // Without this an older profile leaves the previous show's marks in place.
        loadFallback();
        mpv->setSkipOP(false);
        mpv->setSkipED(false);
    }
    mpv->setAniOPStart(0);
    mpv->setAniOPLength(0);
    mpv->setAniEDLength(0);
    m_applying = false;
}

void SkipTimes::saveProfile() {
    auto *mpv = MpvPlayer::instance();
    if (!mpv || m_showLink.isEmpty()) return;
    const QString val = QString("%1,%2,%3,%4,%5")
        .arg(mpv->skipOPStart()).arg(mpv->skipOPLength()).arg(mpv->skipEDLength())
        .arg(mpv->skipOP() ? 1 : 0).arg(mpv->skipED() ? 1 : 0);
    Settings::instance().setValue(Config::skipProfile(m_showLink), val);
}

void SkipTimes::loadFallback() {
    auto *mpv = MpvPlayer::instance();
    if (!mpv) return;
    mpv->setOPStart(Settings::instance().get(Config::SkipOPStart));
    mpv->setOPLength(Settings::instance().get(Config::SkipOPLength));
    mpv->setEDLength(Settings::instance().get(Config::SkipEDLength));
}

void SkipTimes::saveFallback() {
    auto *mpv = MpvPlayer::instance();
    if (!mpv) return;
    Settings::instance().set(Config::SkipOPStart,  static_cast<int>(mpv->skipOPStart()));
    Settings::instance().set(Config::SkipOPLength, static_cast<int>(mpv->skipOPLength()));
    Settings::instance().set(Config::SkipEDLength, static_cast<int>(mpv->skipEDLength()));
}
