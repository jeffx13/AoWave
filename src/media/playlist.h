#pragma once
#include "media/ytdlp.h"
#include <QAbstractItemModel>
#include <QFileSystemWatcher>
#include <QFuture>
#include <QFutureWatcher>
#include <QHash>
#include <QSet>
#include <QSharedPointer>
#include <QTimer>
#include <QWeakPointer>
#include "shows/showdata.h"
#include "media/serverlistmodel.h"
#include "shows/playlistitem.h"
#include "media/progresssync.h"
#include "net/canceltoken.h"
#include <qqmlintegration.h>

class ShowProvider;

class Playlist : public QAbstractItemModel {
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(ServerListModel *serverList READ serverList CONSTANT)
    Q_PROPERTY(bool isLoading READ isLoading NOTIFY isLoadingChanged)
public:
    explicit Playlist(QObject *parent = nullptr);
    ~Playlist();

    QSharedPointer<PlaylistItem> find(const QString &link) const;
    int count() const { return m_root->count(); }

    bool isPlaying(const QString &link) const;
    void rekey(const QString &oldLink, const QSharedPointer<PlaylistItem> &newPlaylist);
    // Keeps whatever is playing. For migration.
    bool relink(const QString &oldLink, const QString &newLink);

    int append(const QSharedPointer<PlaylistItem> &playlist, const QSharedPointer<PlaylistItem> &parent = nullptr);
    int insert(int index, const QSharedPointer<PlaylistItem> &playlist, const QSharedPointer<PlaylistItem> &parent = nullptr);
    int replace(int index, const QSharedPointer<PlaylistItem> &playlist, const QSharedPointer<PlaylistItem> &parent = nullptr);
    Q_INVOKABLE void remove(const QModelIndex &index);
    Q_INVOKABLE void clear();

    // Beside the show it came from, not over it, so a failed fetch leaves the queue alone.
    bool playShowNow(const QSharedPointer<PlaylistItem> &playlist, int episodeIndex = -1);
    Q_INVOKABLE bool playAt(int index);
    Q_INVOKABLE void stepItem(int offset = 1);
    Q_INVOKABLE void stepPlaylist(int offset = 1);
    Q_INVOKABLE void loadIndex(const QModelIndex &index);
    Q_INVOKABLE void reload();
    Q_INVOKABLE void loadServer(int index);
    Q_INVOKABLE void tryNextServer();
    Q_INVOKABLE void showCurrentItemName() const;
    // quiet: the periodic save must not log every tick
    Q_INVOKABLE void saveProgress(bool quiet = false);
    // Marks what is playing as seen without watching the rest.
    Q_INVOKABLE void markCurrentWatched();
    Q_INVOKABLE void cancel();
    // The Stop button: the place is saved, then nothing is current, so nothing says it plays.
    Q_INVOKABLE void stop();
    Q_INVOKABLE void openUrl(QUrl url, bool play);

    void appendShow(const QString &title, const QString &link, ShowProvider *provider,
                    QSharedPointer<PlaylistItem> cached, const ShowData::WatchState &info, bool play);

    enum { TitleRole = Qt::UserRole, IndexRole, NumberRole, IsCurrentIndexRole, IsDeletableRole, LinkRole, IsWatchedRole };
    Q_INVOKABLE QModelIndex currentChild(const QModelIndex &idx) const;
    Q_INVOKABLE QString currentShowName() const;
    Q_INVOKABLE QString currentItemName() const;
    // What stepItem(1) would play: the next episode, else the next playlist. Empty at the end.
    Q_INVOKABLE QString nextItemName() const;
    Q_INVOKABLE int     currentShowEpisodeCount() const;
    // Empty unless the show came from a provider.
    Q_INVOKABLE QString showLinkAt(const QModelIndex &index) const;
    Q_INVOKABLE QString showProviderAt(const QModelIndex &index) const;
    Q_INVOKABLE QString showTitleAt(const QModelIndex &index) const;
    Q_INVOKABLE QString currentShowLink() const;
    Q_INVOKABLE QString currentShowProvider() const;
    // For a subtitle search: {show, season, episode, title, tmdb}; empty off a provider's show.
    Q_INVOKABLE QVariantMap subtitleHints() const;
    Q_INVOKABLE bool isFilteredOut(const QModelIndex &index, const QString &filter) const;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    QModelIndex index(int row, int column, const QModelIndex &parent) const override;
    QModelIndex parent(const QModelIndex &childIndex) const override;
    int columnCount(const QModelIndex &) const override { return 1; }

    // Includes the pending-restart window.
    bool isLoading() const {
        return m_watcher.isRunning() || m_linkChecks > 0 || m_pendingItem || m_pendingServerIndex >= 0;
    }

