#pragma once
#include "app/async.h"
#include "providers/showdata.h"
#include "providers/provider.h"
#include <QObject>
#include <functional>
#include <QFutureWatcher>
#include <qqmlintegration.h>
#include <QAbstractListModel>
#include <QSharedPointer>
#include <QWeakPointer>
#include <QVariant>
#include <QVector>

class EpisodeListModel : public QAbstractListModel {
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(bool    reversed   READ isReversed  WRITE setReversed NOTIFY reversedChanged)
    Q_PROPERTY(QString filterText READ filterText  WRITE setFilterText  NOTIFY filterTextChanged)
    Q_PROPERTY(int     season     READ season      WRITE setSeason      NOTIFY seasonChanged)
    Q_PROPERTY(QVariantList seasons READ seasons NOTIFY seasonsChanged)
    Q_PROPERTY(int seasonFirst READ seasonFirst NOTIFY rangeChanged)
    Q_PROPERTY(int seasonLast  READ seasonLast  NOTIFY rangeChanged)

public:
    explicit EpisodeListModel(QObject *parent = nullptr) : QAbstractListModel(parent) {}

    void setPlaylist(const QSharedPointer<PlaylistItem> &playlist);
    bool isReversed() const { return m_isReversed; }
    void setReversed(bool isReversed);
    QString filterText() const { return m_filterText; }
    void setFilterText(const QString &text);
    int season() const { return m_season; }
    void setSeason(int season);
    QVariantList seasons() const { return m_seasons; }
    int seasonFirst() const;
    int seasonLast() const;

    Q_INVOKABLE int sourceIndex(int visibleRow) const;
    Q_INVOKABLE int visibleIndex(int sourceIdx) const;
    Q_INVOKABLE void refreshProgress();

signals:
    void reversedChanged();
    void filterTextChanged();
    void seasonChanged();
    void seasonsChanged();
    void rangeChanged();

private:
    QWeakPointer<PlaylistItem> m_playlist;
    bool    m_isReversed = false;
    QString m_filterText;
    int     m_season = 0;
    QVariantList m_seasons;
    QVector<int> m_filteredIndices;

    bool filtering() const { return !m_filterText.isEmpty() || m_season != 0; }
    int  visibleCount() const;
    void rebuildFilteredIndices();

    enum {
        TitleRole = Qt::UserRole,
        EpisodeNumberRole,
        SeasonNumberRole,
        ProgressRole,
        ThumbnailRole,
        AiredAtRole,
        PreviewRole,
        AvailableAtRole,
    };
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
};

class CurrentShow : public QObject {
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(QString      title        READ title        NOTIFY showChanged)
    Q_PROPERTY(QString      coverUrl     READ coverUrl     NOTIFY showChanged)
    Q_PROPERTY(QString      detailCover  READ detailCover  NOTIFY showChanged)
    Q_PROPERTY(double       coverAspect  READ coverAspect  NOTIFY showChanged)
    Q_PROPERTY(QString      description  READ description  NOTIFY showChanged)
    Q_PROPERTY(QString      releaseDate  READ releaseDate  NOTIFY showChanged)
    Q_PROPERTY(QString      status       READ status       NOTIFY showChanged)
    Q_PROPERTY(QString      link         READ link         NOTIFY showChanged)
    Q_PROPERTY(QString      updateTime   READ updateTime   NOTIFY showChanged)
    Q_PROPERTY(QString      rating       READ rating       NOTIFY showChanged)
    Q_PROPERTY(QString      views        READ views        NOTIFY showChanged)
    Q_PROPERTY(QString      genresString READ genresString NOTIFY showChanged)
    Q_PROPERTY(bool         exists       READ exists       NOTIFY showChanged)
    Q_PROPERTY(Provider *provider    READ provider     NOTIFY showChanged)
    Q_PROPERTY(QDateTime    nextEpisodeAt READ nextEpisodeAt NOTIFY showChanged)

