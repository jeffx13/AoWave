#pragma once
#include "app/async.h"
#include "net/client.h"
#include <QDate>
#include <QDateTime>
#include <QObject>
#include <QVariantMap>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QList>
#include <QMutex>
#include <QtConcurrent/QtConcurrentRun>
#include <QTimer>
#include <QAbstractListModel>
#include <QHash>
#include <functional>
#include <qqmlintegration.h>

// Everything but authenticate() and signOut() runs on a worker with the caller's Client; credentials are read under a lock.
class Tracker : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString name READ name CONSTANT)
    Q_PROPERTY(bool authenticated READ isAuthenticated NOTIFY authChanged)
    Q_PROPERTY(QString accountName READ accountName NOTIFY authChanged)
public:
    using QObject::QObject;

    enum class ScoreFormat { Point10, Point100, Point5, Point3, Point10Decimal };
    Q_ENUM(ScoreFormat)

    struct Result {
        QString remoteId;
        QString title;
        QString cover;
        int     episodes = 0;
        QString year;
    };

    struct Entry {
        QString remoteId;
        bool    valid = false;
        bool    onList = false;
        QString title;
        QString siteUrl;
        int     total = 0;
        QString status;
        double  score = 0;
        int     progress = 0;
        int     remoteProgress = 0;
        int     repeat = 0;
        QString started, finished;
        QString notes;
        bool    favourite = false;
        qint64  updatedAt = 0;
    };

    static bool advance(Entry &entry, int episode, const QStringList &vocabulary, QDate today);

    virtual QString name() const = 0;
    bool isAuthenticated() const { return !accessToken().isEmpty(); }
    QString accountName() const { return m_accountName; }

    QString clientId() const;
    void setClientId(const QString &id);
    virtual bool needsClientSecret() const { return false; }
    QString clientSecret() const;
    void setClientSecret(const QString &secret);
    bool canAuthenticate() const {
        return !clientId().isEmpty() && (!needsClientSecret() || !clientSecret().isEmpty());
    }
    virtual QString registrationUrl() const = 0;

    virtual QString registrationHint() const = 0;

    virtual void authenticate() = 0;
    void signOut();

    virtual QList<Result> search(Client *client, const QString &query) = 0;
    virtual Entry entryFor(Client *client, const QString &remoteId) = 0;
    virtual QString update(Client *client, const Entry &entry) = 0;

    virtual QStringList statusVocabulary() const = 0;
    virtual QStringList statusLabels() const;
    virtual QVariantMap features() const = 0;
    virtual ScoreFormat scoreFormat() const { return ScoreFormat::Point10; }

    void shutdown();

signals:
    void authChanged();

protected:
    struct Grant {
        QString token;
        QString refresh;
        qint64  expiresAt = 0;
    };

    void storeToken(const Grant &grant);
    void loadToken();
    void setAccountName(const QString &name);
    void lookUpAccount(std::function<QString(Client &)> ask);
    QString settingsGroup() const;

    QString accessToken() const;
    // Serialised: a refresh token is single-use, so two racing workers would sign the account out.
    QString freshToken(Client *client);
    virtual Grant refreshGrant(Client *, const QString &) { return {}; }

    template <typename F>
    void runInBackground(F &&task) { m_runs.add(QtConcurrent::run(std::forward<F>(task), m_cancel)); }

private:
    mutable QMutex m_authMutex;
    QMutex         m_refreshMutex;
    QString        m_token;
    QString        m_refreshToken;
    qint64         m_expiresAt = 0;
    QString        m_clientId;
    QString        m_clientSecret;
    QString        m_accountName;
    CancelToken    m_cancel;
    RunSet         m_runs;
};

class LoopbackAuth : public QObject {
    Q_OBJECT
public:
    explicit LoopbackAuth(QObject *parent = nullptr);

    static LoopbackAuth *start(QObject *parent, QPointer<LoopbackAuth> &pending, int port,
                               const QString &service);

    int listen(int port = 0, int timeoutMs = 300000);

