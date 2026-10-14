#pragma once
#include "app/async.h"
#include "net/client.h"
#include "providers/provider.h"
#include <QCoreApplication>
#include <QDate>
#include <QFuture>
#include <QJsonArray>
#include <functional>
#include <memory>
#include <optional>

class IyfAccount;
class IyfLogin;

// Every request is signed with a key pair the home page hands out (fresh at each visit, any of them good for a while).
class IyfApi {
    Q_DECLARE_TR_FUNCTIONS(IyfApi)
public:
    struct Hosts {
        QString home = QStringLiteral("https://www.iyf.tv");
        QString api = QStringLiteral("https://m10.iyf.tv");
    };
    struct Keys {
        QString pub, priv;
        bool isEmpty() const { return pub.isEmpty() || priv.isEmpty(); }
    };
    struct Session {
        QString uid, expire, gid, sign, token;
        bool isEmpty() const { return uid.isEmpty() || sign.isEmpty() || token.isEmpty(); }
        QString query() const;
        bool operator==(const Session &) const = default;
    };
    struct Reply {
        int code = -1;
        QString message;
        QJsonArray info;
        bool ok() const { return code == 0; }
        bool signatureRefused() const { return code == 1 && message.contains(QStringLiteral("签名")); }
        bool sessionExpired() const { return code == 2 || code == 4; }
        bool signedInElsewhere() const { return code == 3; }
    };
    struct SignIn {
        Session session;
        QString autoLoginKey;
        QString userName, nickName;
    };
    struct Membership {
        QString uid, name;
        QString tier;
        int daysLeft = 0;
        QDate until;
        int gid = 0;
        int level = -1;
        qint64 experience = -1, nextLevel = -1, coins = -1;
        QString since, lastLogin, region;
        int phoneBound = -1;
        bool member() const { return daysLeft > 0; }
    };

    explicit IyfApi(Hosts hosts = {});

    const Hosts &hosts() const { return m_hosts; }

    static QString sign(const QString &input, const Keys &keys);
    // The keys of this site's home page. A stale pair is not told from a missing one by the page: a request it
    // signs is answered "签名错误", which is when forgetKeys() and a second visit is the cure.
    Keys keys(Client *client) const;
    void forgetKeys() const;

    Reply call(Client *client, const std::function<Client::Response(const Keys &)> &send) const;
    // A signed GET. The signature is over the query as the site reads it, decoded; `sent` is the query as it goes over the wire.
    Reply get(Client *client, const QString &path, const QString &query, const QString &sent = {}) const;

    std::optional<SignIn> signIn(Client *client, const QString &token) const;
    std::optional<QString> tokenFor(Client *client, const QString &autoLoginKey) const;
    std::optional<Session> renewed(Client *client, const Session &session) const;
    enum class Standing { Good, Expired, Elsewhere, Unknown };
    // getuserinfo knows whether the session is alive: getuserext answers "fine" for a session that has been dead for weeks.
    struct Profile {
        Standing state = Standing::Unknown;
        std::optional<Membership> member;
    };
    Profile profile(Client *client, const Session &session) const;
    struct Permission {
        QString title;
        bool valid = false;
    };
    QList<Permission> permissions(Client *client, const Session &session) const;

private:
    static Reply reply(const Client::Response &response);

    Hosts m_hosts;
};

class Iyf : public Provider, public FilterBrowse, public DanmakuSource
{
public:
    explicit Iyf(QObject *parent = nullptr, IyfApi::Hosts hosts = {});

    QString name() const override { return "爱壹帆"; }
    QString hostUrl() const override { return "https://www.iyf.tv"; }
    QString showUrl(const QString &link) const override { return hostUrl() + "/play/" + link; }
    QString language() const override { return QStringLiteral("zh_CN"); }
    QStringList availableTypes() const override {
        return {"动漫", "电影", "电视剧", "综艺", "纪录片"};
    }
    Account *account() override;

    QList<ShowData>    search       (Client *client, const QString &query, int page, int typeIndex) override;
    QList<ShowData>    popular      (Client *client, int page, int typeIndex) override { return browse(client, page, false, typeIndex); }
    QList<ShowData>    latest       (Client *client, int page, int typeIndex) override { return browse(client, page, true, typeIndex); }
    FilterBrowse *filters() override { return this; }
    DanmakuSource *danmaku() override { return this; }
    QList<DanmakuComment> comments(Client *client, const QVariantMap &ids) override;
    QVariantMap        filterOptions(Client *client, int typeIndex) override;
    QList<ShowData>    filtered     (Client *client, const QString & , int page, int typeIndex,
                                     const QVariantMap &filters, bool latest) override {
        return browse(client, page, latest, typeIndex, filters);
    }
    QList<VideoServer> loadServers  (Client *, const PlaylistItem *episode) const override { return {VideoServer{"Default", episode->link}}; }
    Source           extractSource(Client *client, VideoServer server) override;
    IyfApi::Session    session      () const;
    static QString     cover        (const QString &image);
    QString            detailCover  (const QString &url) const override;
    QString            listCover    (const QString &stored) const override;

private:
    int             loadShow(Client *client, ShowData &show, LoadParts parts) const override;
    QList<ShowData> browse  (Client *client, int page, bool latest, int typeIndex,
                             const QVariantMap &filters = {});
    QJsonObject     callApi (Client *client, const QString &path, const QString &query) const;
    void            explainLockedQualities(Client *client, const IyfApi::Session &session) const;
    // Static: it runs after the server race that created it, and may outlive the provider.
    static QList<DanmakuComment> fetchDanmaku(Client *client, const IyfApi &api, const QString &uniqueKey);

    static constexpr const char *kCategoryIds[] = {
        "0,1,6",
        "0,1,3",
        "0,1,4",
        "0,1,5",
        "0,1,7",
    };
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

    IyfApi m_api;
    IyfAccount *m_account;
};

// Signing in is the site's own login page in a window; the password goes to the site, never here.
class IyfAccount : public Account {
    Q_OBJECT
    QML_ANONYMOUS
public:
    explicit IyfAccount(IyfApi api, QObject *parent = nullptr);
    ~IyfAccount() override;

    QString label() const override { return QStringLiteral("爱壹帆 (iyf)"); }
    QString hint() const override;
    QString offlineHint() const override;
    QStringList required() const override { return {QStringLiteral("uid"), QStringLiteral("sign"), QStringLiteral("token")}; }

    void signIn() override;
    void signInWithToken(const QString &token);
    void refresh() override;
    void signOut() override;

    IyfApi::Session session() const;
    static QDateTime sessionEnd(const QString &expire);

protected:
    void adopted() override;

private:
    void look();
    void signedInWith(const IyfApi::SignIn &signedIn);
    static Fields fieldsOf(const IyfApi::Session &session, const QString &autoLoginKey, const QString &name);
    static QList<QPair<QString, QString>> detailsOf(const IyfApi::Membership *member, const QString &uid, const QList<IyfApi::Permission> &permissions, bool renews);

    IyfApi m_api;
    CancelToken m_cancel;
    QFuture<void> m_signIn, m_check;
    // On the GUI thread, not read off the futures: a worker's last act is a queued call that runs before its
    // future is finished, and a question asked in that gap must not be dropped.
    bool m_signingIn = false, m_checking = false, m_recheck = false;
    std::unique_ptr<IyfLogin> m_login;
};
