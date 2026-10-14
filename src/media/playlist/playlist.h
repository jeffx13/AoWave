#pragma once
#include "media/playlist/ytdlp.h"
#include <QAbstractItemModel>
#include <QFileSystemWatcher>
#include <QFuture>
#include <QFutureWatcher>
#include <QHash>
#include <QSet>
#include <QSharedPointer>
#include <QTimer>
#include <QWeakPointer>
#include "providers/showdata.h"
#include "media/playlist/servers.h"
#include "library/progresssync.h"
#include "app/async.h"
#include <qqmlintegration.h>

class Provider;

class Playlist : public QAbstractItemModel {
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(ServerListModel *serverList READ serverList CONSTANT)
    Q_PROPERTY(bool isLoading READ isLoading NOTIFY isLoadingChanged)
    Q_PROPERTY(QString playingList READ playingList NOTIFY currentItemChanged)
public:
    explicit Playlist(QObject *parent = nullptr);
    ~Playlist();

    QSharedPointer<PlaylistItem> find(const QString &link) const;
    void refreshList(const QString &link) {
        if (const auto list = find(link); list && !list->isEmpty()) refreshPath(list->at(0).data());
    }
    int count() const { return m_root->count(); }

    bool isPlaying(const QString &link) const;
    void rekey(const QString &oldLink, const QSharedPointer<PlaylistItem> &newPlaylist);
    bool relink(const QString &oldLink, const QString &newLink);

    int append(const QSharedPointer<PlaylistItem> &playlist, const QSharedPointer<PlaylistItem> &parent = nullptr);
    int insert(int index, const QSharedPointer<PlaylistItem> &playlist, const QSharedPointer<PlaylistItem> &parent = nullptr);
    int replace(int index, const QSharedPointer<PlaylistItem> &playlist, const QSharedPointer<PlaylistItem> &parent = nullptr);
    Q_INVOKABLE void remove(const QModelIndex &index);
    Q_INVOKABLE void clear();

    bool playShowNow(const QSharedPointer<PlaylistItem> &playlist, int episodeIndex = -1);
    Q_INVOKABLE bool playAt(int index);
    Q_INVOKABLE void stepItem(int offset = 1);
    Q_INVOKABLE void stepPlaylist(int offset = 1);
    Q_INVOKABLE void loadIndex(const QModelIndex &index);
    Q_INVOKABLE void reload();
    Q_INVOKABLE void loadServer(int index);
    Q_INVOKABLE void showCurrentItemName() const;
    Q_INVOKABLE void saveProgress(bool quiet = false);
    Q_INVOKABLE void markCurrentWatched();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void openUrl(QUrl url, bool play);

    void appendShow(const QString &title, const QString &link, Provider *provider,
                    QSharedPointer<PlaylistItem> cached, const ShowData::WatchState &info, bool play);
    void mergeEpisodes(const QSharedPointer<PlaylistItem> &list, const QSharedPointer<PlaylistItem> &fresh);

    enum { TitleRole = Qt::UserRole, IndexRole, NumberRole, IsCurrentIndexRole, IsDeletableRole, LinkRole, IsWatchedRole,
           IsLocalRole, ProgressRole };
    bool canRemove(const PlaylistItem *item) const;
    Q_INVOKABLE QModelIndex currentChild(const QModelIndex &idx) const;
    Q_INVOKABLE QString currentShowName() const;
    Q_INVOKABLE QString currentItemName() const;
    Q_INVOKABLE QString nextItemName() const;
    Q_INVOKABLE int     currentShowEpisodeCount() const;
    Q_INVOKABLE QString showLinkAt(const QModelIndex &index) const;
    Q_INVOKABLE QString showProviderAt(const QModelIndex &index) const;
    Q_INVOKABLE QString showTitleAt(const QModelIndex &index) const;
    Q_INVOKABLE QString currentShowLink() const;
    Q_INVOKABLE QString currentShowProvider() const;
    Q_INVOKABLE QVariantMap subtitleHints() const;
    Q_INVOKABLE bool isFilteredOut(const QModelIndex &index, const QString &filter) const;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    QModelIndex index(int row, int column, const QModelIndex &parent) const override;
    QModelIndex parent(const QModelIndex &childIndex) const override;
    int columnCount(const QModelIndex &) const override { return 1; }

    bool isLoading() const {
        return m_watcher.isRunning() || m_linkChecks > 0 || m_pendingItem || m_pendingServerIndex >= 0
               || (m_appendPlays && m_appendFuture.isRunning());
    }

