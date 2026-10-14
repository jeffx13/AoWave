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
class DownloadQueue;

class DownloadList : public QAbstractListModel {
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(bool showHidden READ showHidden WRITE setShowHidden NOTIFY hiddenChanged)
    Q_PROPERTY(int hiddenCount READ hiddenCount NOTIFY hiddenChanged)
public:
    enum Role { KeyRole = Qt::UserRole, GroupRole, FolderRole, TitleRole, CoverRole, PathRole, StatusRole, ProgressRole,
                ProgressTextRole, StatsRole, DetailRole, TaskRowRole, CountRole, ActiveRole, FailedRole, SizeRole,
                AtRole, OpenRole, HiddenRole, ShowLinkRole, TotalRole, TotalSizeRole, TasksRole, PausedRole };

    explicit DownloadList(DownloadQueue *queue);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    bool showHidden() const { return m_showHidden; }
    void setShowHidden(bool show);
    int hiddenCount() const { return m_hiddenCount; }

    Q_INVOKABLE void setOpen(const QString &folder, bool open);
    Q_INVOKABLE qint64 hide(const QString &folder);
    Q_INVOKABLE void unhide(const QString &folder, qint64 mark = 0);
    void rebuild();

signals:
    void hiddenChanged();

private:
    struct Row {
        QString key, folder, title, cover, path, progressText, stats, detail, showLink;
        bool group = false, open = false, hidden = false;
        int status = -1, progress = 0, taskRow = -1, count = 0, active = 0, paused = 0, failed = 0, season = 0;
        int tasks = 0;
        int total = 0;
        double number = -1;
        qint64 size = 0, at = 0, totalSize = 0;
        bool operator==(const Row &) const = default;
    };
    static Row rowFor(int taskRow, const DownloadTask &task);
    void aggregate(Row &group) const;
    void refreshTask(int row);
    void dropTasks(int first, int last);
    void apply(const QList<Row> &next);
    void store(const QString &folder, const char *column, const QVariant &value);

    DownloadQueue *m_queue;
    QList<Row> m_rows;
    QTimer m_rebuildSoon;
    bool m_showHidden = false;
    int m_hiddenCount = 0;
};

class DownloadQueue : public QAbstractListModel {
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(int maxDownloads READ maxDownloads WRITE setMaxDownloads NOTIFY maxDownloadsChanged)
    Q_PROPERTY(QString overview READ overview NOTIFY overviewChanged)
    Q_PROPERTY(int downloading READ downloading NOTIFY overviewChanged)
    Q_PROPERTY(int taskCount READ count NOTIFY overviewChanged)
    Q_PROPERTY(int active READ active NOTIFY overviewChanged)
    Q_PROPERTY(int paused READ paused NOTIFY overviewChanged)
    Q_PROPERTY(int failed READ failed NOTIFY overviewChanged)
    Q_PROPERTY(DownloadList *list READ list CONSTANT)
public:
    enum Role { NameRole = Qt::UserRole, PathRole, ProgressValueRole, ProgressTextRole, StatusRole, StatsRole };

    explicit DownloadQueue(QObject *parent = nullptr);
    ~DownloadQueue() override;

    Q_INVOKABLE bool downloadLink(const QString &name, const QString &link);
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
    Q_INVOKABLE void retryFailed();
    Q_INVOKABLE QVariantList holdCancel(int index);
    Q_INVOKABLE void settleCancel(const QVariantList &held);
    Q_INVOKABLE void undoCancel(const QVariantList &held);
    Q_INVOKABLE int deleteDownloads(const QString &folder, const QString &path = QString());
    Q_INVOKABLE static void showInFolder(const QString &path);
    Q_INVOKABLE static void openFolder(const QString &folder);
    QVariantList finished() const;
    Q_INVOKABLE void pruneMissing();
    // -1 file gone, -2 not looked at yet
    qint64 knownSize(const QString &path) const { return m_sizes.value(path, -2); }
    DownloadList *list() { return &m_list; }

    void saveQueue() const;
    void restoreQueue();
    static QSqlDatabase &database();

    int maxDownloads() const;
    QString overview() const;
    int downloading() const;
    int active() const;
    int paused() const;
    int failed() const;
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
    void trashed(const QStringList &paths);
    void queued(int added, int skipped);

private:
    void startTasks();
    void runTask(QSharedPointer<DownloadTask> task);
    void resume(const QSharedPointer<DownloadTask> &task);
    static constexpr int kAutoRetries = 3;
    void removeTask(const QSharedPointer<DownloadTask> &task, bool force = false);
    void recordFinished(const DownloadTask &task);
    mutable QHash<QString, QHash<QString, QString>> m_keptByShow;
    struct Gone {
        QString path, variant;
    };
    struct Pruned {
        QList<Gone> gone;
        QHash<QString, qint64> sizes;
    };
    void applyPruned(const Pruned &result);
    QFutureWatcher<Pruned> m_pruneWatcher;
    QHash<QString, qint64> m_sizes;
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
    QSet<QString> m_dropFolders;
    CancelToken m_resolveCancel;
    QThreadPool m_threadPool;
    QThreadPool m_resolvePool;
    DownloadList m_list{this};
};