    // Goes in the authorisation URL; a redirect without it is ignored, so a page that finds the port cannot sign the user in as someone else.
    QString state() const { return m_state; }

signals:
    void codeReceived(const QString &code);
    void tokenReceived(const QString &token, qint64 expiresIn);
    void failed(const QString &reason);

private:
    void serve(class QTcpSocket *socket, const QByteArray &contentType, const QByteArray &body);
    bool m_servedBridge = false;
    QString m_state;
};

class AniList : public Tracker {
    Q_OBJECT
public:
    explicit AniList(QObject *parent = nullptr);
    QString name() const override { return QStringLiteral("AniList"); }

    void authenticate() override;
    QList<Result> search(Client *client, const QString &query) override;
    Entry entryFor(Client *client, const QString &remoteId) override;
    QString update(Client *client, const Entry &entry) override;

    QStringList statusVocabulary() const override {
        return {"CURRENT", "PLANNING", "COMPLETED", "DROPPED", "PAUSED", "REPEATING"};
    }
    QStringList statusLabels() const override {
        return {tr("Watching"), tr("Planned"), tr("Completed"), tr("Dropped"), tr("Paused"), tr("Rewatching")};
    }
    QVariantMap features() const override {
        return {{"accent", "#02A9FF"}, {"rewatches", true}, {"dates", true}, {"notes", true}, {"favourite", true}};
    }
    ScoreFormat scoreFormat() const override { return m_scoreFormat; }

    bool needsClientSecret() const override { return false; }
    QString registrationUrl() const override {
        return QStringLiteral("https://anilist.co/settings/developer");
    }
    QString registrationHint() const override {
        return tr("On AniList, create a client with the redirect URL http://127.0.0.1:%1 (no trailing slash), then "
                  "paste its Client ID here. The Client Secret is optional.")
            .arg(kRedirectPort);
    }
    static constexpr int kRedirectPort = 43217;

private:
    QPointer<LoopbackAuth> m_pendingAuth;

    QJsonObject graphql(Client *client, const QString &query, const QJsonObject &variables,
                        QString *error = nullptr);
    void exchangeCode(const QString &code);
    void openAuthorization();
    void fetchViewer();

    ScoreFormat m_scoreFormat = ScoreFormat::Point10;
};

class Mal : public Tracker {
    Q_OBJECT
public:
    explicit Mal(QObject *parent = nullptr);
    QString name() const override { return QStringLiteral("MyAnimeList"); }

    void authenticate() override;
    QList<Result> search(Client *client, const QString &query) override;
    Entry entryFor(Client *client, const QString &remoteId) override;
    QString update(Client *client, const Entry &entry) override;

    QString registrationUrl() const override {
        return QStringLiteral("https://myanimelist.net/apiconfig");
    }
    QString registrationHint() const override {
        return tr("On MyAnimeList, create an API app with the App Redirect URL http://127.0.0.1:%1, then paste its "
                  "Client ID here.")
            .arg(kRedirectPort);
    }
    QStringList statusVocabulary() const override {
        return {"watching", "plan_to_watch", "completed", "dropped", "on_hold"};
    }
    QStringList statusLabels() const override {
        return {tr("Watching"), tr("Planned"), tr("Completed"), tr("Dropped"), tr("Paused")};
    }
    QVariantMap features() const override {
        return {{"accent", "#2E51A2"}, {"rewatches", true}, {"dates", true}, {"notes", true}, {"favourite", false}};
    }
    ScoreFormat scoreFormat() const override { return ScoreFormat::Point10; }

    static constexpr int kRedirectPort = 43218;

private:
    Grant refreshGrant(Client *client, const QString &refreshToken) override;
    QPointer<LoopbackAuth> m_pendingAuth;

    void fetchViewer();
    QString m_verifier;
};

class Trakt : public Tracker {
    Q_OBJECT
public:
    explicit Trakt(QObject *parent = nullptr);
    QString name() const override { return QStringLiteral("Trakt"); }

