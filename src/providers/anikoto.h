#pragma once
#include "shows/showprovider.h"

class Anikoto : public ShowProvider {
public:
    explicit Anikoto(QObject *parent = nullptr) : ShowProvider(parent) {}
    QString name() const override { return "Anikoto"; }
    QString hostUrl() const override { return "https://anikototv.to/"; }

    QStringList availableTypes() const override { return {"Anime"}; }
    bool publishesAiringTimes() const override { return true; }
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
    // `resultsOnly`: a search page's results, not the top-rated column beside them.
    QList<ShowData> parseShowList(const QString &html, bool resultsOnly = false);

private:
    // `endAt404`: a 404 means there is nothing more, as past a search's last page.
    Client::Response request(Client *client, const QString &url,
                             const QMap<QString, QString> &headers,
                             bool expectJson = false, bool endAt404 = false) const;
    int loadShow(Client *client, ShowData &show, LoadParts parts) const override;

    void            loadDetails(Client *client, ShowData &show) const;
    PlayInfo        extractEmbed(Client *client, const QString &embedUrl, const VideoServer &server) const;

    QMap<QString, QString> m_headers = {
        {"User-Agent",       kFirefoxUserAgent},
        {"X-Requested-With", "XMLHttpRequest"},
        {"Referer",          "https://anikototv.to/"},
    };
};
