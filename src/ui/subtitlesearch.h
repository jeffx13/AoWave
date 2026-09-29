#pragma once
#include <QAbstractListModel>
#include <QFutureWatcher>
#include <qqmlintegration.h>
#include "net/canceltoken.h"
#include <QUrl>

class Client;
class MpvPlayer;

namespace SubDl {

// SubDL wraps files in a release even when there is only one.
struct Result {
    QString fileId;        // stable per file; doubles as the cache key
    QString name;
    QString releaseName;
    QString language;
    QString author;
    QUrl    url;
    int     season = 0;
    int     episode = 0;
    qint64  size = 0;      // Used to spot a half-written cache entry.
    bool    hearingImpaired = false;
};

// What the player knows of the episode: the show's TMDB id searches exactly where a title is
// a fuzzy match, and the episode's title ranks the files that carry it.
struct Hints {
    QString show;
    QString tmdb;           // "tv/<id>" or "movie/<id>"
    QString episodeTitle;
    int     season = 0;
    int     episode = 0;
};

struct Query {
    QString title;
    int     season  = 0;
    int     episode = 0;
};
// "Show s01e05", "Show 1x05", "Show season 1 episode 5", "Show s01", "Show e05".
Query parseQuery(QString text);

// The files for one episode, best first: its number, said in the name too, its title in the
// name, then its season. Files for other episodes go; files that say no episode come last.
QList<Result> forEpisode(QList<Result> results, int season, int episode, const QString &episodeTitle = {});

// Passed in: this runs on a worker, which must not touch QSettings.
QList<Result> search(Client *client, const QString &query,
                     const QString &apiKey, const QString &languages, const Hints &hints = {});

QString fetch(Client *client, const Result &result);

QString cachePath(const QString &fileId);

// {code, name} pairs
QVariantList languages();

}

class SubtitleSearch : public QAbstractListModel
{
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(bool    isLoading READ isLoading NOTIFY isLoadingChanged)
    Q_PROPERTY(int     count     READ count     NOTIFY countChanged)
    Q_PROPERTY(QString query     READ query     NOTIFY queryChanged)
    Q_PROPERTY(QVariantList languages READ languages CONSTANT)
public:
    enum { DisplayNameRole = Qt::UserRole, ReleaseRole, LanguageRole, AuthorRole,
           EpisodeRole, HiRole, TagsRole, SlotRole, FetchingRole };

    explicit SubtitleSearch(QObject *parent = nullptr);
    ~SubtitleSearch();

    // `hints`: Playlist::subtitleHints(), used while the query still names that show.
    Q_INVOKABLE void search(const QString &query, const QVariantMap &hints = {});
    Q_INVOKABLE void searchIfNew(const QString &query, const QVariantMap &hints = {});
    Q_INVOKABLE void use(int row, bool secondary = false);
    Q_INVOKABLE void cancel();

    int  count() const { return m_rows.size(); }
    bool isLoading() const { return m_searchWatcher.isRunning(); }
    QString query() const { return m_query; }
    QVariantList languages() const { return SubDl::languages(); }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_SIGNAL void isLoadingChanged();
    Q_SIGNAL void countChanged();
    Q_SIGNAL void queryChanged();

private:
    struct Row {
        SubDl::Result result;
        QString       displayName;
        QStringList   tags;
        QString       localPath;      // set once downloaded
        int           slot = 0;       // 0 none, 1 primary, 2 secondary
        bool          fetching = false;
    };

    void setResults(const QList<SubDl::Result> &results);
    int  slotFor(const QString &localPath);
    void refreshSlots();
    int  rowForFileId(const QString &fileId) const;
    // MpvPlayer is built by QML, so it does not exist yet.
    MpvPlayer *mpv();

    CancelToken                          m_searchCancel;
    CancelToken                          m_fetchCancel;
    QList<Row>                           m_rows;
    QString                              m_query;
    QString                              m_searchedQuery;   // last query that finished
    QFutureWatcher<QList<SubDl::Result>> m_searchWatcher;
    QFutureWatcher<void>                 m_fetchWatcher;
    bool                                 m_mpvConnected = false;
};
