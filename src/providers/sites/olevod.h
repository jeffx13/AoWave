#pragma once
#include "providers/provider.h"

class Olevod : public Provider, public FilterBrowse {
public:
    explicit Olevod(QObject *parent = nullptr) : Provider(parent) {}
    QString name() const override { return "OleVod"; }
    QString hostUrl() const override { return "https://www.olevod.com/"; }
    QString showUrl(const QString &link) const override { return hostUrl() + "index.php/vod/detail/id/" + link; }
    QString language() const override { return QStringLiteral("zh_CN"); }

    QStringList availableTypes() const override { return {"动漫", "电影", "连续剧", "综艺"}; }
    QList<ShowData>    search       (Client *client, const QString &query, int page, int typeIndex) override;
    QList<ShowData>    popular      (Client *client, int page, int typeIndex) override;
    QList<ShowData>    latest       (Client *client, int page, int typeIndex) override;
    FilterBrowse *filters() override { return this; }
    QVariantMap        filterOptions(Client *client, int typeIndex) override;
    QList<ShowData>    filtered     (Client *client, const QString &query, int page, int typeIndex,
                                     const QVariantMap &filters, bool latest) override;
    QList<VideoServer> loadServers  (Client *client, const PlaylistItem *episode) const override;
    Source           extractSource(Client *client, VideoServer server) override;

    QList<ShowData> parseList(const QJsonArray &items);

private:
    int loadShow(Client *client, ShowData &show, LoadParts parts) const override;

    static QString signedUrl(const QString &path);
    static QString vv(qint64 unixSeconds);
    QList<ShowData> listing(Client *client, int page, int typeIndex, const QString &sort,
                            const QVariantMap &filters = {});

    static constexpr const char *kApi = "https://api.olelive.com";
    static constexpr const char *kStatic = "https://static.olelive.com/";
    static constexpr int kTypeIds[] = {4, 1, 2, 3};
    static constexpr int kPageSize = 48;

    QMap<QString, QString> m_headers = {
        {"User-Agent", kFirefoxUserAgent},
        {"Origin",     "https://www.olevod.com"},
        {"Referer",    "https://www.olevod.com/"},
    };
};
