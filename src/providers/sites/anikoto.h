#pragma once
#include "providers/provider.h"

class Anikoto : public Provider, public FilterBrowse {
public:
    explicit Anikoto(QObject *parent = nullptr) : Provider(parent) {}
    QString name() const override { return "Anikoto"; }
    QString hostUrl() const override { return "https://anikototv.to/"; }

    QStringList availableTypes() const override { return {"Anime"}; }
    bool publishesAiringTimes() const override { return true; }
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

    QList<ShowData> parseShowList(const QString &html, bool resultsOnly = false);

private:
    Client::Response request(Client *client, const QString &url,
                             const QMap<QString, QString> &headers,
                             bool expectJson = false, bool endAt404 = false) const;
    int loadShow(Client *client, ShowData &show, LoadParts parts) const override;

    void            loadDetails(Client *client, ShowData &show) const;
    Source        extractEmbed(Client *client, const QString &embedUrl, const VideoServer &server) const;

    QMap<QString, QString> m_headers = {
        {"User-Agent",       kFirefoxUserAgent},
        {"X-Requested-With", "XMLHttpRequest"},
        {"Referer",          "https://anikototv.to/"},
    };
};
