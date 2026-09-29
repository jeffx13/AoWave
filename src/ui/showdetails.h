#pragma once
#include "net/canceltoken.h"
#include "shows/showdata.h"
#include "shows/showprovider.h"  // Full type needed for Q_PROPERTY(ShowProvider*).
#include "ui/episodelistmodel.h"

#include <QObject>
#include <QFutureWatcher>
#include <qqmlintegration.h>

class ShowDetails : public QObject {
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(QString      title        READ title        NOTIFY showChanged)
    Q_PROPERTY(QString      coverUrl     READ coverUrl     NOTIFY showChanged)
    Q_PROPERTY(QString      description  READ description  NOTIFY showChanged)
    Q_PROPERTY(QString      releaseDate  READ releaseDate  NOTIFY showChanged)
    Q_PROPERTY(QString      status       READ status       NOTIFY showChanged)
    Q_PROPERTY(QString      link         READ link         NOTIFY showChanged)
    Q_PROPERTY(QString      updateTime   READ updateTime   NOTIFY showChanged)
    Q_PROPERTY(QString      rating       READ rating       NOTIFY showChanged)
    Q_PROPERTY(QString      views        READ views        NOTIFY showChanged)
    Q_PROPERTY(QString      genresString READ genresString NOTIFY showChanged)
    Q_PROPERTY(bool         exists       READ exists       NOTIFY showChanged)
    Q_PROPERTY(ShowProvider *provider    READ provider     NOTIFY showChanged)
    Q_PROPERTY(QDateTime    nextEpisodeAt READ nextEpisodeAt NOTIFY showChanged)

    Q_PROPERTY(bool              isLoading        READ isLoading        NOTIFY isLoadingChanged)
    Q_PROPERTY(EpisodeListModel *episodes         READ episodes         CONSTANT)
    Q_PROPERTY(QString           continueText     READ continueText     NOTIFY lastWatchedIndexChanged)
    Q_PROPERTY(int               lastWatchedIndex READ lastWatchedIndex WRITE setLastWatchedIndex NOTIFY lastWatchedIndexChanged)
    Q_PROPERTY(int               continueIndex    READ continueIndex    NOTIFY lastWatchedIndexChanged)
    Q_PROPERTY(int               episodeCount     READ episodeCount     NOTIFY lastWatchedIndexChanged)
    Q_PROPERTY(int               watchedCount     READ watchedCount     NOTIFY lastWatchedIndexChanged)

public:
    explicit ShowDetails(QObject *parent = nullptr);
    ~ShowDetails();

    QString       title()        const { return m_show.title; }
    QString       coverUrl()     const { return m_show.coverUrl; }
    QString       description()  const { return m_show.description; }
    QString       releaseDate()  const { return m_show.releaseDate; }
    QString       updateTime()   const { return m_show.updateTime; }
    QString       rating()       const { return m_show.score; }
    QString       genresString() const { return m_show.genres.join(','); }
    QString       views()        const { return m_show.views; }
    QString       status()       const { return m_show.status; }
    QString       link()         const { return m_show.link; }
    ShowProvider *provider()     const { return m_show.provider; }
    bool          exists()       const { return !m_show.link.isEmpty(); }
    QDateTime     nextEpisodeAt() const { return m_show.nextEpisodeAt; }

    const ShowData &show() const { return m_show; }
    QSharedPointer<PlaylistItem> playlist() const { return m_show.playlist(); }

    void setShow(const ShowData &show, const ShowData::WatchState &watchState, bool navigate = true);
    // Re-fetch a show that is already loaded.
    void reload(const ShowData &show, const ShowData::WatchState &watchState);

    QString continueText()     const { return m_continueText; }
    int     continueIndex()    const { return m_continueIndex; }
    int     lastWatchedIndex() const;
    // Trailers excluded. Watched means before the continue point or past the watched mark.
    int     episodeCount() const;
    int     watchedCount() const;
    void    setLastWatchedIndex(int index);
    void    onPlaybackIndexChanged() { updateContinueEpisode(); emit lastWatchedIndexChanged(); }

    Q_INVOKABLE void markWatched(int index);
    // Back to not started, and the show continues from it.
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
    QFutureWatcher<void>     m_watcher;
    CancelToken              m_cancel;
    int                      m_continueIndex = -1;
    QString                  m_continueText;

    ShowData                 m_pendingShow;
    ShowData::WatchState  m_pendingInfo;
    bool                     m_pendingNavigate = true;
    bool                     m_hasPending = false;
};