    using LocalResumeLookup = std::function<QList<QPair<QString, double>>(const QString &)>;
    void setLocalResumeLookup(LocalResumeLookup lookup) { m_localResume = std::move(lookup); }

    void setEpisodeResumeLookup(std::function<void(const QSharedPointer<PlaylistItem> &)> lookup) { m_episodeResume = std::move(lookup); }

    void setSyncHooks(ProgressSync::Hooks hooks) { m_progressSync.setHooks(std::move(hooks)); }

    using DownloadLookup = std::function<QString(const QString &showLink, const QString &episodeLink)>;
    void setDownloadLookup(DownloadLookup lookup) { m_downloadLookup = std::move(lookup); }
    struct KeptEpisode {
        bool valid = false;
        QString showLink, provider, showTitle, cover, episodeLink;
        int episodeIndex = -1;
        float number = -1;
    };
    using KeptLookup = std::function<KeptEpisode(const QString &path)>;
    void setKeptLookup(KeptLookup lookup) { m_keptLookup = std::move(lookup); }
    Q_INVOKABLE void queueDownloaded(const QString &path);
    QString playingList() const;
    Q_SIGNAL void remoteResumeResolved(QString episodeLink, double seconds);
    Q_SIGNAL void metadataLoaded(QString link, QString title, QString cover, QString provider, int total);
    Q_SIGNAL void currentItemChanged(const QModelIndex &index);
    Q_SIGNAL void isLoadingChanged();
    Q_SIGNAL void progressUpdated(QString link, int progressIndex, double progress) const;
    Q_SIGNAL void episodeStarted(QString link, int index, QString episodeTitle) const;
    Q_SIGNAL void episodeCompleted(QString showLink, int episodeNumber) const;
    Q_SIGNAL void localProgressUpdated(QString path, QString folder, double progress) const;
    Q_SIGNAL void localProgressStale(QStringList paths) const;

private:
    QSharedPointer<PlaylistItem> m_root = QSharedPointer<PlaylistItem>::create("root", nullptr, "/");
    QWeakPointer<PlaylistItem> m_currentItem;
    QHash<QString, QWeakPointer<PlaylistItem>> m_byLink;
    ServerListModel m_serverListModel;
    QFileSystemWatcher m_folderWatcher;
    CancelToken m_cancel;

    QFutureWatcher<Source> m_watcher;
    CancelToken m_linkCheck;
    CancelToken m_queueCheck;
    int m_linkChecks = 0;
    QSharedPointer<PlaylistItem> m_pendingItem;
    int m_pendingServerIndex = -1;
    LocalResumeLookup m_localResume;
    std::function<void(const QSharedPointer<PlaylistItem> &)> m_episodeResume;
    QSharedPointer<PlaylistItem> m_loadingItem;
    void setLoadingItem(const QSharedPointer<PlaylistItem> &item);
    void refreshPath(const PlaylistItem *item);
    struct Recovery {
        QString link;
        QString reason;
        double at = -1;
        double fraction = -1;
        int hostMoves = 0;
        bool reextracted = false;
        QSet<QString> triedServers;
        qint64 lastFailureMs = 0;
    };
    Recovery m_recovery;
    bool m_recovering = false;
    quint64 m_openedGeneration = 0;
    QSharedPointer<PlaylistItem> m_replacePending;
    static constexpr int kMaxHostMoves = 3;
    static constexpr qint64 kRecoveryWindowMs = 60'000;
    void recoverStream(const QString &reason, double at, bool fromPlayer = true);
    void reopen(const QSharedPointer<PlaylistItem> &item, Source info);
    void forgetSource(const QString &server);
    void resolveServer(int index, double at, double fraction, bool fresh);
    Source m_openedInfo;
    QString m_remoteAheadLink;
    double m_remoteAheadAt = -1;
    double m_resumeAt = -1;
    double m_resumeFraction = -1;
    void openResolved(Source &info);
    qint64 m_lastProgressSaveMs = 0;
    static constexpr qint64 kProgressSaveIntervalMs = 15'000;
    qint64 m_lastPauseSyncMs = 0;
    static constexpr qint64 kPauseSyncIntervalMs = 5'000;

    static constexpr int kQuickPlaySeason = -1;
    QSharedPointer<PlaylistItem> m_quickPlayPending;
    void dropOtherQuickPlayRows(const QString &keepLink);

    CancelToken       m_appendCancel;
    QFuture<void>     m_appendFuture;
    bool              m_appendPlays = false;

