#pragma once
#include "providers/provider.h"
#include <QHash>

class Miruro : public Provider
{
public:
    explicit Miruro(QObject *parent = nullptr) : Provider(parent) {}
    QString name() const override { return "Miruro"; }
    QString hostUrl() const override { return "https://www.miruro.to/"; }
    QStringList availableTypes() const override { return {"Anime"}; }

    QList<ShowData>    search       (Client *client, const QString &query, int page, int typeIndex) override;
    QList<ShowData>    popular      (Client *client, int page, int typeIndex) override;
    QList<ShowData>    latest       (Client *client, int page, int typeIndex) override;
    QList<VideoServer> loadServers  (Client *client, const PlaylistItem *episode) const override;
    Source           extractSource(Client *client, VideoServer server) override;

private:
    int loadShow(Client *client, ShowData &show, LoadParts parts) const override;

    QJsonDocument pipe(Client *client, const QString &path, const QJsonObject &query = {}) const;
    QJsonArray    browse(Client *client, const QJsonObject &query, QList<ShowData> &out) const;

    QByteArray obfuscationKey(Client *client, bool refresh = false) const;

    QJsonObject episodesFor(Client *client, int anilistId) const;

    mutable QMutex m_cacheMutex;
    mutable QByteArray m_key;
    mutable QHash<int, QJsonObject> m_episodes;
};
