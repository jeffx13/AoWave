#pragma once

#include <QAbstractListModel>
#include <QFutureWatcher>
#include <QSqlDatabase>
#include <QDir>
#include <QProcess>
#include <QMutex>
#include <QThreadPool>
#include <QTimer>
#include <QSharedPointer>
#include <atomic>
#include <optional>

#include "app/async.h"
#include "library/downloads/downloadtask.h"
#include <qqmlintegration.h>

struct ShowData;
class PlaylistItem;
class Provider;
class Client;

class DownloadQueue : public QAbstractListModel {
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(int maxDownloads READ maxDownloads WRITE setMaxDownloads NOTIFY maxDownloadsChanged)
    Q_PROPERTY(QString overview READ overview NOTIFY overviewChanged)
    Q_PROPERTY(int downloading READ downloading NOTIFY overviewChanged)
    Q_PROPERTY(QVariantList finished READ finished NOTIFY finishedChanged)
public:
    enum Role { NameRole = Qt::UserRole, PathRole, ProgressValueRole, ProgressTextRole, StatusRole, StatsRole };

    explicit DownloadQueue(QObject *parent = nullptr);
    ~DownloadQueue() override;

    Q_INVOKABLE void downloadLink(const QString &name, const QString &link);
    void downloadShow(const ShowData &show, int startIndex, int endIndex);
    static QString cleanFolderName(const QString &name);
    static QString showFolder(const QString &showTitle);
    static QString episodeFileName(const PlaylistItem &episode);
    enum EpisodeState { NotDownloaded, Queued, Downloaded };
    EpisodeState episodeState(const QString &showTitle, const QString &showLink, const PlaylistItem &episode) const;

    QString downloadedFile(const QString &showLink, const QString &episodeLink) const;
    struct Kept {
        bool valid = false;
        QString showLink, provider, showTitle, cover, episodeLink;
        int episodeIndex = -1;
        float episodeNumber = -1;
        int season = 0;
    };
    Kept keptFor(const QString &path) const;
    Q_INVOKABLE void cancelTask(int index);
    Q_INVOKABLE void pauseTask(int index);
    Q_INVOKABLE void resumeTask(int index);
    Q_INVOKABLE void pauseAll();
    Q_INVOKABLE void resumeAll();
    Q_INVOKABLE QVariantList holdCancel(int index);
    Q_INVOKABLE void settleCancel(const QVariantList &held);
    Q_INVOKABLE void undoCancel(const QVariantList &held);
    Q_INVOKABLE void forgetFinished(const QString &path);
    Q_INVOKABLE void clearFinished();
    Q_INVOKABLE static void showInFolder(const QString &path);
    QVariantList finished() const;
    Q_INVOKABLE void pruneMissing();

    void saveQueue() const;
    void restoreQueue();
    static QSqlDatabase &database();

    int maxDownloads() const;
    QString overview() const;
    int downloading() const;
    void setMaxDownloads(int newMaxDownloads);

    int count() const { return m_tasks.count(); }
    DownloadTask *taskAt(int i) const {
        return (i >= 0 && i < m_tasks.size()) ? m_tasks.at(i).data() : nullptr;
    }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void maxDownloadsChanged();
    void overviewChanged();
    void finishedChanged();
    void pruned(int removed);
    void downloadFinished(const QString &path, const QString &title);

private:
    void startTasks();
    void runTask(QSharedPointer<DownloadTask> task);
    void removeTask(const QSharedPointer<DownloadTask> &task, bool force = false);
    void recordFinished(const DownloadTask &task);
    mutable QHash<QString, QHash<QString, QString>> m_keptByShow;
    struct Gone {
        QString path, variant;   // variant: the same download under the other container's extension
    };
    void applyPruned(const QList<Gone> &gone);
    QFutureWatcher<QList<Gone>> m_pruneWatcher;
    mutable QTimer m_pruneSoon;
    bool m_pruneAgain = false;
    mutable qint64 m_prunedAtMs = 0;
    void emitRowChanged(int row);
    int  rowOf(const QSharedPointer<DownloadTask> &task) const;
    int  rowOfPath(const QString &path) const;

    static constexpr int kMaxDownloadsLimit = 8;
    int m_maxDownloads;
    std::atomic<int> m_currentConcurrentDownloads{0};
    mutable QMutex m_mutex;
    QSet<QString> m_ongoingPaths;
    QList<QSharedPointer<DownloadTask>> m_taskQueue;
    QList<QSharedPointer<DownloadTask>> m_tasks;
    CancelToken m_resolveCancel;
    QThreadPool m_threadPool;
};
