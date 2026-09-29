#pragma once
#include <QAbstractListModel>
#include <QHash>
#include <QVariantMap>
#include <functional>
#include <qqmlintegration.h>
#include "trackers/tracker.h"

class TrackerList : public QAbstractListModel {
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(int authenticatedCount READ authenticatedCount NOTIFY changed)
public:
    enum Role { NameRole = Qt::UserRole, AuthenticatedRole, AccountRole };

    explicit TrackerList(QObject *parent = nullptr) : QAbstractListModel(parent) {}
    ~TrackerList() override;

    // Takes ownership.
    void setTrackers(QList<Tracker *> &&trackers);

    static Tracker *byName(const QString &name) { return s_byName.value(name, nullptr); }
    int authenticatedCount() const;

    Q_INVOKABLE QStringList authenticatedNames() const;
    Q_INVOKABLE void authenticate(const QString &name);
    Q_INVOKABLE void signOut(const QString &name);
    Q_INVOKABLE QStringList statusVocabulary(const QString &name) const;

    // The settings page reads and writes the client id.
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

    // Silent when nothing is linked, the switch is off, or the service already knows.
    void autoPush(const QString &showLink, int episodeNumber,
                  const std::function<QVariantMap(QString, QString)> &linkLookup);

    // Fire-and-forget from QML; results land on the signals below.
    Q_INVOKABLE void search(const QString &name, const QString &query);
    Q_INVOKABLE void link(const QString &name, const QString &showLink, const QString &remoteId);
    Q_INVOKABLE void unlink(const QString &name, const QString &showLink);
    Q_INVOKABLE void pushEntry(const QString &name, const QString &showLink,
                               const QString &status, double score, int progress);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void changed();
    void searchFinished(QString tracker, QVariantList results);
    void entryLoaded(QString tracker, QString showLink, QVariantMap entry);
    void pushFinished(QString tracker, bool ok);

private:
    QList<Tracker *> m_trackers;
    static inline QHash<QString, Tracker *> s_byName;
    CancelToken m_cancel;
    RunSet m_runs;
};
