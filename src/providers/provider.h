#pragma once
#include "app/settings.h"
#include "net/client.h"
#include "providers/showdata.h"
#include <QDate>
#include <QDateTime>
#include <QMutex>
#include <QVariantMap>
#include <qqmlintegration.h>

class Account : public QObject {
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(QString label           READ label           NOTIFY changed)
    Q_PROPERTY(QString hint            READ hint            NOTIFY changed)
    Q_PROPERTY(QString offlineHint     READ offlineHint     NOTIFY changed)
    Q_PROPERTY(QString pastePrompt     READ pastePrompt     NOTIFY changed)
    Q_PROPERTY(bool    syncsProgress   READ syncsProgress   CONSTANT)
    Q_PROPERTY(bool    signedIn        READ signedIn        NOTIFY changed)
    Q_PROPERTY(QString name            READ name            NOTIFY changed)
    Q_PROPERTY(QString membership      READ membership      NOTIFY changed)
    Q_PROPERTY(QString tone            READ tone            NOTIFY changed)
    Q_PROPERTY(QString expiry          READ expiry          NOTIFY changed)
    Q_PROPERTY(QString checked         READ checked         NOTIFY changed)
    Q_PROPERTY(QString problem         READ problem         NOTIFY changed)
    Q_PROPERTY(QString caution         READ caution         NOTIFY changed)
    Q_PROPERTY(QVariantList details    READ details         NOTIFY changed)
    Q_PROPERTY(bool    busy            READ busy            NOTIFY changed)
    Q_PROPERTY(QString loginQr         READ loginQr         NOTIFY loginChanged)
    Q_PROPERTY(QString loginStatus     READ loginStatus     NOTIFY loginChanged)
public:
    using Fields = QMap<QString, QString>;
    struct Standing {
        bool signedIn = false;
        QString name;
        bool asked = false;
        bool member = false;
        QString plan;
        QDate until;
        bool lifetime = false;
        QDateTime sessionEnds;
        QDateTime checkedAt;
        QString problem;
        QString caution;               // something to know about an account that works (the site refuses what it still accepts)
        QList<QPair<QString, QString>> details;
    };

    Account(const Config::Section &store, QObject *parent = nullptr);

    virtual QString label() const = 0;
    virtual QString hint() const { return {}; }
    virtual QString offlineHint() const { return {}; }
    // Names a pasted session has to carry; none: the site's session is not something to paste.
    virtual QStringList required() const { return {}; }
    virtual bool syncsProgress() const { return false; }
    QString pastePrompt() const;

    const Standing &standing() const { return m_standing; }
    bool signedIn() const { return m_standing.signedIn; }
    QString name() const { return m_standing.name; }
    QString problem() const { return m_standing.problem; }
    QString caution() const { return m_standing.caution; }
    QVariantList details() const;
    bool busy() const { return m_busy; }
    QString membership() const;
    QString tone() const;
    static QString plain(double number);
    QString expiry() const;
    QString checked() const;
    QString loginQr() const { return m_loginQr; }
    QString loginStatus() const { return m_loginStatus; }

    // The session as stored, on any thread.
    Fields fields() const;

    Q_INVOKABLE virtual void signIn() {}
    Q_INVOKABLE virtual void copyLoginLink() const {}
    Q_INVOKABLE virtual void cancelLogin() {}
    Q_INVOKABLE bool signInWithText(const QString &pasted);
    Q_INVOKABLE virtual void refresh() = 0;
    Q_INVOKABLE virtual void signOut();

    static Fields parse(const QString &text);

signals:
    void changed();
    void loginChanged();
    void sessionChanged();

protected:
    // Stores the session (GUI thread).
    void keep(const Fields &fields);
    virtual void adopted() {}
    void setStanding(const Standing &standing);
    void setBusy(bool busy);
    void setLogin(const QString &status, const QString &qr = {});

private:
    void remember(const Fields &fields);

    const char *m_store;
    mutable QMutex m_fieldsMutex;
    Fields m_fields;
    Standing m_standing;
    bool m_busy = false;
    QString m_loginQr, m_loginStatus;
};

