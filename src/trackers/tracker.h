#pragma once
#include <QDateTime>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QList>
#include "core/async.h"
#include "net/client.h"
#include <QMutex>
#include <QtConcurrent/QtConcurrentRun>

// Everything but authenticate() and signOut() runs on a worker with the caller's Client.
// Credentials are read and refreshed under a lock: sign-in and sign-out rewrite them on the GUI
// thread while those workers run.
class Tracker : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString name READ name CONSTANT)
    Q_PROPERTY(bool authenticated READ isAuthenticated NOTIFY authChanged)
    Q_PROPERTY(QString accountName READ accountName NOTIFY authChanged)
public:
    using QObject::QObject;

    // AniList lets each account pick its own scale, so the control is built from what it reports.
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
        QString status;      // in the service's own vocabulary
        double  score = 0;
        int     progress = 0;
        qint64  updatedAt = 0;
        bool    valid = false;
    };

    virtual QString name() const = 0;
    bool isAuthenticated() const { return !accessToken().isEmpty(); }
    QString accountName() const { return m_accountName; }

    QString clientId() const;
    void setClientId(const QString &id);
    // AniList and Trakt sign their exchange with a secret; MAL uses PKCE.
    virtual bool needsClientSecret() const { return false; }
    QString clientSecret() const;
    void setClientSecret(const QString &secret);
    bool canAuthenticate() const {
        return !clientId().isEmpty() && (!needsClientSecret() || !clientSecret().isEmpty());
    }
    virtual QString registrationUrl() const = 0;

    // Getting the redirect wrong fails at the exchange, with an error only the service sees.
    virtual QString registrationHint() const = 0;

    // Calling it again abandons the one in flight.
    virtual void authenticate() = 0;
    void signOut();

    virtual QList<Result> search(Client *client, const QString &query) = 0;
    virtual Entry entryFor(Client *client, const QString &remoteId) = 0;
    virtual bool update(Client *client, const Entry &entry) = 0;

    virtual QStringList statusVocabulary() const = 0;
    virtual ScoreFormat scoreFormat() const { return ScoreFormat::Point10; }

    // Cancels this tracker's own background work and waits for it. Before destruction, while
    // the derived members those workers use still exist.
    void shutdown();

signals:
    void authChanged();

protected:
    struct Grant {
        QString token;
        QString refresh;
        qint64  expiresAt = 0;
    };

    // Through the keystore, never into settings.ini. GUI thread.
    void storeToken(const Grant &grant);
    void loadToken();
    void setAccountName(const QString &name);
    QString settingsGroup() const;

    QString accessToken() const;
    // The access token, refreshed first when it is within five minutes of expiring; empty
    // when signed out or the refresh failed. Refreshes are serialised: a refresh token is
    // single-use, so two racing workers would sign the account out.
    QString freshToken(Client *client);
    // No refresh by default: AniList's tokens last a year and it issues no refresh token.
    virtual Grant refreshGrant(Client *, const QString &) { return {}; }

    // Work that must end before the tracker does. The lambda gets the tracker's cancel token.
    template <typename F>
    void runInBackground(F &&task) { m_runs.add(QtConcurrent::run(std::forward<F>(task), m_cancel)); }

private:
    mutable QMutex m_authMutex;   // guards the credentials below
    QMutex         m_refreshMutex;
    QString        m_token;
    QString        m_refreshToken;
    qint64         m_expiresAt = 0;
    QString        m_clientId;
    QString        m_clientSecret;
    QString        m_accountName;   // GUI thread only
    CancelToken    m_cancel;
    RunSet         m_runs;
};

// The port is held until the redirect arrives or the listener times out, so `start` abandons
// the first rather than letting a second fail to bind.
class LoopbackAuth : public QObject {
    Q_OBJECT
public:
    explicit LoopbackAuth(QObject *parent = nullptr);

    // Replaces `pending`. Null when the port could not be bound.
    static LoopbackAuth *start(QObject *parent, QPointer<LoopbackAuth> &pending, int port,
                               const QString &service);

    // AniList and MAL compare the redirect URI character for character.
    int listen(int port = 0, int timeoutMs = 300000);

    // Goes in the authorisation URL. A redirect that does not bring it back is ignored, so a
    // page that finds the port cannot sign the user in to someone else's account.
    QString state() const { return m_state; }

signals:
    // Authorisation-code grant: the redirect carries ?code=.
    void codeReceived(const QString &code);
    // Implicit grant: the token rides in the fragment, which never reaches a server. The
    // bridge page below hands it back as a query on a second request.
    void tokenReceived(const QString &token, qint64 expiresIn);
    void failed(const QString &reason);

private:
    void serve(class QTcpSocket *socket, const QByteArray &contentType, const QByteArray &body);
    // Answered once per listener: a second bare request means the redirect really was empty.
    bool m_servedBridge = false;
    QString m_state;
};
