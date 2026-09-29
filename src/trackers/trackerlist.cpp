#include "trackers/trackerlist.h"
#include "core/logger.h"
#include "core/settings.h"
#include <QtConcurrent/QtConcurrentRun>

TrackerList::~TrackerList() {
    m_cancel.cancel();
    m_runs.waitAll("TrackerList request");
    for (Tracker *tracker : std::as_const(m_trackers)) tracker->shutdown();
}

void TrackerList::setTrackers(QList<Tracker *> &&trackers) {
    beginResetModel();
    m_trackers = std::move(trackers);
    s_byName.clear();
    for (Tracker *tracker : std::as_const(m_trackers)) {
        tracker->setParent(this);
        s_byName.insert(tracker->name(), tracker);
        connect(tracker, &Tracker::authChanged, this, [this]() {
            emit changed();
            if (!m_trackers.isEmpty())
                emit dataChanged(index(0), index(int(m_trackers.size()) - 1));
        });
    }
    endResetModel();
    emit changed();
}

int TrackerList::authenticatedCount() const {
    int n = 0;
    for (Tracker *tracker : m_trackers) if (tracker->isAuthenticated()) ++n;
    return n;
}

QStringList TrackerList::authenticatedNames() const {
    QStringList names;
    for (Tracker *tracker : m_trackers) if (tracker->isAuthenticated()) names << tracker->name();
    return names;
}

void TrackerList::authenticate(const QString &name) {
    if (Tracker *tracker = byName(name)) tracker->authenticate();
}

void TrackerList::signOut(const QString &name) {
    if (Tracker *tracker = byName(name)) tracker->signOut();
}

QStringList TrackerList::statusVocabulary(const QString &name) const {
    Tracker *tracker = byName(name);
    return tracker ? tracker->statusVocabulary() : QStringList{};
}

QString TrackerList::clientId(const QString &name) const {
    Tracker *tracker = byName(name);
    return tracker ? tracker->clientId() : QString{};
}

void TrackerList::setClientId(const QString &name, const QString &id) {
    if (Tracker *tracker = byName(name)) tracker->setClientId(id);
}

bool TrackerList::needsClientSecret(const QString &name) const {
    Tracker *tracker = byName(name);
    return tracker && tracker->needsClientSecret();
}

QString TrackerList::clientSecret(const QString &name) const {
    Tracker *tracker = byName(name);
    return tracker ? tracker->clientSecret() : QString{};
}

void TrackerList::setClientSecret(const QString &name, const QString &secret) {
    if (Tracker *tracker = byName(name)) tracker->setClientSecret(secret);
}

bool TrackerList::canAuthenticate(const QString &name) const {
    Tracker *tracker = byName(name);
    return tracker && tracker->canAuthenticate();
}

QString TrackerList::registrationUrl(const QString &name) const {
    const Tracker *tracker = byName(name);
    return tracker ? tracker->registrationUrl() : QString{};
}

QString TrackerList::registrationHint(const QString &name) const {
    const Tracker *tracker = byName(name);
    return tracker ? tracker->registrationHint() : QString{};
}

// POINT_10_DECIMAL is the only fractional one.
QVariantMap TrackerList::scoreScale(const QString &name) const {
    Tracker *tracker = byName(name);
    if (!tracker) return {{"max", 10}, {"step", 1}, {"decimals", 0}};
    switch (tracker->scoreFormat()) {
    case Tracker::ScoreFormat::Point100:       return {{"max", 100}, {"step", 1},  {"decimals", 0}};
    case Tracker::ScoreFormat::Point5:         return {{"max", 5},   {"step", 1},  {"decimals", 0}};
    case Tracker::ScoreFormat::Point3:         return {{"max", 3},   {"step", 1},  {"decimals", 0}};
    case Tracker::ScoreFormat::Point10Decimal: return {{"max", 10},  {"step", 1},  {"decimals", 1}};
    case Tracker::ScoreFormat::Point10:        break;
    }
    return {{"max", 10}, {"step", 1}, {"decimals", 0}};
}

