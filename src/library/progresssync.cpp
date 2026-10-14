#include "library/progresssync.h"
#include "app/logger.h"
#include "app/settings.h"
#include "net/client.h"
#include "providers/showdata.h"
#include "providers/provider.h"
#include <QDateTime>
#include <QtConcurrent/QtConcurrentRun>
#include <cmath>

ProgressSync::ProgressSync(QObject *parent) : QObject(parent) {
    connect(&m_watcher, &QFutureWatcher<Outcome>::finished, this, [this]() {
        apply(m_watcher.result());
    });
}

ProgressSync::~ProgressSync() {
    m_cancel.cancel();
    m_reconciles.waitAll("Progress reconcile");
    disconnect(&m_watcher, nullptr, this, nullptr);
    waitFor(m_watcher, "Progress sync");
    if (m_watcher.future().resultCount() > 0) apply(m_watcher.result());
}

void ProgressSync::push(const QSharedPointer<PlaylistItem> &item, double seconds, double duration) {
    auto playlist = item->parent();
    Provider *provider = playlist ? playlist->provider() : nullptr;
    WatchHistory *history = provider ? provider->watchHistory() : nullptr;
    if (!history || !(item->type & PlaylistItem::Online)
        || m_watcher.isRunning() || !Settings::instance().bilibiliSync()) return;

    const QString service = provider->name();
    const QString link = item->link;
    Library::SyncState state = m_hooks.read ? m_hooks.read(link, service) : Library::SyncState{};
    if (state.valid && seconds + 5.0 < state.pushedPos) return;

    const QList<Library::OutboxEntry> pending =
        m_hooks.pending ? m_hooks.pending(service) : QList<Library::OutboxEntry>{};

    m_watcher.setFuture(QtConcurrent::run(
        [history, service, link, seconds, duration, state, pending, cancel = m_cancel]() {
            Outcome outcome{service, {}, link, seconds, duration, false, false, state};
            Client client(cancel, false);
            try {
                for (const Library::OutboxEntry &entry : pending) {
                    if (cancel.isCancelled()) return outcome;
                    if (history->reportProgress(&client, entry.episodeLink, entry.seconds, entry.duration))
                        outcome.delivered.append(entry);
                }
                if (cancel.isCancelled()) return outcome;

                outcome.attempted = true;
                outcome.pushed = history->reportProgress(&client, link, seconds, duration);
            } catch (const std::exception &e) {
                logWarn() << "Sync" << "progress sync failed:" << e.what();
                return outcome;
            }
            if (outcome.pushed) {
                outcome.state.pushedPos = seconds;
                outcome.state.basePos = seconds;
                outcome.state.baseServerAt = QDateTime::currentSecsSinceEpoch() + history->clockSkew();
                outcome.state.valid = true;
            }
            return outcome;
        }));
}

void ProgressSync::apply(const Outcome &outcome) {
    if (m_hooks.delivered)
        for (const Library::OutboxEntry &entry : outcome.delivered)
            m_hooks.delivered(outcome.service, entry);
    if (!outcome.attempted) return;
    if (outcome.pushed) {
        if (m_hooks.write) m_hooks.write(outcome.link, outcome.service, outcome.state);
    } else if (m_hooks.queue) {
        m_hooks.queue(outcome.link, outcome.service, outcome.seconds, outcome.duration);
    }
}

// Comparing the two positions directly is unanswerable; comparing both against the base is not.
void ProgressSync::reconcile(const QSharedPointer<PlaylistItem> &item, double localPos, double duration) {
    auto playlist = item->parent();
    Provider *provider = playlist ? playlist->provider() : nullptr;
    WatchHistory *history = provider ? provider->watchHistory() : nullptr;
    if (!history || !(item->type & PlaylistItem::Online)
        || duration <= 0 || !Settings::instance().bilibiliSync() || !m_hooks.read) return;

    const QString service = provider->name();
    const QString link = item->link;
    if (m_reconciled.contains(link)) return;
    m_reconciled.insert(link);

    const Library::SyncState state = m_hooks.read(link, service);
    const double watched = Settings::instance().watchedFraction() * duration;

    m_reconciles.add(QtConcurrent::run(
        [this, history, service, link, localPos, duration, state, watched,
         cancel = m_cancel]() mutable {
        Client client(cancel, false);
        const WatchHistory::RemoteProgress remote = history->fetchProgress(&client, link);
        if (cancel.isCancelled() || !remote.valid) return;

        constexpr double kEpsilon = 5.0;
        const bool localMoved  = std::abs(localPos - state.basePos) > kEpsilon;
        const bool remoteMoved = std::abs(remote.seconds - state.basePos) > kEpsilon;
        if (!localMoved && !remoteMoved) return;

        double winner = localPos;
        if (!localMoved)       winner = remote.seconds;
        else if (!remoteMoved) winner = localPos;
        else {
            const bool localDone = localPos >= watched, remoteDone = remote.seconds >= watched;
            if (localDone != remoteDone) {
                winner = localDone ? localPos : remote.seconds;
            } else {
                const qint64 localAt = QDateTime::currentSecsSinceEpoch() + history->clockSkew();
                const bool remoteNewer = remote.serverAt > localAt + 60;
                const bool localNewer  = localAt > remote.serverAt + 60;
                if (remoteNewer && remote.seconds < localPos)      winner = remote.seconds;
                else if (localNewer && localPos < remote.seconds)  winner = localPos;
                else winner = std::max(localPos, remote.seconds);
            }
        }

        if (std::abs(winner - remote.seconds) > kEpsilon)
            history->reportProgress(&client, link, winner, duration);

        Library::SyncState next = state;
        next.basePos = winner;
        next.baseServerAt = remote.serverAt;
        next.pushedPos = std::max(state.pushedPos, winner);
        next.valid = true;
        QMetaObject::invokeMethod(this, [this, link, service, next, winner, localPos]() {
            if (m_hooks.write) m_hooks.write(link, service, next);
            if (std::abs(winner - localPos) > 5.0) emit remoteResumeResolved(link, winner);
        }, Qt::QueuedConnection);
    }));
}
