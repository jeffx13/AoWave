#pragma once
#include <QPointer>
#include "trackers/tracker.h"

// REST v2, OAuth2 with PKCE.
class Mal : public Tracker {
    Q_OBJECT
public:
    explicit Mal(QObject *parent = nullptr);
    QString name() const override { return QStringLiteral("MyAnimeList"); }

    void authenticate() override;
    QList<Result> search(Client *client, const QString &query) override;
    Entry entryFor(Client *client, const QString &remoteId) override;
    bool update(Client *client, const Entry &entry) override;

    QString registrationUrl() const override {
        return QStringLiteral("https://myanimelist.net/apiconfig");
    }
    QString registrationHint() const override {
        return tr("Create an API application, set its App Redirect URL to "
                  "http://127.0.0.1:%1 and paste the Client ID here.")
            .arg(kRedirectPort);
    }
    QStringList statusVocabulary() const override {
        return {"watching", "plan_to_watch", "completed", "dropped", "on_hold"};
    }
    ScoreFormat scoreFormat() const override { return ScoreFormat::Point10; }

    // The App Redirect URL must be exactly http://127.0.0.1:43218.
    static constexpr int kRedirectPort = 43218;

private:
    Grant refreshGrant(Client *client, const QString &refreshToken) override;
    // So a second sign-in can take the port back.
    QPointer<LoopbackAuth> m_pendingAuth;

    void fetchViewer();
    QString m_verifier;
};
