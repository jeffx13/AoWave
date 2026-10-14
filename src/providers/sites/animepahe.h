#pragma once
#include "providers/provider.h"
#include <QJsonArray>

class AnimePahe : public Provider
{
public:
    explicit AnimePahe(QObject *parent = nullptr) : Provider(parent) {}
    QString name() const override { return "AnimePahe"; }
    QString hostUrl() const override { return "https://animepahe.pw/"; }
    QString showUrl(const QString &link) const override { return hostUrl() + "anime/" + link; }
    QString episodeUrl(const PlaylistItem &episode) const override { return hostUrl() + (episode.link.startsWith('/') ? episode.link.mid(1) : episode.link); }

    QStringList availableTypes() const override { return {"Anime"}; }
    QList<ShowData>    search       (Client *client, const QString &query, int page, int typeIndex) override;
    QList<ShowData>    popular      (Client *client, int page, int typeIndex) override;
    QList<ShowData>    latest       (Client *client, int page, int typeIndex) override;
    QList<VideoServer> loadServers  (Client *client, const PlaylistItem *episode) const override;
    Source           extractSource(Client *client, VideoServer server) override;

    struct Episode { double number; QString session; QString snapshot; QString createdAt; };

private:
    int loadShow(Client *client, ShowData &show, LoadParts parts) const override;
    QVector<Episode> fetchEpisodes(Client *client, const QString &showSession,
                                   bool countOnly, int &reportedTotal) const;

    // api answers HTML without X-Requested-With
    const QMap<QString, QString> m_headers = {
        {"User-Agent",       kFirefoxUserAgent},
        {"X-Requested-With", "XMLHttpRequest"},
        {"Accept",           "application/json, text/javascript, */*; q=0.01"},
        {"Accept-Language",  "en-US,en;q=0.9"},
        {"Referer",          "https://animepahe.pw/"},
    };
};
