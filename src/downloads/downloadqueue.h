#pragma once

#include <QAbstractListModel>
#include <QSqlDatabase>
#include <QDir>
#include <QProcess>
#include <QMutex>
#include <QThreadPool>
#include <QSharedPointer>
#include <atomic>

#include "net/canceltoken.h"
#include "shows/playinfo.h"
#include <qqmlintegration.h>

struct ShowData;
class PlaylistItem;
class ShowProvider;
class Client;

class DownloadTask : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by DownloadQueue; QML uses it for Status.")
public:
    DownloadTask(const QString &videoName, const QString &folder, const QString &link,
                 const QString &displayName, const QMap<QString, QString> &headers = {});

    DownloadTask(QSharedPointer<PlaylistItem> episode, ShowProvider *provider, const QString &workDir);

    ~DownloadTask() override = default;

    enum Status { Queued, Running, Paused, Failed };
    Q_ENUM(Status)

    struct SubtitleFile {
        QString path;
        QString lang;    // ISO code, or empty when the provider gave none
        QString name;
        bool    owned;   // ours to delete; a provider's own local file we never touch
    };

    QString videoName;
    QString folder;
    QString link;
    QString audioLink;   // set when the source has a separate audio stream (Bilibili DASH)
    QString videoTitle;
    QString audioTitle;
    QMap<QString, QString> headers;
    QString displayName;
    QString maxSpeed;   // read on the GUI thread; QSettings is not thread-safe
    // 0 for the tallest. Fixed when queued, so a resume keeps to the variant its segments came from.
    int maxHeight = 0;
    QList<SubtitleFile> subtitleFiles;

    // Dedup key. Extension-less: the container is only known once extracted.
    QString basePath() const { return QDir::cleanPath(folder + "/" + videoName); }
    QString path() const     { return basePath() + (useMkv() ? ".mkv" : ".mp4"); }
    // ffmpeg picks the container from this suffix.
    QString partPath() const { return basePath() + (useMkv() ? ".part.mkv" : ".part.mp4"); }
    QString tmpDir() const   { return basePath() + QStringLiteral(".tmp"); }

    // Where a provider episode came from; empty for a pasted link. Fixed at creation, so the
    // queue can be saved while a worker is still filling in the stream fields.
    struct Origin {
        QString provider;
        QString episodeLink;
        QString showName;
        float   episodeNumber = -1;
        int     episodeSeason = 0;
    };
    Origin origin;

    // Comes back Paused. A provider episode is extracted afresh: last session's link is
    // usually dead.
    static QSharedPointer<DownloadTask> restore(const QString &savedName, const QString &savedFolder,
                                                const QString &savedLink, const QString &savedLabel,
                                                const Origin &savedOrigin);

    QStringList toolArguments() const;
    // Separate video+audio isn't a manifest N_m3u8DL-RE takes; ffmpeg muxes it.
    bool usesFfmpeg() const { return !audioLink.isEmpty(); }
    bool useMkv() const { return m_useMkv.load(std::memory_order_acquire); }
    QString program() const { return usesFfmpeg() ? ffmpegPath() : toolPath(); }
    QStringList ffmpegArguments() const;
    QString extractLink();   // empty on failure
    QString extractLinkInner();
    void refreshLink();      // renewed immediately before each downloader run
    // Worker thread, once per run, so the argument list and path() agree.
    void prepareSubtitles();
    void discardSubtitles();

    // A failed rename left the only copy in the .part file.
    std::atomic<bool> keepPart{false};

    int progressValue() const { return m_progressValue; }
    QString progressText() const { return m_progressText; }
    QString stats() const { return m_stats; }
    // Percent a second over this run; 0 until there is enough to tell.
    double rate() const { return m_rate; }
    static QString formatEta(int seconds);
    // A regex for N_m3u8DL-RE's res filter, matching a "WxH" taller than `height`.
    static QString tallerThan(int height);
    void setProgressValue(int value);
    void setProgressText(const QString &text);
    void setSpeed(const QString &speed);
    void resetStats();

    int  status() const { return m_status.load(); }
    void setStatus(int s) { m_status.store(s); }

    bool isCancelled() const { return m_cancel.isCancelled(); }
    void cancel() { m_cancel.cancel(); }
    bool isPaused() const { return m_isPaused.load(); }
    void setPaused(bool p) { m_isPaused = p; }
    QProcess *process() const { return m_process.load(std::memory_order_acquire); }
    void setProcess(QProcess *proc) { m_process.store(proc, std::memory_order_release); }

    static bool checkDependencies();
    static QString toolPath();
    static QString ffmpegPath();

