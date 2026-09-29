#include "media/progresssync.h"
#include "core/logger.h"
#include "core/settings.h"
#include "net/client.h"
#include "shows/playlistitem.h"
#include "shows/showprovider.h"
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
    // The finished signal will not be delivered now, so a push that failed is queued here.
    disconnect(&m_watcher, nullptr, this, nullptr);
    waitFor(m_watcher, "Progress sync");
    if (m_watcher.future().resultCount() > 0) apply(m_watcher.result());
}

void ProgressSync::push(const QSharedPointer<PlaylistItem> &item, double seconds, double duration) {
    auto playlist = item->parent();
    ShowProvider *provider = playlist ? playlist->provider() : nullptr;
    if (!provider || !provider->syncsProgress() || !(item->type & PlaylistItem::Online)
        || m_watcher.isRunning() || !Settings::instance().bilibiliSync()) return;

    const QString service = provider->name();
    const QString link = item->link;
    // A stall or a recovery reload must not rewind the account; a real backward seek may.
    Library::SyncState state = m_hooks.read ? m_hooks.read(link, service) : Library::SyncState{};
    if (state.valid && seconds + 5.0 < state.pushedPos) return;

    const QList<Library::OutboxEntry> pending =
        m_hooks.pending ? m_hooks.pending(service) : QList<Library::OutboxEntry>{};

    m_watcher.setFuture(QtConcurrent::run(
        [provider, service, link, seconds, duration, state, pending, cancel = m_cancel]() {
            Outcome outcome{service, {}, link, seconds, duration, false, false, state};
            Client client(cancel, false);
            try {
                // Whatever a previous attempt could not deliver goes first.
                for (const Library::OutboxEntry &entry : pending) {
                    if (cancel.isCancelled()) return outcome;
                    if (provider->reportProgress(&client, entry.episodeLink, entry.seconds, entry.duration))
                        outcome.delivered.append(entry);
                }
                if (cancel.isCancelled()) return outcome;

                outcome.attempted = true;
                outcome.pushed = provider->reportProgress(&client, link, seconds, duration);
            } catch (const std::exception &e) {
                logWarn() << "Playlist" << "progress sync failed:" << e.what();
                return outcome;
            }
            if (outcome.pushed) {
                outcome.state.pushedPos = seconds;
                outcome.state.basePos = seconds;
                outcome.state.baseServerAt = QDateTime::currentSecsSinceEpoch() + provider->clockSkew();
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
    ShowProvider *provider = playlist ? playlist->provider() : nullptr;
    if (!provider || !provider->syncsProgress() || !(item->type & PlaylistItem::Online)
        || duration <= 0 || !Settings::instance().bilibiliSync() || !m_hooks.read) return;

    const QString service = provider->name();
    const QString link = item->link;
    if (m_reconciled.contains(link)) return;
    m_reconciled.insert(link);

    const Library::SyncState state = m_hooks.read(link, service);
    const double watched = Settings::instance().watchedFraction() * duration;

    m_reconciles.add(QtConcurrent::run(
        [this, provider, service, link, localPos, duration, state, watched,
         cancel = m_cancel]() mutable {
        Client client(cancel, false);
        const ShowProvider::RemoteProgress remote = provider->fetchProgress(&client, link);
        if (cancel.isCancelled() || !remote.valid) return;

        // 5s absorbs heartbeat granularity and rounding on both sides.
        constexpr double kEpsilon = 5.0;
        const bool localMoved  = std::abs(localPos - state.basePos) > kEpsilon;
        const bool remoteMoved = std::abs(remote.seconds - state.basePos) > kEpsilon;
        if (!localMoved && !remoteMoved) return;

        double winner = localPos;
        if (!localMoved)       winner = remote.seconds;
        else if (!remoteMoved) winner = localPos;
        else {
            // Never un-finish an episode.
            const bool localDone = localPos >= watched, remoteDone = remote.seconds >= watched;
            if (localDone != remoteDone) {
                winner = localDone ? localPos : remote.seconds;
            } else {
                // A deliberate restart always carries a newer timestamp. Skew-corrected.
                const qint64 localAt = QDateTime::currentSecsSinceEpoch() + provider->clockSkew();
                const bool remoteNewer = remote.serverAt > localAt + 60;
                const bool localNewer  = localAt > remote.serverAt + 60;
                if (remoteNewer && remote.seconds < localPos)      winner = remote.seconds;
                else if (localNewer && localPos < remote.seconds)  winner = localPos;
                else winner = std::max(localPos, remote.seconds);
            }
        }

        if (std::abs(winner - remote.seconds) > kEpsilon)
            provider->reportProgress(&client, link, winner, duration);

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
