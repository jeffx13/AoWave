#pragma once
#include "shows/showprovider.h"

class Duboku : public ShowProvider {
public:
    explicit Duboku(QObject *parent = nullptr) : ShowProvider(parent) {}
    QString name() const override { return "Duboku"; }
    QString hostUrl() const override { return "https://www.duboku.lv/"; }
    QString showUrl(const QString &link) const override { return hostUrl() + "v/" + link + ".html"; }
    QString language() const override { return QStringLiteral("zh_CN"); }

    QStringList availableTypes() const override { return {"动漫", "电影", "剧集", "综艺"}; }
    QList<ShowData>    search       (Client *client, const QString &query, int page, int typeIndex) override;
    QList<ShowData>    popular      (Client *client, int page, int typeIndex) override;
    QList<ShowData>    latest       (Client *client, int page, int typeIndex) override;
    QList<VideoServer> loadServers  (Client *client, const PlaylistItem *episode) const override;
    PlayInfo           extractSource(Client *client, VideoServer server) override;

    // Pure: the fixture tests feed them saved responses.
    QList<ShowData> parseList(const QString &html, int showType);

private:
    int loadShow(Client *client, ShowData &show, LoadParts parts) const override;

    QList<ShowData> listing(Client *client, int page, int typeIndex, const QString &by);
    QString absolute(const QString &path) const;

    static int showTypeOf(const QString &label);

    static constexpr int kTypeIds[] = {4, 1, 2, 3};

    QMap<QString, QString> m_headers = {
        {"User-Agent", kFirefoxUserAgent},
        {"Referer",    "https://www.duboku.lv/"},
    };
};