// One place decides whether an automatic push is wanted; progress only moves forward.
void TrackerList::autoPush(const QString &showLink, int episodeNumber,
                           const std::function<QVariantMap(QString, QString)> &linkLookup) {
    if (episodeNumber <= 0 || showLink.isEmpty()) return;
    if (!Settings::instance().trackerAutoPush()) return;

    for (Tracker *tracker : std::as_const(m_trackers)) {
        if (!tracker->isAuthenticated()) continue;
        const QVariantMap link = linkLookup(showLink, tracker->name());
        if (!link.value(QStringLiteral("valid")).toBool()) continue;
        const QString remoteId = link.value(QStringLiteral("remoteId")).toString();
        if (remoteId.isEmpty()) continue;
        if (link.value(QStringLiteral("progress")).toInt() >= episodeNumber) continue;

        QString status = link.value(QStringLiteral("status")).toString();
        if (status.isEmpty()) {
            const QStringList vocabulary = tracker->statusVocabulary();
            status = vocabulary.isEmpty() ? QString() : vocabulary.first();   // "watching"
        }
        logInfo() << tracker->name() << "auto-push episode" << episodeNumber << "for" << remoteId;
        pushEntry(tracker->name(), remoteId, status,
                  link.value(QStringLiteral("score")).toDouble(), episodeNumber);
    }
}

void TrackerList::search(const QString &name, const QString &query) {
    Tracker *tracker = byName(name);
    if (!tracker || query.trimmed().isEmpty()) return;
    m_runs.add(QtConcurrent::run([this, tracker, name, query, cancel = m_cancel]() {
        Client client(cancel, false);
        QVariantList out;
        for (const Tracker::Result &result : tracker->search(&client, query))
            out.append(QVariantMap{{"remoteId", result.remoteId}, {"title", result.title},
                                   {"cover", result.cover}, {"episodes", result.episodes},
                                   {"year", result.year}});
        if (cancel.isCancelled()) return;
        QMetaObject::invokeMethod(this, [this, name, out]() {
            emit searchFinished(name, out);
        }, Qt::QueuedConnection);
    }));
}

// The link is a row in tracker_links; this only fetches the remote entry.
void TrackerList::link(const QString &name, const QString &showLink, const QString &remoteId) {
    Tracker *tracker = byName(name);
    if (!tracker) return;
    m_runs.add(QtConcurrent::run([this, tracker, name, showLink, remoteId, cancel = m_cancel]() {
        Client client(cancel, false);
        const Tracker::Entry entry = tracker->entryFor(&client, remoteId);
        if (cancel.isCancelled()) return;
        QVariantMap out{{"remoteId", entry.remoteId}, {"status", entry.status},
                        {"score", entry.score}, {"progress", entry.progress},
                        {"valid", entry.valid}};
        QMetaObject::invokeMethod(this, [this, name, showLink, out]() {
            emit entryLoaded(name, showLink, out);
        }, Qt::QueuedConnection);
    }));
}

void TrackerList::unlink(const QString &name, const QString &showLink) {
    emit entryLoaded(name, showLink, QVariantMap{{"valid", false}});
}

void TrackerList::pushEntry(const QString &name, const QString &showLink, const QString &status,
                            double score, int progress) {
    Tracker *tracker = byName(name);
    if (!tracker) return;
    m_runs.add(QtConcurrent::run([this, tracker, name, showLink, status, score, progress, cancel = m_cancel]() {
        Client client(cancel, false);
        Tracker::Entry entry;
        entry.remoteId = showLink;   // the remote id the caller stored for this show
        entry.status = status;
        entry.score = score;
        entry.progress = progress;
        const bool ok = tracker->update(&client, entry);
        if (cancel.isCancelled()) return;
        QMetaObject::invokeMethod(this, [this, name, ok]() {
            emit pushFinished(name, ok);
        }, Qt::QueuedConnection);
    }));
}

int TrackerList::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : int(m_trackers.size());
}

QVariant TrackerList::data(const QModelIndex &index, int role) const {
    if (index.row() < 0 || index.row() >= m_trackers.size()) return {};
    Tracker *tracker = m_trackers.at(index.row());
    switch (role) {
    case NameRole:          return tracker->name();
    case AuthenticatedRole: return tracker->isAuthenticated();
    case AccountRole:       return tracker->accountName();
    default:                return {};
    }
}

QHash<int, QByteArray> TrackerList::roleNames() const {
    return {{NameRole, "name"}, {AuthenticatedRole, "authenticated"}, {AccountRole, "account"}};
}