    ProgressSync      m_progressSync;

    CancelToken       m_bgCacheCancel;
    QFuture<void>     m_bgCacheFuture;
    QFutureWatcher<void> m_bgCacheWatcher;
    bool              m_bgCachePending = false;
    void cacheRemainingServers();

    bool m_currentCompleted     = false;
    bool m_mpvProgressConnected = false;
    void ensureMpvProgressConnection();
    void onPlaybackProgress();

    struct Prefetch {
        bool valid = false;
        qint64 resolvedAtMs = 0;
        QString itemLink;
        QList<VideoServer> servers;
        Provider *provider = nullptr;
        QString chosen;
        ServerListModel::SourceCache sources;
        const Source *chosenSource() const {
            const auto it = sources.constFind(chosen);
            return valid && it != sources.constEnd() && it->fresh() ? &it->info : nullptr;
        }
    };
    Prefetch          m_prefetch;
    Prefetch          m_previous;
    QString           m_serversFor;
    CancelToken       m_prefetchCancel;
    QFuture<void>     m_prefetchFuture;
    QTimer            m_prefetchTimer;
    bool              m_prefetchRefreshed = false;
    void prefetchNextEpisode();
    void startNextEpisodePrefetch();
    QSharedPointer<PlaylistItem> nextItem() const;
    bool tryUsePrefetch(const QSharedPointer<PlaylistItem> &item);
    void applyServers(const QString &episodeLink, const QList<VideoServer> &servers, Provider *provider,
                      const QString &chosen, ServerListModel::SourceCache cache);

    void onPlayFinished();
    bool tryPlay(const QSharedPointer<PlaylistItem> &item);
    // Passed in, not read by the worker: a member shows a newer play's token, and item->parent() races the GUI thread.
    Source resolvePlayback(const QSharedPointer<PlaylistItem> &item, const QSharedPointer<PlaylistItem> &playlist,
                             const CancelToken &cancel);
    QSharedPointer<PlaylistItem> resolveToPlayableItem(QSharedPointer<PlaylistItem> item);
    Source loadSource(const QSharedPointer<PlaylistItem> &item, const QSharedPointer<PlaylistItem> &playlist,
                          const CancelToken &cancel);
    Source loadPastedSource(const QSharedPointer<PlaylistItem> &item);
    // "url|Header: value|..." as pasted.
    static Source pastedSource(const QString &link);
    Source loadOnlineSource(const QSharedPointer<PlaylistItem> &item, const QSharedPointer<PlaylistItem> &playlist,
                                const CancelToken &cancel);
    Source loadLocalSource(const QSharedPointer<PlaylistItem> &item);
    static Source localSource(const QString &path);
    DownloadLookup m_downloadLookup;
    KeptLookup m_keptLookup;
    QString m_streamInstead;
    bool playDownloaded(const QSharedPointer<PlaylistItem> &item, const QString &file);
    struct Counted { QString itemLink, link; int row = -1; float number = -1; };
    Counted m_countedAs;
    Counted countedAs(const QSharedPointer<PlaylistItem> &item) const;
    void finalizePlayback(const QSharedPointer<PlaylistItem> &item);
    static double resumePoint(const PlaylistItem &item);
    void setCurrentItem(const QSharedPointer<PlaylistItem> &currentItem);

    QModelIndex indexFor(PlaylistItem *item) const {
        return (!item || item == m_root.data()) ? QModelIndex() : createIndex(item->row(), 0, item);
    }

    void openLocalPath(const QUrl &url, const QString &urlString, bool play);
    void openRemoteUrl(const QString &urlString, const QUrl &url, bool play);
    void addPastedUrl(const QString &urlString, const QUrl &url, bool play);

    Q_SLOT void onLocalDirectoryChanged(const QString &path);
    QSet<QString> m_changedFolders;
    QTimer m_rescanTimer;
    void rescanChangedFolders();
    void rescanFolder(const QString &path);

    void registerPlaylist(const QSharedPointer<PlaylistItem> &playlist);
    void deregisterPlaylist(const QSharedPointer<PlaylistItem> &playlist);
    void applyLocalResume(const QSharedPointer<PlaylistItem> &playlist);

    using PlaylistVisitor = std::function<void(const QSharedPointer<PlaylistItem> &)>;
    void visitListNodes(const QSharedPointer<PlaylistItem> &root, const PlaylistVisitor &visitor);
    ServerListModel *serverList() { return &m_serverListModel; }
};
