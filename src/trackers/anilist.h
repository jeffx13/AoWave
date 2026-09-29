#pragma once
#include <QPointer>
#include "trackers/tracker.h"

// GraphQL, OAuth2 authorisation code through a loopback redirect.
class AniList : public Tracker {
    Q_OBJECT
public:
    explicit AniList(QObject *parent = nullptr);
    QString name() const override { return QStringLiteral("AniList"); }

    void authenticate() override;
    QList<Result> search(Client *client, const QString &query) override;
    Entry entryFor(Client *client, const QString &remoteId) override;
    bool update(Client *client, const Entry &entry) override;

    QStringList statusVocabulary() const override {
        return {"CURRENT", "PLANNING", "COMPLETED", "DROPPED", "PAUSED", "REPEATING"};
    }
    ScoreFormat scoreFormat() const override { return m_scoreFormat; }

    // Optional. Without one this signs in by implicit grant, which AniList allows and which
    // wants nothing but the id - the secret only buys the authorisation-code exchange, and
    // pasting the id into the secret box is what "invalid_client" actually means.
    bool needsClientSecret() const override { return false; }
    QString registrationUrl() const override {
        return QStringLiteral("https://anilist.co/settings/developer");
    }
    QString registrationHint() const override {
        return tr("On AniList, Create New Client with the redirect URL "
                  "http://127.0.0.1:%1 - exactly, no trailing slash - then paste its "
                  "Client ID here. The Client Secret is optional; leave it blank "
                  "unless you have one, and never paste the ID into it.")
            .arg(kRedirectPort);
    }
    // AniList compares the redirect URI exactly, so the port is fixed.
    static constexpr int kRedirectPort = 43217;

private:
    // So a second sign-in can take the port back off an abandoned first.
    QPointer<LoopbackAuth> m_pendingAuth;

    QJsonObject graphql(Client *client, const QString &query, const QJsonObject &variables);
    // Trades an authorisation code for a token. Only reachable with a client secret.
    void exchangeCode(const QString &code);
    void fetchViewer();

    // Assuming 0-10 writes wrong ratings on a 100-point account.
    ScoreFormat m_scoreFormat = ScoreFormat::Point10;
};
