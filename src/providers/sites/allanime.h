#pragma once
#include "providers/provider.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QTime>
#include "providers/showdata.h"

class AllAnime : public Provider, public FilterBrowse
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
    FilterBrowse *filters() override { return this; }
    QVariantMap        filterOptions(Client *client, int typeIndex) override;
    bool               filtersSearch() const override { return true; }
    QList<ShowData>    filtered     (Client *client, const QString &query, int page, int typeIndex,
                                     const QVariantMap &filters, bool latest) override;
    QList<VideoServer> loadServers  (Client *client, const PlaylistItem *episode) const override;
    Source           extractSource(Client *client, VideoServer server) override;

    QList<ShowData> parseJsonArray(const QJsonArray &shows, bool isPopular = false);
    QString decryptSource(const QString &input) const;

    static QJsonObject bakedSigning();
    static QString     bootSignature(const QJsonObject &config, qint64 epoch);

private:
    int loadShow(Client *client, ShowData &show, LoadParts parts) const override;

    static constexpr const char *kApiBase = "https://api.allanime.day/api";
    static constexpr const char *kEndPoint = "https://allanime.day";

    const QMap<QString, QString> m_headers = {
                                              {"Origin",     "https://youtu-chan.com"},
                                              {"Referer",    "https://youtu-chan.com/"},
                                              {"User-Agent", kFirefoxUserAgent},
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