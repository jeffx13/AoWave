#pragma once
#include "app/async.h"
#include "providers/provider.h"
#include <QFuture>
#include <QJsonObject>
#include <QMutex>
#include <atomic>
#include <functional>

class BilibiliAccount;

class Bilibili : public Provider, public FilterBrowse, public WatchHistory, public DanmakuSource
{
public:
    explicit Bilibili(QObject *parent = nullptr);
    QString name() const override { return "哔哩哔哩"; }
    QString hostUrl() const override { return "https://www.bilibili.com/"; }
    QString showUrl(const QString &link) const override;
    QString language() const override { return QStringLiteral("zh_CN"); }
    QStringList availableTypes() const override {
        return {"国创", "番剧", "电影", "电视剧", "综艺", "纪录片", "视频"};
    }
    bool playsDirectly(const QString &link) const override { return link.startsWith(QLatin1String("BV")); }
    Account *account() override;

    QList<ShowData>    search       (Client *client, const QString &query, int page, int typeIndex) override;
    QList<ShowData>    popular      (Client *client, int page, int typeIndex) override;
    QList<ShowData>    latest       (Client *client, int page, int typeIndex) override;
    FilterBrowse *filters() override { return this; }
    WatchHistory *watchHistory() override { return this; }
    DanmakuSource *danmaku() override { return this; }
    QList<DanmakuComment> comments(Client *client, const QVariantMap &ids) override;
    QVariantMap        filterOptions(Client *client, int typeIndex) override;
    QList<ShowData>    filtered     (Client *client, const QString &query, int page, int typeIndex,
                                     const QVariantMap &filters, bool latest) override;
    QList<VideoServer> loadServers  (Client *client, const PlaylistItem *episode) const override;
    Source           extractSource(Client *client, VideoServer server) override;
    bool               parseUrl     (const QUrl &url, QString &showLink, int &episodeIndex) const override;
    bool               reportProgress(Client *client, const QString &episodeLink, double seconds,
                                      double duration) override;
    RemoteProgress     fetchProgress(Client *client, const QString &episodeLink) override;
    qint64             clockSkew() const override { return m_clockSkew.load(std::memory_order_relaxed); }

    static qint64 regularSlotAfter(const QString &schedule, qint64 earlyAt);

    bool reloadCredentials();
    bool isSignedIn() const;
    QMap<QString, QString> headers() const;
    QString proxyApi() const;
    QString cdnHost() const;
    QString csrf() const;

    static void primeWebSession(Client *client);
    static QMap<QString, QString> withWebSession(Client *client, QMap<QString, QString> headers);
    // The same minus any account: danmaku is public and a stale sign-in must not hide it.
    static QMap<QString, QString> anonymousHeaders(Client *client);
    static QString signedQuery(Client *client, QMap<QString, QString> params);

    struct DanmakuSegment {
        QList<DanmakuComment> comments;
        QString refusal;
    };
    static QList<DanmakuComment> walkDanmaku(Client *client, int durationMs,
                                             const std::function<DanmakuSegment(int index)> &fetch,
                                             QString &refusal, QStringList &missing);

    static QList<DanmakuComment> fetchDanmaku(Client *client, qint64 cid, qint64 aid, int durationMs,
                                              const QMap<QString, QString> &headers,
                                              const QString &proxyApi);

private:

    int loadShow(Client *client, ShowData &show, LoadParts parts) const override;
    int loadSeason(Client *client, ShowData &show, LoadParts parts) const;
    int loadVideo(Client *client, ShowData &show, LoadParts parts) const;
    QList<ShowData> filterSearch(Client *client, int sortBy, int page, int typeIndex,
                                 const QVariantMap &filters = {});
    static constexpr int kVideoType = 6;
    QList<ShowData> videos(const QJsonArray &items) const;

    Client::Response apiGet(Client *client, const QString &url,
                            const QMap<QString, QString> &params = {}) const;
    Client::Response apiGetSigned(Client *client, const QString &url,
                                  QMap<QString, QString> params) const;

    static constexpr int kSeasonTypes[] = {
        4,
        1,
        2,
        5,
        7,
        3,
    };
    static constexpr ShowData::ShowType kShowTypes[] = {
        ShowData::Anime,
        ShowData::Anime,
        ShowData::Movie,
        ShowData::TvSeries,
        ShowData::Variety,
        ShowData::Documentary,
    };

    BilibiliAccount *m_account;
    mutable QMutex m_credentialsMutex;
    QMap<QString, QString> m_headers;
    QString m_proxyApi;
    QString m_cdnHost;
    QString m_csrf;
    bool m_credentialsLoaded = false;
    std::atomic<bool> m_syncRefused{false};
    std::atomic<qint64> m_clockSkew{0};
    void noteServerClock(const QString &dateHeader);
};

class BilibiliAccount : public Account {
    Q_OBJECT
    QML_ANONYMOUS
public:
    explicit BilibiliAccount(QObject *parent = nullptr);
    ~BilibiliAccount() override;

    QString label() const override;
    QString hint() const override;
    QString offlineHint() const override;
    QStringList required() const override;
    bool syncsProgress() const override { return true; }

    void signIn() override;
    void copyLoginLink() const override;
    void cancelLogin() override;
    void refresh() override;
    void signOut() override;

    bool keepTicket(const QString &ticket, qint64 expiresAt);

    static Standing standingFrom(const QJsonObject &nav);
    // SESSDATA is "<hex>,<expiry as epoch seconds>,<hash>*..." with the commas percent-encoded.
    static QDateTime sessionEnd(const QString &sessdata);

    struct LoginTicket {
        QString qrcodeKey;
        QUrl    confirmUrl;
        bool isValid() const { return !qrcodeKey.isEmpty() && confirmUrl.isValid(); }
    };
    enum class LoginStatus {
        Pending,
        Scanned,
        Confirmed,
        Expired,
        Failed,
    };
    struct LoginPoll {
        LoginStatus status = LoginStatus::Pending;
        QMap<QString, QString> cookies;
    };
    static LoginTicket beginLogin(Client *client);
    static LoginPoll pollLogin(Client *client, const LoginTicket &ticket);

protected:
    void adopted() override;

private:
    void look();
    void showLogin(const QString &url, const QString &status);
    void signedInWith(const Fields &cookies);

    CancelToken   m_loginCancel;
    CancelToken   m_checkCancel;
    QFuture<void> m_loginFuture;
    QFuture<void> m_accountFuture;
    // On the GUI thread, not read off the future: the worker's last act is a queued call that runs before the future
    // is finished, and a question asked in that gap must not be dropped.
    bool m_checking = false, m_recheck = false;
    QString       m_loginUrl;
};