class FilterBrowse {
public:
    virtual ~FilterBrowse() = default;
    // Status values are "airing", "finished" and "upcoming".
    [[nodiscard]] virtual QVariantMap filterOptions(Client *client, int typeIndex) = 0;
    [[nodiscard]] virtual QList<ShowData> filtered(Client *client, const QString &query, int page, int typeIndex,
                                                   const QVariantMap &filters, bool latest) = 0;
    virtual bool filtersSearch() const { return false; }

    static QVariantMap filterOption(const QString &value, const QString &label) {
        return {{QStringLiteral("value"), value}, {QStringLiteral("label"), label}};
    }
    static QVariantList statusOptions(const QStringList &values) {
        QVariantList options;
        for (const QString &value : values) options.append(QVariantMap{{QStringLiteral("value"), value}});
        return options;
    }
};

class WatchHistory {
public:
    virtual ~WatchHistory() = default;
    virtual bool reportProgress(Client *client, const QString &episodeLink, double seconds, double duration) = 0;
    // `serverAt` is the service's own timestamp, never the local clock.
    struct RemoteProgress {
        double seconds = 0;
        qint64 serverAt = 0;
        bool   valid = false;
    };
    virtual RemoteProgress fetchProgress(Client *client, const QString &episodeLink) = 0;
    virtual qint64 clockSkew() const { return 0; }
};

class DanmakuSource {
public:
    virtual ~DanmakuSource() = default;
    // The comments `ids` name (what the provider put there when it extracted the episode), on a worker with a
    // Client of its own: this is called minutes later for a prefetch, so nothing of the extraction's is used.
    [[nodiscard]] virtual QList<DanmakuComment> comments(Client *client, const QVariantMap &ids) = 0;
};

// Every method runs on a worker thread, with the caller's Client.
class Provider : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString name READ name CONSTANT)
    Q_PROPERTY(QString hostUrl READ hostUrl CONSTANT)
    Q_PROPERTY(QString language READ language CONSTANT)
public:
    explicit Provider(QObject *parent = nullptr) : QObject(parent) {}

    virtual QString name() const = 0;
    virtual QString hostUrl() const = 0;
    virtual QString language() const { return QStringLiteral("en"); }
    virtual QStringList availableTypes() const = 0;

    [[nodiscard]] virtual QList<ShowData>    search       (Client *client, const QString &query, int page, int typeIndex) = 0;
    [[nodiscard]] virtual QList<ShowData>    popular      (Client *client, int page, int typeIndex) = 0;
    [[nodiscard]] virtual QList<ShowData>    latest       (Client *client, int page, int typeIndex) = 0;
    [[nodiscard]] virtual QList<VideoServer> loadServers  (Client *client, const PlaylistItem *episode) const = 0;
    // By value: each concurrent caller mutates its own copy.
    [[nodiscard]] virtual Source           extractSource(Client *client, VideoServer server) = 0;

    // The optional parts (filters, account, watchHistory, danmaku) are owned by the provider; null when the site has none.
    virtual FilterBrowse *filters() { return nullptr; }

    virtual QString tmdbRef(const QString & ) const { return {}; }

    virtual bool parseUrl(const QUrl &, QString &, int &) const { return false; }
    virtual QString showUrl(const QString & ) const { return {}; }
    virtual bool playsDirectly(const QString & ) const { return false; }

    virtual Account *account() { return nullptr; }

    virtual WatchHistory *watchHistory() { return nullptr; }

    virtual DanmakuSource *danmaku() { return nullptr; }

    virtual bool publishesAiringTimes() const { return false; }

    // The cover for the Details page, where a site can serve a larger one than the list cards ask for.
    virtual QString detailCover(const QString &cover) const { return cover; }

    // A cover as the list pages show it: one stored before a site's cover addresses were fixed may be gone from the server.
    virtual QString listCover(const QString &stored) const { return stored; }

    enum LoadPart {
        CountOnly = 0x1,
        Episodes  = 0x2,
        Details   = 0x4,
    };
    Q_DECLARE_FLAGS(LoadParts, LoadPart)

    void offerDanmaku(Source &info, const QString &key, const QVariantMap &ids);

    int loadShow(Client *client, ShowData &show) const {
        LoadParts parts = Details;
        if (!show.playlist()) parts |= Episodes;
        return loadShow(client, show, parts);
    }
    int fetchEpisodeCount(Client *client, ShowData &show) const { return loadShow(client, show, CountOnly); }
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

Q_DECLARE_OPERATORS_FOR_FLAGS(Provider::LoadParts)
