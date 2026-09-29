#pragma once
#include "shows/showprovider.h"
#include <QMutex>
#include <atomic>

class Bilibili : public ShowProvider
{
public:
    explicit Bilibili(QObject *parent = nullptr);
    QString name() const override { return "哔哩哔哩"; }
    QString hostUrl() const override { return "https://www.bilibili.com/"; }
    QString showUrl(const QString &link) const override;
    QString language() const override { return QStringLiteral("zh_CN"); }
    QStringList availableTypes() const override {
        return {"国创", "番剧", "电影", "电视剧", "综艺", "纪录片"};
    }

    QList<ShowData>    search       (Client *client, const QString &query, int page, int typeIndex) override;
    QList<ShowData>    popular      (Client *client, int page, int typeIndex) override;
    QList<ShowData>    latest       (Client *client, int page, int typeIndex) override;
    QVariantMap        filterOptions(Client *client, int typeIndex) override;
    QList<ShowData>    filtered     (Client *client, const QString &query, int page, int typeIndex,
                                     const QVariantMap &filters, bool latest) override;
    QList<VideoServer> loadServers  (Client *client, const PlaylistItem *episode) const override;
    PlayInfo           extractSource(Client *client, VideoServer server) override;
    bool               parseUrl     (const QUrl &url, QString &showLink, int &episodeIndex) const override;
    bool               syncsProgress() const override { return true; }
    bool               reportProgress(Client *client, const QString &episodeLink, double seconds,
                                      double duration) override;
    RemoteProgress     fetchProgress(Client *client, const QString &episodeLink) override;
    qint64             clockSkew() const override { return m_clockSkew.load(std::memory_order_relaxed); }

    // The signed-in jar, sealed like tracker tokens: SESSDATA is the account. GUI thread only.
    static QMap<QString, QString> storedCookies();
    // Empty values are dropped, so an empty map signs out. False if it could not be sealed.
    static bool storeCookies(const QMap<QString, QString> &cookies);
    // A renewed bili_ticket into the stored jar, so the next start does not open with a lapsed
    // one; a jar whose own ticket has a day or more left keeps it. Nothing when signed out.
    // GUI thread; true when it was stored.
    static bool keepTicket(const QString &ticket, qint64 expiresAt);

    // True only when the cookies changed. Safe from the GUI thread while workers run.
    bool reloadCredentials();
    bool isSignedIn() const;
    QMap<QString, QString> headers() const;
    QString csrf() const;

    // bilibili's risk control wants a browser's fingerprint cookies and an unexpired
    // bili_ticket even from a signed-out visitor; without them x/web-interface/view answers
    // 412 and the danmaku endpoints return nothing. Per run, shared by every worker.
    static void primeWebSession(Client *client);
    // Merges the session's cookies into `headers`, leaving the caller's own values alone.
    static QMap<QString, QString> withWebSession(Client *client, QMap<QString, QString> headers);
    // The same minus any account: danmaku is public and a stale sign-in must not hide it.
    static QMap<QString, QString> anonymousHeaders(Client *client);
    // Adds `wts` and `w_rid` and returns the whole query string already encoded - the
    // signature only holds if the request is spelled the same way it was signed.
    static QString signedQuery(Client *client, QMap<QString, QString> params);

    // durationMs may be 0; aid may be 0 on older links.
    static QList<DanmakuComment> fetchDanmaku(Client *client, qint64 cid, qint64 aid, int durationMs,
                                              const QMap<QString, QString> &headers,
                                              const QString &proxyApi);

    // bilibili's web QR endpoints. Static: Application drives this before any provider call.
    struct LoginTicket {
        QString qrcodeKey;    // what pollLogin() is keyed on
        QUrl    confirmUrl;
        bool isValid() const { return !qrcodeKey.isEmpty() && confirmUrl.isValid(); }
    };
    enum class LoginStatus {
        Pending,     // issued, nobody has confirmed
        Scanned,     // opened, awaiting the confirm tap
        Confirmed,   // `cookies` is filled in
        Expired,
        Failed,
    };
    struct LoginPoll {
        LoginStatus status = LoginStatus::Pending;
        QMap<QString, QString> cookies;   // SESSDATA, bili_jct, DedeUserID, ...
    };

    // An invalid ticket means bilibili refused.
    static LoginTicket beginLogin(Client *client);
    // About once a second: bilibili rate-limits harder.
    static LoginPoll pollLogin(Client *client, const LoginTicket &ticket);
    // Raw Cookie header or a DevTools copy; empty unless all three are present.
    static QMap<QString, QString> parseCookieString(const QString &text);

private:
    // "media season", "ss id" / "ep id", or "BVxxxxxxxxxx". Episodes are
    // "season&ep&cid&aid" or "BV...&cid".

    int loadShow(Client *client, ShowData &show, LoadParts parts) const override;
    int loadSeason(Client *client, ShowData &show, LoadParts parts) const;
    int loadVideo(Client *client, ShowData &show, LoadParts parts) const;
    QList<ShowData> filterSearch(Client *client, int sortBy, int page, int typeIndex,
                                 const QVariantMap &filters = {});

    // Via bilibili/proxy, for geo-locked content.
    Client::Response apiGet(Client *client, const QString &url,
                            const QMap<QString, QString> &params = {}) const;
    // For the endpoints risk control refuses unsigned. Takes params by value: signing adds to
    // them, and the signed query travels in the url rather than as params.
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

    // Rebuilt on sign-in, read from workers.
    mutable QMutex m_credentialsMutex;
    QMap<QString, QString> m_headers;
    QString m_proxyApi;
    QString m_csrf;   // bili_jct, echoed by every account write
    std::atomic<bool> m_syncRefused{false};
    // EWMA of (server now - local now), from response Date headers.
    std::atomic<qint64> m_clockSkew{0};
    void noteServerClock(const QString &dateHeader);
};
