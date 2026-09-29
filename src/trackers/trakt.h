#pragma once
#include "trackers/tracker.h"
#include <QTimer>

// REST, device-code flow: no redirect and no loopback listener.
class Trakt : public Tracker {
    Q_OBJECT
public:
    explicit Trakt(QObject *parent = nullptr);
    QString name() const override { return QStringLiteral("Trakt"); }

    void authenticate() override;
    QList<Result> search(Client *client, const QString &query) override;
    Entry entryFor(Client *client, const QString &remoteId) override;
    bool update(Client *client, const Entry &entry) override;

    bool needsClientSecret() const override { return true; }
    QString registrationUrl() const override {
        return QStringLiteral("https://trakt.tv/oauth/applications/new");
    }
    QString registrationHint() const override {
        return tr("Create an application with the redirect URI "
                  "urn:ietf:wg:oauth:2.0:oob, then paste both its Client ID and "
                  "Client Secret here.");
    }
    QStringList statusVocabulary() const override {
        return {"watching", "watchlist", "completed", "dropped", "paused"};
    }

signals:
    // The user types this code at the url Trakt returns.
    void deviceCodeReady(QString userCode, QString verificationUrl);

private:
    Grant refreshGrant(Client *client, const QString &refreshToken) override;
    void pollForToken(const QString &deviceCode, int intervalSecs, int expiresIn);
    void fetchViewer();
    QTimer m_poll;
};
