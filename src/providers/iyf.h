#pragma once
#include "shows/showprovider.h"
#include <QJsonArray>

class Iyf : public ShowProvider
{
public:
    explicit Iyf(QObject *parent = nullptr);

    QString name() const override { return "爱壹帆"; }
    QString hostUrl() const override { return "https://www.iyf.tv"; }
    QString showUrl(const QString &link) const override { return hostUrl() + "/play/" + link; }
    QString language() const override { return QStringLiteral("zh_CN"); }
    QStringList availableTypes() const override {
        return {"动漫", "电影", "电视剧", "综艺", "纪录片"};
    }

    QList<ShowData>    search       (Client *client, const QString &query, int page, int typeIndex) override;
    QList<ShowData>    popular      (Client *client, int page, int typeIndex) override { return browse(client, page, false, typeIndex); }
    QList<ShowData>    latest       (Client *client, int page, int typeIndex) override { return browse(client, page, true, typeIndex); }
    QVariantMap        filterOptions(Client *client, int typeIndex) override;
    QList<ShowData>    filtered     (Client *client, const QString & /*query*/, int page, int typeIndex,
                                     const QVariantMap &filters, bool latest) override {
        return browse(client, page, latest, typeIndex, filters);
    }
    // The episode link is the stream.
    QList<VideoServer> loadServers  (Client *, const PlaylistItem *episode) const override { return {VideoServer{"Default", episode->link}}; }
    PlayInfo           extractSource(Client *client, VideoServer server) override;

private:
    using KeyPair = QPair<QString, QString>;   // public, private

    int             loadShow(Client *client, ShowData &show, LoadParts parts) const override;
    QList<ShowData> browse  (Client *client, int page, bool latest, int typeIndex,
                             const QVariantMap &filters = {});
    QJsonObject     callApi (Client *client, const QString &prefixUrl, const QString &query) const;
    KeyPair         authKeys(Client *client) const;
    static QString  sign    (const QString &input, const KeyPair &keys);
    // Static: it runs after the server race that created it, and may outlive the provider.
    static QList<DanmakuComment> fetchDanmaku(Client *client, const QString &uniqueKey,
                                              const KeyPair &keys);
    QString         session () const;   // the uid/expire/sign/token query fragment

    static constexpr const char *kCategoryIds[] = {
        "0,1,6",   // 动漫
        "0,1,3",   // 电影
        "0,1,4",   // 电视剧
        "0,1,5",   // 综艺
        "0,1,7",   // 纪录片
    };
    // Each category's genre list, in the same order.
    static constexpr const char *kGenreLists[] = {
        "AnimeType", "FilmType", "TvType", "VarietyType", "DocumentaryType",
    };
    static constexpr ShowData::ShowType kShowTypes[] = {
        ShowData::Anime, ShowData::Movie, ShowData::TvSeries,
        ShowData::Variety, ShowData::Documentary,
    };

    const QMap<QString, QString> m_headers = {
        {"referer", "https://www.iyf.tv"},
        {"X-Requested-With", "XMLHttpRequest"},
    };

    QString m_expire, m_sign, m_token, m_uid;   // account query params from settings.ini
};
