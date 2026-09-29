#pragma once
#include "shows/showprovider.h"
#include <QJsonArray>

class AnimePahe : public ShowProvider
{
public:
    explicit AnimePahe(QObject *parent = nullptr) : ShowProvider(parent) {}
    QString name() const override { return "AnimePahe"; }
    // .com and .ru redirect here.
    QString hostUrl() const override { return "https://animepahe.pw/"; }
    QString showUrl(const QString &link) const override { return hostUrl() + "anime/" + link; }

    QStringList availableTypes() const override { return {"Anime"}; }
    QList<ShowData>    search       (Client *client, const QString &query, int page, int typeIndex) override;
    QList<ShowData>    popular      (Client *client, int page, int typeIndex) override;
    QList<ShowData>    latest       (Client *client, int page, int typeIndex) override;
    QList<VideoServer> loadServers  (Client *client, const PlaylistItem *episode) const override;
    PlayInfo           extractSource(Client *client, VideoServer server) override;

    struct Episode { double number; QString session; QString snapshot; QString createdAt; };

private:
    int loadShow(Client *client, ShowData &show, LoadParts parts) const override;
    // From page 1 when countOnly.
    QVector<Episode> fetchEpisodes(Client *client, const QString &showSession,
                                   bool countOnly, int &reportedTotal) const;

    // X-Requested-With: the api endpoint answers HTML without it.
    const QMap<QString, QString> m_headers = {
        {"User-Agent",       kFirefoxUserAgent},
        {"X-Requested-With", "XMLHttpRequest"},
        {"Accept",           "application/json, text/javascript, */*; q=0.01"},
        {"Accept-Language",  "en-US,en;q=0.9"},
        {"Referer",          "https://animepahe.pw/"},
    };
};