    Q_PROPERTY(bool              isLoading        READ isLoading        NOTIFY isLoadingChanged)
    Q_PROPERTY(EpisodeListModel *episodes         READ episodes         CONSTANT)
    Q_PROPERTY(QString           continueText     READ continueText     NOTIFY lastWatchedIndexChanged)
    Q_PROPERTY(int               lastWatchedIndex READ lastWatchedIndex WRITE setLastWatchedIndex NOTIFY lastWatchedIndexChanged)
    Q_PROPERTY(int               continueIndex    READ continueIndex    NOTIFY lastWatchedIndexChanged)
    Q_PROPERTY(int               episodeCount     READ episodeCount     NOTIFY lastWatchedIndexChanged)
    Q_PROPERTY(int               watchedCount     READ watchedCount     NOTIFY lastWatchedIndexChanged)

public:
    explicit CurrentShow(QObject *parent = nullptr);
    ~CurrentShow();

    QString       title()        const { return m_show.title; }
    QString       coverUrl()     const { return m_show.coverUrl; }
    QString       detailCover()  const { return m_show.provider ? m_show.provider->detailCover(m_show.coverUrl) : m_show.coverUrl; }
    double        coverAspect()  const { return m_show.coverAspect; }
    QString       description()  const { return m_show.description; }
    QString       releaseDate()  const { return m_show.releaseDate; }
    QString       updateTime()   const { return m_show.updateTime; }
    QString       rating()       const { return m_show.score; }
    QString       genresString() const { return m_show.genres.join(','); }
    QString       views()        const { return m_show.views; }
    QString       status()       const { return m_show.status; }
    QString       link()         const { return m_show.link; }
    Provider *provider()     const { return m_show.provider; }
    bool          exists()       const { return !m_show.link.isEmpty(); }
    QDateTime     nextEpisodeAt() const { return m_show.nextEpisodeAt; }

    const ShowData &show() const { return m_show; }
    QSharedPointer<PlaylistItem> playlist() const { return m_show.playlist(); }

    void setShow(const ShowData &show, const ShowData::WatchState &watchState, bool navigate = true);
    // How a fresh load joins a list the player already holds. GUI thread; installed by Application.
    using EpisodeMerger = std::function<void(const QSharedPointer<PlaylistItem> &list,
                                             const QSharedPointer<PlaylistItem> &fresh)>;
    void setEpisodeMerger(EpisodeMerger merge) { m_merge = std::move(merge); }
    void reload(const ShowData &show, const ShowData::WatchState &watchState);

    QString continueText()     const { return m_continueText; }
    int     continueIndex()    const { return m_continueIndex; }
    int     lastWatchedIndex() const;
    int     episodeCount() const;
    int     watchedCount() const;
    void    setLastWatchedIndex(int index);
    void    onPlaybackIndexChanged() { updateContinueEpisode(); emit lastWatchedIndexChanged(); }

    Q_INVOKABLE void markWatched(int index);
    Q_INVOKABLE void markUnwatched(int index);
    Q_SIGNAL void episodeMarked(QString link, int index, double progress);
    Q_INVOKABLE void cancel();
    bool isLoading() const { return m_watcher.isRunning(); }

    Q_SIGNAL void showChanged();
    Q_SIGNAL void lastWatchedIndexChanged();
    Q_SIGNAL void isLoadingChanged();

private:
    EpisodeListModel *episodes() { return &m_episodes; }
    void updateContinueEpisode();
    void load(ShowData show, ShowData::WatchState watchState, bool navigate, CancelToken cancel);
    void onLoadFinished();

    ShowData                 m_show;
    EpisodeListModel         m_episodes;
    EpisodeMerger            m_merge;
    QFutureWatcher<void>     m_watcher;
    CancelToken              m_cancel;
    int                      m_continueIndex = -1;
    QString                  m_continueText;

    ShowData                 m_pendingShow;
    ShowData::WatchState  m_pendingInfo;
    bool                     m_pendingNavigate = true;
    bool                     m_hasPending = false;
};