private:
    void rebuildStats();
    void fetchSubtitles(Client &client, const QList<Track> &tracks);

    CancelToken       m_cancel;
    std::atomic<bool> m_isPaused{false};
    std::atomic<bool> m_useMkv{false};
    std::atomic<int>  m_status{Queued};
    std::atomic<QProcess*> m_process{nullptr};
    int m_progressValue = 0;
    QString m_progressText = tr("Waiting to start...");

    QString m_stats;
    QString m_speed;
    QString m_etaText;
    double  m_rate = 0;
    qint64  m_startTimeMs = 0;
    int     m_startProgress = 0;   // a resume does not begin at 0

    QSharedPointer<PlaylistItem> m_episode;
    ShowProvider *m_provider = nullptr;
    std::function<QUrl()> m_refreshPrimaryVideoUrl;
};

class DownloadQueue : public QAbstractListModel {
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(int maxDownloads READ maxDownloads WRITE setMaxDownloads NOTIFY maxDownloadsChanged)
    // "42% overall · 12:05 left" across queued and running tasks; empty when none are.
    Q_PROPERTY(QString overview READ overview NOTIFY overviewChanged)
    // Downloads that finished and are still on disk, newest first: [{path, title, at}].
    Q_PROPERTY(QVariantList finished READ finished NOTIFY finishedChanged)
public:
    enum Role { NameRole = Qt::UserRole, PathRole, ProgressValueRole, ProgressTextRole, StatusRole, StatsRole };

    explicit DownloadQueue(QObject *parent = nullptr);
    ~DownloadQueue() override;

    Q_INVOKABLE void downloadLink(const QString &name, const QString &link);
    void downloadShow(const ShowData &show, int startIndex, int endIndex);
    static QString cleanFolderName(const QString &name);
    // Where downloadShow puts a show's episodes, and an episode's name there before the suffix.
    static QString showFolder(const QString &showTitle);
    static QString episodeFileName(const PlaylistItem &episode);
    enum EpisodeState { NotDownloaded, Queued, Downloaded };
    // Queued covers running; Downloaded is a finished file on disk.
    EpisodeState episodeState(const QString &showTitle, const PlaylistItem &episode) const;
    Q_INVOKABLE void cancelTask(int index);
    Q_INVOKABLE void pauseTask(int index);
    Q_INVOKABLE void resumeTask(int index);
    Q_INVOKABLE void pauseAll();
    Q_INVOKABLE void resumeAll();
    // Cancel with a moment to undo, by path since rows move: holdCancel pauses a task (-1: all)
    // and says which it held, [{path, active}]; settleCancel cancels those still stopped, and
    // undoCancel resumes the ones that were going.
    Q_INVOKABLE QVariantList holdCancel(int index);
    Q_INVOKABLE void settleCancel(const QVariantList &held);
    Q_INVOKABLE void undoCancel(const QVariantList &held);
    Q_INVOKABLE void forgetFinished(const QString &path);
    Q_INVOKABLE void clearFinished();
    Q_INVOKABLE static void showInFolder(const QString &path);
    QVariantList finished() const;

    void saveQueue() const;
    void restoreQueue();
    static QSqlDatabase &database();

    int maxDownloads() const;
    QString overview() const;
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
    void downloadFinished(const QString &path, const QString &title);

private:
    void startTasks();
    void runTask(QSharedPointer<DownloadTask> task);
    void removeTask(const QSharedPointer<DownloadTask> &task, bool force = false);
    void recordFinished(const QString &path, const QString &title);
    void emitRowChanged(int row);   // safe from any thread
    int  rowOf(const QSharedPointer<DownloadTask> &task) const;
    int  rowOfPath(const QString &path) const;

    static constexpr int kMaxDownloadsLimit = 8;   // matches the spin box on the download page
    int m_maxDownloads;
    std::atomic<int> m_currentConcurrentDownloads{0};
    mutable QMutex m_mutex;   // guards the three containers below
    QSet<QString> m_ongoingPaths;
    QList<QSharedPointer<DownloadTask>> m_taskQueue;
    QList<QSharedPointer<DownloadTask>> m_tasks;
    CancelToken m_resolveCancel;
    QThreadPool m_threadPool;
};
