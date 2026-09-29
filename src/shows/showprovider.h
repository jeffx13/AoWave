#pragma once
#include "net/client.h"
#include "shows/showdata.h"
#include "shows/playlistitem.h"
#include "shows/playinfo.h"
#include <QMutex>
#include <QVariantMap>

// Every method runs on a worker thread, with the caller's Client.
class ShowProvider : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString name READ name CONSTANT)
    Q_PROPERTY(QString hostUrl READ hostUrl CONSTANT)
    Q_PROPERTY(QString language READ language CONSTANT)
public:
    explicit ShowProvider(QObject *parent = nullptr) : QObject(parent) {}

    virtual QString name() const = 0;
    virtual QString hostUrl() const = 0;
    virtual QString language() const { return QStringLiteral("en"); }
    virtual QStringList availableTypes() const = 0;

    [[nodiscard]] virtual QList<ShowData>    search       (Client *client, const QString &query, int page, int typeIndex) = 0;
    [[nodiscard]] virtual QList<ShowData>    popular      (Client *client, int page, int typeIndex) = 0;
    [[nodiscard]] virtual QList<ShowData>    latest       (Client *client, int page, int typeIndex) = 0;
    [[nodiscard]] virtual QList<VideoServer> loadServers  (Client *client, const PlaylistItem *episode) const = 0;
    // By value: each concurrent caller mutates its own copy.
    [[nodiscard]] virtual PlayInfo           extractSource(Client *client, VideoServer server) = 0;

    // What a browse can be narrowed by: for each of "genre", "year" and "status" the provider
    // takes, a list of {value, label}. Status values are "airing", "finished" and "upcoming",
    // named by the app. Empty when the provider takes none.
    [[nodiscard]] virtual QVariantMap filterOptions(Client * /*client*/, int /*typeIndex*/) { return {}; }
    // Whether the filters narrow a search for words too, not only Latest and Popular.
    virtual bool filtersSearch() const { return false; }
    // Latest or Popular (or with filtersSearch() a search) narrowed by filter values, {"genre": value, ...}.
    [[nodiscard]] virtual QList<ShowData> filtered(Client * /*client*/, const QString & /*query*/, int /*page*/,
                                                   int /*typeIndex*/, const QVariantMap & /*filters*/,
                                                   bool /*latest*/) { return {}; }
    static QVariantMap filterOption(const QString &value, const QString &label) {
        return {{QStringLiteral("value"), value}, {QStringLiteral("label"), label}};
    }
    static QVariantList statusOptions(const QStringList &values) {
        QVariantList options;
        for (const QString &value : values) options.append(QVariantMap{{QStringLiteral("value"), value}});
        return options;
    }

    // "tv/<id>" or "movie/<id>" where the show is a TMDB title, for services keyed on TMDB ids.
    virtual QString tmdbRef(const QString & /*showLink*/) const { return {}; }

    // The show link, and the episode to start at (-1 leaves that to the load).
    virtual bool parseUrl(const QUrl &, QString &, int &) const { return false; }
    // The show's page on the site; empty where a link alone can't say (a slug the link lacks).
    virtual QString showUrl(const QString & /*showLink*/) const { return {}; }

    // Whether the service keeps a watch position of its own. Without it, progress is only local,
    // and there is nothing to push, queue or reconcile.
    virtual bool syncsProgress() const { return false; }

    // False for a failure that cannot happen - no credentials, or the service refused.
    virtual bool reportProgress(Client *, const QString & /*episodeLink*/, double /*seconds*/,
                                double /*duration*/) { return false; }

    // `serverAt` is the service's own timestamp, never the local clock.
    struct RemoteProgress {
        double seconds = 0;
        qint64 serverAt = 0;    // unix seconds, as the service reported it
        bool   valid = false;
    };
    virtual RemoteProgress fetchProgress(Client *, const QString & /*episodeLink*/) { return {}; }

    // From response Date headers. Never compare a service timestamp to local wall time without it.
    virtual qint64 clockSkew() const { return 0; }

    // True when a Details load sets show.nextEpisodeAt and returns the episode count.
    virtual bool publishesAiringTimes() const { return false; }

    // Never combined: implementations return as soon as the count is known.
    enum LoadPart {
        CountOnly = 0x1,
        Episodes  = 0x2,   // fill the show's playlist
        Details   = 0x4,   // description, genres, status, cover, ...
    };
    Q_DECLARE_FLAGS(LoadParts, LoadPart)

    int loadShow(Client *client, ShowData &show) const {
        LoadParts parts = Details;
        if (!show.playlist()) parts |= Episodes;
        return loadShow(client, show, parts);
    }
    int fetchEpisodeCount(Client *client, ShowData &show) const { return loadShow(client, show, CountOnly); }
    // One request more, where the provider publishes airing times.
    int fetchEpisodeCountAndAiring(Client *client, ShowData &show) const {
        return loadShow(client, show, publishesAiringTimes() ? Details : CountOnly);
    }
    void loadPlaylist(Client *client, ShowData &show) const { loadShow(client, show, Episodes); }

    void setPreferredServer(const QString &serverName) {
        QMutexLocker lock(&m_preferredServerMutex);
        m_preferredServer = serverName;
    }
    QString preferredServer() const {
        QMutexLocker lock(&m_preferredServerMutex);
        return m_preferredServer;
    }

protected:
    virtual int loadShow(Client *client, ShowData &show, LoadParts parts) const = 0;

private:
    QString m_preferredServer;
    mutable QMutex m_preferredServerMutex;
};

Q_DECLARE_OPERATORS_FOR_FLAGS(ShowProvider::LoadParts)
