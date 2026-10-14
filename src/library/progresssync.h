#pragma once
#include <QFutureWatcher>
#include <QObject>
#include <QSet>
#include <QSharedPointer>
#include <functional>
#include "app/async.h"
#include "library/library.h"

class PlaylistItem;

// Workers only talk to the provider; the hooks write the database on the GUI thread (or in the destructor).
class ProgressSync : public QObject {
    Q_OBJECT
public:
    struct Hooks {
        std::function<Library::SyncState(const QString &, const QString &)> read;
        std::function<void(const QString &, const QString &, const Library::SyncState &)> write;
        std::function<void(const QString &, const QString &, double, double)> queue;
        std::function<QList<Library::OutboxEntry>(const QString &)> pending;
        std::function<void(const QString &, const Library::OutboxEntry &)> delivered;
    };

    explicit ProgressSync(QObject *parent = nullptr);
    ~ProgressSync() override;

    void setHooks(Hooks hooks) { m_hooks = std::move(hooks); }

    void push(const QSharedPointer<PlaylistItem> &item, double seconds, double duration);
    void reconcile(const QSharedPointer<PlaylistItem> &item, double localPos, double duration);

signals:
    void remoteResumeResolved(QString episodeLink, double seconds);

private:
    struct Outcome {
        QString service;
        QList<Library::OutboxEntry> delivered;
        QString link;
        double seconds = 0, duration = 0;
        bool attempted = false;
        bool pushed = false;
        Library::SyncState state;
    };
    void apply(const Outcome &outcome);

    Hooks                   m_hooks;
    CancelToken             m_cancel;
    QFutureWatcher<Outcome> m_watcher;
    RunSet                  m_reconciles;
    QSet<QString>           m_reconciled;
};
