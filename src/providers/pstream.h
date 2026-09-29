#pragma once
#include "shows/showprovider.h"

class PStream : public ShowProvider
{
public:
    explicit PStream(QObject *parent = nullptr);
    QString name() const override { return "P-Stream"; }
    QString hostUrl() const override { return "https://aether.bar/"; }
    QString showUrl(const QString &link) const override;
    QStringList availableTypes() const override { return {"Movies", "TV Shows"}; }

    QList<ShowData>    search       (Client *client, const QString &query, int page, int typeIndex) override;
    QList<ShowData>    popular      (Client *client, int page, int typeIndex) override;
    QList<ShowData>    latest       (Client *client, int page, int typeIndex) override;
    QVariantMap        filterOptions(Client *client, int typeIndex) override;
    QList<ShowData>    filtered     (Client *client, const QString &query, int page, int typeIndex,
                                     const QVariantMap &filters, bool latest) override;
    QString            tmdbRef      (const QString &showLink) const override;
    QList<VideoServer> loadServers  (Client *client, const PlaylistItem *episode) const override;
    PlayInfo           extractSource(Client *client, VideoServer server) override;

private:
    int loadShow(Client *client, ShowData &show, LoadParts parts) const override;

    QJsonObject tmdb(Client *client, const QString &path, QMap<QString, QString> params = {}) const;
    QList<ShowData> collect(const QJsonArray &results, const QString &kind, int showType) const;

    QMap<QString, QString> m_headers;   // aether checks Origin/Referer, including on the m3u8.
    QString m_token;
};
