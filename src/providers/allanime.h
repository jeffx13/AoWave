#pragma once
#include "shows/showprovider.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QTime>
#include "shows/playinfo.h"

class AllAnime : public ShowProvider
{
public:
    explicit AllAnime(QObject *parent = nullptr);
    QString name() const override { return "AllAnime"; }
    QString hostUrl() const override { return "https://allmanga.to/"; }
    QString showUrl(const QString &link) const override { return hostUrl() + "bangumi/" + link; }
    QStringList availableTypes() const override { return {"Anime"}; }

    QList<ShowData>    search       (Client *client, const QString &query, int page, int typeIndex) override;
    QList<ShowData>    popular      (Client *client, int page, int typeIndex) override;
    QList<ShowData>    latest       (Client *client, int page, int typeIndex) override;
    QVariantMap        filterOptions(Client *client, int typeIndex) override;
    bool               filtersSearch() const override { return true; }
    QList<ShowData>    filtered     (Client *client, const QString &query, int page, int typeIndex,
                                     const QVariantMap &filters, bool latest) override;
    QList<VideoServer> loadServers  (Client *client, const PlaylistItem *episode) const override;
    PlayInfo           extractSource(Client *client, VideoServer server) override;

    // Pure: the fixture tests feed them saved responses.
    QList<ShowData> parseJsonArray(const QJsonArray &shows, bool isPopular = false);
    QString decryptSource(const QString &input) const;

private:
    int loadShow(Client *client, ShowData &show, LoadParts parts) const override;

    static constexpr const char *kApiBase = "https://api.allanime.day/api";
    static constexpr const char *kEndPoint = "https://allanime.day";

    const QMap<QString, QString> m_headers = {
                                              {"Origin",     "https://youtu-chan.com"},
                                              {"Referer",    "https://youtu-chan.com/"},
                                              {"User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:148.0) Gecko/20100101 Firefox/148.0"},
                                              };

    static QString apiUrl(const QString &variables, const QString &hash) {
        return QString("%1?variables=%2&extensions={%22persistedQuery%22:{%22version%22:1,%22sha256Hash%22:%22%3%22}}")
        .arg(kApiBase, variables, hash);
    }

    QString coverImage(const QJsonObject &json) const;
    QString convertJsonSubToSrt(const QJsonObject &json, const QString &sourceUrl) const;

    static QString msToSrtTime(double seconds) {
        int total = static_cast<int>(seconds);
        int millis = static_cast<int>((seconds - total) * 1000);
        return QTime(total / 3600, (total % 3600) / 60, total % 60, millis)
            .toString("hh:mm:ss,zzz");
    }
};