    // folder -> (file path, progress). Installed by Application.
    using LocalResumeLookup = std::function<QList<QPair<QString, double>>(const QString &)>;
    void setLocalResumeLookup(LocalResumeLookup lookup) { m_localResume = std::move(lookup); }

    void setEpisodeResumeLookup(std::function<void(const QSharedPointer<PlaylistItem> &)> lookup) { m_episodeResume = std::move(lookup); }

    void setSyncHooks(ProgressSync::Hooks hooks) { m_progressSync.setHooks(std::move(hooks)); }
    // The account is further on than the local file.
    Q_SIGNAL void remoteResumeResolved(QString episodeLink, double seconds);
    Q_SIGNAL void metadataLoaded(QString link, QString title, QString cover, QString provider, int total);
    Q_SIGNAL void currentItemChanged(const QModelIndex &index);
    Q_SIGNAL void isLoadingChanged();
    Q_SIGNAL void progressUpdated(QString link, int progressIndex, double progress) const;
    Q_SIGNAL void episodeStarted(QString link, int index, QString episodeTitle) const;
    // Once per episode. The playlist knows nothing about trackers.
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

    QFutureWatcher<PlayInfo> m_watcher;
    // Pasted links being checked before they join the playlist: a newer one to play wins, while
    // every one only queued (a drop of several) is checked.
    CancelToken m_linkCheck;
    CancelToken m_queueCheck;
    int m_linkChecks = 0;
    QSharedPointer<PlaylistItem> m_pendingItem;
    int m_pendingServerIndex = -1;
    LocalResumeLookup m_localResume;
    std::function<void(const QSharedPointer<PlaylistItem> &)> m_episodeResume;
    QSharedPointer<PlaylistItem> m_loadingItem;
    int m_streamRecoveryAttempts = 0;
    double m_recoveryProgress = -1;
    void openResolved(PlayInfo &info);
    qint64 m_lastProgressSaveMs = 0;
    static constexpr qint64 kProgressSaveIntervalMs = 15'000;
    qint64 m_lastPauseSyncMs = 0;
    static constexpr qint64 kPauseSyncIntervalMs = 5'000;
    QSet<QString> m_autoTriedServers;

    // Row marker for shows started from the episode list.
    static constexpr int kQuickPlaySeason = -1;
    QSharedPointer<PlaylistItem> m_quickPlayPending;
    void dropOtherQuickPlayRows(const QString &keepLink);

    CancelToken       m_appendCancel;
    QFuture<void>     m_appendFuture;

    ProgressSync      m_progressSync;

    CancelToken       m_bgCacheCancel;
    QFuture<void>     m_bgCacheFuture;
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
        ShowProvider *provider = nullptr;
        int chosenIndex = -1;
        QHash<QString, PlayInfo> cachedSources;
        PlayInfo playInfo;
    };
    Prefetch          m_prefetch;
    CancelToken       m_prefetchCancel;
    QFuture<void>     m_prefetchFuture;
    QTimer            m_prefetchTimer;
    void prefetchNextEpisode();
    void startNextEpisodePrefetch();
    QSharedPointer<PlaylistItem> nextItem() const;
    bool tryUsePrefetch(const QSharedPointer<PlaylistItem> &item);
    void applyServers(const QList<VideoServer> &servers, ShowProvider *provider,
                           int chosenIndex, QHash<QString, PlayInfo> cache);

    void onPlayFinished();
    bool tryPlay(const QSharedPointer<PlaylistItem> &item);
    // A worker reading the member would see whichever token a newer play installed.
    PlayInfo resolvePlayback(const QSharedPointer<PlaylistItem> &item, const CancelToken &cancel);
    QSharedPointer<PlaylistItem> resolveToPlayableItem(QSharedPointer<PlaylistItem> item);
    PlayInfo loadPlayInfo(const QSharedPointer<PlaylistItem> &item, const CancelToken &cancel);
    PlayInfo loadPastedPlayInfo(const QSharedPointer<PlaylistItem> &item);
    // "url|Header: value|..." as pasted.
    static PlayInfo pastedPlayInfo(const QString &link);
    PlayInfo loadOnlinePlayInfo(const QSharedPointer<PlaylistItem> &item, const CancelToken &cancel);
    PlayInfo loadLocalPlayInfo(const QSharedPointer<PlaylistItem> &item);
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

    void registerPlaylist(const QSharedPointer<PlaylistItem> &playlist);
    void deregisterPlaylist(const QSharedPointer<PlaylistItem> &playlist);
    // For a freshly built local tree.
    void applyLocalResume(const QSharedPointer<PlaylistItem> &playlist);

    using PlaylistVisitor = std::function<void(const QSharedPointer<PlaylistItem> &)>;
    void visitListNodes(const QSharedPointer<PlaylistItem> &root, const PlaylistVisitor &visitor);
    ServerListModel *serverList() { return &m_serverListModel; }
};
