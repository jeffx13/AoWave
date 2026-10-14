#pragma once
#include "app/async.h"
#include <QAbstractListModel>
#include <QFutureWatcher>
#include <QList>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <qqmlintegration.h>

class Client;

namespace SubDl {

struct Result {
    QString fileId;
    QString name;
    QString releaseName;
    QString language;
    QString author;
    QUrl    url;
    int     season = 0;
    int     episode = 0;
    qint64  size = 0;
    bool    hearingImpaired = false;
};

struct Hints {
    QString show;
    QString tmdb;
    QString episodeTitle;
    int     season = 0;
    int     episode = 0;
};

struct Query {
    QString title;
    int     season  = 0;
    int     episode = 0;
};
Query parseQuery(QString text);

QList<Result> forEpisode(QList<Result> results, int season, int episode, const QString &episodeTitle = {});

// Passed in: this runs on a worker, which must not touch QSettings.
QList<Result> search(Client *client, const QString &query,
                     const QString &apiKey, const QString &languages, const Hints &hints = {});

QString fetch(Client *client, const Result &result);

QString cachePath(const QString &fileId);

QVariantList languages();

}

class Client;
class MpvPlayer;

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
        QString       localPath;
        int           slot = 0;
        bool          fetching = false;
    };

    void setResults(const QList<SubDl::Result> &results);
    int  slotFor(const QString &localPath);
    void refreshSlots();
    int  rowForFileId(const QString &fileId) const;
    MpvPlayer *mpv();

    CancelToken                          m_searchCancel;
    CancelToken                          m_fetchCancel;
    QList<Row>                           m_rows;
    QString                              m_query;
    QString                              m_searchedQuery;
    QFutureWatcher<QList<SubDl::Result>> m_searchWatcher;
    QFutureWatcher<void>                 m_fetchWatcher;
    bool                                 m_mpvConnected = false;
};