    void authenticate() override;
    QList<Result> search(Client *client, const QString &query) override;
    Entry entryFor(Client *client, const QString &remoteId) override;
    QString update(Client *client, const Entry &entry) override;

    bool needsClientSecret() const override { return true; }
    QString registrationUrl() const override {
        return QStringLiteral("https://trakt.tv/oauth/applications/new");
    }
    QString registrationHint() const override {
        return tr("On Trakt, create an app with the redirect URI urn:ietf:wg:oauth:2.0:oob, then paste its Client ID "
                  "and Client Secret here.");
    }
    QStringList statusVocabulary() const override {
        return {"watching", "watchlist", "completed", "dropped", "paused"};
    }
    QStringList statusLabels() const override {
        return {tr("Watching"), tr("Planned"), tr("Completed"), tr("Dropped"), tr("Paused")};
    }
    QVariantMap features() const override {
        return {{"accent", "#ED1C24"}, {"rewatches", false}, {"dates", false}, {"notes", false}, {"favourite", false}};
    }

    enum class PollOutcome { Pending, SlowDown, SignedIn, Refused };
    static PollOutcome pollOutcome(int code, bool hasToken);

private:
    Grant refreshGrant(Client *client, const QString &refreshToken) override;
    void pollForToken(const QString &deviceCode, int intervalSecs, int expiresIn);
    void fetchViewer();
    QTimer m_poll;
    bool m_polling = false;
};

class TrackerList : public QAbstractListModel {
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(int authenticatedCount READ authenticatedCount NOTIFY changed)
public:
    enum Role { NameRole = Qt::UserRole, AuthenticatedRole, AccountRole };

    explicit TrackerList(QObject *parent = nullptr) : QAbstractListModel(parent) {}
    ~TrackerList() override;

    void setTrackers(QList<Tracker *> &&trackers);

    static Tracker *byName(const QString &name) { return s_byName.value(name, nullptr); }
    int authenticatedCount() const;

    Q_INVOKABLE QStringList authenticatedNames() const;
    Q_INVOKABLE void authenticate(const QString &name);
    Q_INVOKABLE void signOut(const QString &name);
    Q_INVOKABLE QStringList statusVocabulary(const QString &name) const;
    Q_INVOKABLE QVariantMap features(const QString &name) const;

    Q_INVOKABLE QString clientId(const QString &name) const;
    Q_INVOKABLE void setClientId(const QString &name, const QString &id);
    Q_INVOKABLE bool needsClientSecret(const QString &name) const;
    Q_INVOKABLE QString clientSecret(const QString &name) const;
    Q_INVOKABLE void setClientSecret(const QString &name, const QString &secret);
    Q_INVOKABLE bool canAuthenticate(const QString &name) const;
    Q_INVOKABLE QString registrationUrl(const QString &name) const;
    Q_INVOKABLE QString registrationHint(const QString &name) const;

    // {max, step, decimals}: a spinner fixed at 0-100 writes 85 into a POINT_10 account.
    Q_INVOKABLE QVariantMap scoreScale(const QString &name) const;

    void autoPush(const QString &showLink, int episodeNumber,
                  const std::function<QVariantMap(QString, QString)> &linkLookup);

    static QVariantMap toMap(const Tracker::Entry &entry);
    static Tracker::Entry fromMap(const QVariantMap &map);

    Q_INVOKABLE void search(const QString &name, const QString &query);
    Q_INVOKABLE void link(const QString &name, const QString &showLink, const QString &remoteId);
    Q_INVOKABLE void pushEntry(const QString &name, const QString &showLink, const QVariantMap &entry);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void changed();
    void searchFinished(QString tracker, QVariantList results);
    void entryLoaded(QString tracker, QString showLink, QVariantMap entry);
    void pushFinished(QString tracker, QString showLink, bool ok, QString error, QVariantMap entry, bool automatic);

private:
    QList<Tracker *> m_trackers;
    static inline QHash<QString, Tracker *> s_byName;
    CancelToken m_cancel;
    RunSet m_runs;
};
