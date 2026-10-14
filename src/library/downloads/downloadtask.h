#pragma once

#include <QObject>
#include <QProcess>
#include <QSharedPointer>
#include <atomic>
#include <optional>

#include "media/player/danmaku.h"
#include "app/async.h"
#include "providers/showdata.h"
#include <qqmlintegration.h>

class PlaylistItem;
class Provider;
class Client;

class DownloadTask : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by DownloadQueue; QML uses it for Status.")
public:
    DownloadTask(const QString &videoName, const QString &folder, const QString &link,
                 const QString &displayName, const QMap<QString, QString> &headers = {});

    DownloadTask(QSharedPointer<PlaylistItem> episode, Provider *provider, const QString &workDir);

    ~DownloadTask() override = default;

    enum Status { Queued, Running, Paused, Failed };
    Q_ENUM(Status)

    struct SubtitleFile {
        QString path;
        QString lang;
        QString name;
        bool    owned;
        bool    danmaku = false;
    };
    QString danmakuXml;

    QString videoName;
    QString folder;
    QString link;
    QString audioLink;
    QString videoTitle;
    QString audioTitle;
    QMap<QString, QString> headers;
    QString displayName;
    QString maxSpeed;   // read on the GUI thread; QSettings is not thread-safe
    int maxHeight = 0;
    std::optional<DanmakuOptions> danmaku;
    QList<SubtitleFile> subtitleFiles;

    QString basePath() const { return QDir::cleanPath(folder + "/" + videoName); }
    QString path() const     { return basePath() + (useMkv() ? ".mkv" : ".mp4"); }
    QString partPath() const { return basePath() + (useMkv() ? ".part.mkv" : ".part.mp4"); }
    QString tmpDir() const   { return basePath() + QStringLiteral(".tmp"); }

    struct Origin {
        QString provider;
        QString episodeLink;
        QString showName;
        float   episodeNumber = -1;
        int     episodeSeason = 0;
        QString showLink;
        QString cover;
        int     episodeIndex = -1;
    };
    Origin origin;

    static QSharedPointer<DownloadTask> restore(const QString &savedName, const QString &savedFolder,
                                                const QString &savedLink, const QString &savedLabel,
                                                const Origin &savedOrigin);

    QStringList toolArguments() const;
    bool usesFfmpeg() const { return !audioLink.isEmpty(); }
    bool useMkv() const { return m_useMkv.load(std::memory_order_acquire); }
    QString program() const { return usesFfmpeg() ? ffmpegPath() : toolPath(); }
    QStringList ffmpegArguments() const;
    QString extractLink();
    QString extractLinkInner();
    void refreshLink();
    void prepareSubtitles();
    void discardSubtitles();
    void addDanmaku();

    // A failed rename leaves the only copy in the .part file.
    std::atomic<bool> keepPart{false};

    int progressValue() const { return m_progressValue; }
    QString progressText() const { return m_progressText; }
    QString stats() const { return m_stats; }
    double rate() const { return m_rate; }
    static QString formatEta(int seconds);
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
    void fetchDanmaku(const Source &source);

    CancelToken       m_cancel;
    std::atomic<bool> m_isPaused{false};
    std::atomic<bool> m_useMkv{false};
    std::atomic<int>  m_status{Queued};
    std::atomic<QProcess*> m_process{nullptr};
    int m_progressValue = 0;
    QString m_progressText = tr("Waiting to start…");

    QString m_stats;
    QString m_speed;
    QString m_etaText;
    double  m_rate = 0;
    qint64  m_startTimeMs = 0;
    int     m_startProgress = 0;

    QSharedPointer<PlaylistItem> m_episode;
    Provider *m_provider = nullptr;
    std::function<QUrl()> m_refreshPrimaryVideoUrl;
};
