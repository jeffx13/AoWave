#include "providers/sites/iyf.h"
#include <QCryptographicHash>
#include <QDate>
#include <QJsonObject>
#include <QMap>
#include <QMutex>
#include <QPointer>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineProfileBuilder>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineView>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <optional>
#include "app/mainwindow.h"
#include "app/async.h"
#include "media/player/danmaku.h"
#include "app/exception.h"
#include "app/logger.h"

namespace {
QMutex g_keysMutex;
QMap<QString, IyfApi::Keys> g_keys;   // by home page, so a test server and the site do not share a pair

// The site sends some numbers as numbers and some as text.
QString text(const QJsonValue &value) {
    if (value.isString()) return value.toString();
    if (value.isDouble()) return Account::plain(value.toDouble());
    return {};
}

qint64 whole(const QJsonValue &value, qint64 fallback = -1) {
    bool ok = false;
    const double number = text(value).toDouble(&ok);
    return ok ? qint64(number) : fallback;
}

QDate dayOf(const QJsonValue &value) {
    const QString raw = text(value).trimmed();
    if (raw.isEmpty()) return {};
    bool numeric = false;
    const double number = raw.toDouble(&numeric);
    if (numeric && number > 1e8) return QDateTime::fromSecsSinceEpoch(qint64(number > 1e11 ? number / 1000 : number)).date();
    QDate date = QDate::fromString(raw.left(10), Qt::ISODate);
    if (!date.isValid()) date = QDate::fromString(raw.left(10).replace(QLatin1Char('/'), QLatin1Char('-')), Qt::ISODate);
    return date;
}

// The site names a membership by a picture. Whether the account is a member is the days left, not this.
QString tierOf(const QString &image) {
    const QString name = image.toLower();
    if (name.contains(QLatin1String("royal-vvvip"))) return QStringLiteral("至尊VIP");
    if (name.contains(QLatin1String("g-vip"))) return QStringLiteral("黄金VIP");
    if (name.contains(QLatin1String("cpvip")) || name.contains(QLatin1String("editor"))) return QStringLiteral("情侣VIP");
    if (name.contains(QLatin1String("et-vip"))) return QStringLiteral("企业会员");
    if (name.contains(QLatin1String("royal-admin"))) return QStringLiteral("站长");
    if (name.contains(QLatin1String("normal-vip"))) return QStringLiteral("普通会员");
    return image.isEmpty() ? QString() : QStringLiteral("VIP");
}

QString shapeOf(const QJsonObject &object) {
    QStringList parts;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        const char *kind = it->isString() ? "text" : it->isDouble() ? "number" : it->isBool() ? "bool" : it->isArray() ? "list"
                           : it->isObject() ? "object" : "null";
        parts << it.key() + QLatin1Char(':') + QLatin1String(kind);
    }
    return parts.join(QStringLiteral(", "));
}

std::atomic_bool g_toldInfoShape{false}, g_toldPermissionShape{false};

QString encoded(const QString &value) { return QString::fromLatin1(QUrl::toPercentEncoding(value)); }
}

IyfApi::IyfApi(Hosts hosts) : m_hosts(std::move(hosts)) {}

QString IyfApi::Session::query() const {
    if (isEmpty()) return QStringLiteral("uid=&expire=&gid=1&sign=&token=");
    return QStringLiteral("uid=%1&expire=%2&gid=%3&sign=%4&token=%5")
        .arg(uid, expire, gid.isEmpty() ? QStringLiteral("1") : gid, sign, token);
}

QString IyfApi::sign(const QString &input, const Keys &keys) {
    return QString::fromLatin1(QCryptographicHash::hash((keys.pub + "&" + input.toLower() + "&" + keys.priv).toUtf8(),
                                                        QCryptographicHash::Md5).toHex());
}

IyfApi::Keys IyfApi::keys(Client *client) const {
    QMutexLocker locker(&g_keysMutex);
    Keys &keys = g_keys[m_hosts.home];
    if (keys.isEmpty()) {
        static const QRegularExpression pattern(R"("publicKey":"([^"]+)\","privateKey\":\[\"([^"]+)\")");
        const auto match = pattern.match(client->get(m_hosts.home).body);
        if (!match.hasMatch() || match.lastCapturedIndex() != 2)
            throw AppException(tr("Could not read the iyf signing keys."),
                               QStringLiteral("爱壹帆"));
        keys = {match.captured(1), match.captured(2)};
    }
    return keys;
}

void IyfApi::forgetKeys() const {
    QMutexLocker locker(&g_keysMutex);
    g_keys.remove(m_hosts.home);
}

IyfApi::Reply IyfApi::reply(const Client::Response &response) {
    const QJsonObject data = response.toJsonObject().value(QStringLiteral("data")).toObject();
    Reply reply;
    if (data.isEmpty()) return reply;
    reply.code = data.value(QStringLiteral("code")).toInt(-1);
    reply.message = data.value(QStringLiteral("msg")).toString();
    reply.info = data.value(QStringLiteral("info")).toArray();
    return reply;
}

IyfApi::Reply IyfApi::call(Client *client, const std::function<Client::Response(const Keys &)> &send) const {
    Reply last;
    for (int attempt = 0; attempt < 2; ++attempt) {
        last = reply(send(keys(client)));
        if (!last.signatureRefused()) return last;
        forgetKeys();
    }
    return last;
}

IyfApi::Reply IyfApi::get(Client *client, const QString &path, const QString &query, const QString &sent) const {
    return call(client, [&](const Keys &keys) {
        return client->get(m_hosts.api + path + QLatin1Char('?') + (sent.isEmpty() ? query : sent) + QStringLiteral("&vv=")
                           + sign(query, keys) + QStringLiteral("&pub=") + keys.pub);
    });
}

std::optional<IyfApi::SignIn> IyfApi::signIn(Client *client, const QString &token) const {
    const Reply answer = get(client, QStringLiteral("/api/user/validate"), QStringLiteral("cinema=1&token=%1&autologin=1").arg(token),
                             QStringLiteral("cinema=1&token=%1&autologin=1").arg(encoded(token)));
    const QJsonObject info = answer.info.at(0).toObject();
    const QJsonObject fresh = info.value(QStringLiteral("userToken")).toObject();
    SignIn signedIn;
    signedIn.session = {text(fresh.value(QStringLiteral("uid"))), text(fresh.value(QStringLiteral("expire"))),
                        text(fresh.value(QStringLiteral("gid"))), text(fresh.value(QStringLiteral("sign"))),
                        text(fresh.value(QStringLiteral("token")))};
    if (!answer.ok() || signedIn.session.isEmpty()) return std::nullopt;
    signedIn.autoLoginKey = text(info.value(QStringLiteral("autoLoginKey")));
    signedIn.userName = info.value(QStringLiteral("userName")).toString();
    signedIn.nickName = info.value(QStringLiteral("nickName")).toString();
    return signedIn;
}

std::optional<QString> IyfApi::tokenFor(Client *client, const QString &autoLoginKey) const {
    const Reply answer = get(client, QStringLiteral("/api/user/UserAutoLogin"), QStringLiteral("cinema=1&autologin=%1").arg(autoLoginKey),
                             QStringLiteral("cinema=1&autologin=%1").arg(encoded(autoLoginKey)));
    if (!answer.ok()) return std::nullopt;
    const QJsonValue first = answer.info.at(0);
    const QString token = first.isString() ? first.toString() : text(first.toObject().value(QStringLiteral("token")));
    return token.isEmpty() ? std::nullopt : std::optional(token);
}

std::optional<IyfApi::Session> IyfApi::renewed(Client *client, const Session &session) const {
    const std::optional<SignIn> again = signIn(client, session.token);
    return again ? std::optional(again->session) : std::nullopt;
}

IyfApi::Profile IyfApi::profile(Client *client, const Session &session) const {
    const Reply answer = get(client, QStringLiteral("/v3/user/getuserinfo"), QStringLiteral("cinema=1&") + session.query());
    Profile profile;
    const QJsonObject info = answer.info.at(0).toObject();
    if (answer.code == 3) profile.state = Standing::Elsewhere;
    // 2 and 4: no session, a lapsed sign. 0 with nothing in it is what a session the site no longer knows gets.
    else if (answer.sessionExpired() || (answer.ok() && info.isEmpty())) profile.state = Standing::Expired;
    else if (!answer.ok()) return profile;
    if (!answer.ok() || info.isEmpty()) return profile;
    profile.state = Standing::Good;
    if (!g_toldInfoShape.exchange(true)) logInfo() << "Sources" << QStringLiteral("爱壹帆:") << "account fields:" << shapeOf(info);
    Membership member;
    member.uid = text(info.value(QStringLiteral("uid")));
    member.name = info.value(QStringLiteral("nickName")).toString();
    if (member.name.isEmpty()) member.name = info.value(QStringLiteral("userName")).toString();
    member.daysLeft = int(whole(info.value(QStringLiteral("endDays")), 0));
    const QDate stated = dayOf(info.value(QStringLiteral("endDate")));
    const QDate today = QDate::currentDate();
    if (member.daysLeft != 0) {
        const QDate counted = today.addDays(member.daysLeft);
        member.until = stated.isValid() && std::abs(stated.daysTo(counted)) <= 1 ? stated : counted;
    } else if (stated.isValid() && std::abs(stated.daysTo(today)) <= 1) {
        member.until = stated;
    }
    member.tier = tierOf(info.value(QStringLiteral("vipImage")).toString());
    member.gid = int(whole(info.value(QStringLiteral("gid")), 0));
    member.level = int(whole(info.value(QStringLiteral("userLevel"))));
    member.experience = whole(info.value(QStringLiteral("experience")));
    member.nextLevel = whole(info.value(QStringLiteral("nextLevel")));
    member.coins = whole(info.value(QStringLiteral("gold")));
    if (const QDate day = dayOf(info.value(QStringLiteral("sDate"))); day.isValid()) member.since = day.toString(Qt::ISODate);
    if (const QDate day = dayOf(info.value(QStringLiteral("lastTime"))); day.isValid()) member.lastLogin = day.toString(Qt::ISODate);
    member.region = info.value(QStringLiteral("from")).toString();
    const QString bound = text(info.value(QStringLiteral("isBindTelphone")));
    member.phoneBound = info.value(QStringLiteral("isBindTelphone")).isBool() ? int(info.value(QStringLiteral("isBindTelphone")).toBool())
                        : bound.isEmpty() ? -1 : int(bound != QLatin1String("0"));
    profile.member = member;
    return profile;
}

QList<IyfApi::Permission> IyfApi::permissions(Client *client, const Session &session) const {
    const Reply answer = get(client, QStringLiteral("/api/user/getuserext"), QStringLiteral("cinema=1&") + session.query());
    QList<Permission> list;
    if (!answer.ok()) return list;
    for (const QJsonValue &value : answer.info) {
        const QJsonObject permission = value.toObject();
        QString title = permission.value(QStringLiteral("title")).toString();
        if (title.isEmpty()) title = permission.value(QStringLiteral("Title")).toString();
        if (title.isEmpty()) continue;
        const QJsonValue valid = permission.value(QStringLiteral("isValid"));
        list.append({title.simplified(), valid.toBool() || text(valid) == QLatin1String("1")});
    }
    if (!g_toldPermissionShape.exchange(true) && !answer.info.isEmpty())
        logInfo() << "Sources" << QStringLiteral("爱壹帆:") << "account permissions:" << shapeOf(answer.info.at(0).toObject());
    return list;
}

Iyf::Iyf(QObject *parent, IyfApi::Hosts hosts)
    : Provider(parent), m_api(std::move(hosts)), m_account(new IyfAccount(m_api, this)) {}

Account *Iyf::account() { return m_account; }

IyfApi::Session Iyf::session() const { return m_account->session(); }

QString Iyf::cover(const QString &image) {
    if (image.isEmpty() || image.contains(QLatin1Char('?'))) return image;
    // Some names end in an "s" (a small copy) that the server does not have.
    static const QRegularExpression small(QStringLiteral(R"((\d)s(\.\w+)$)"));
    return QString(image).replace(small, QStringLiteral("\\1\\2")) + QStringLiteral("?w=216&h=309&format=jpg&mode=stretch");
}

// The list cards use the site's own 216x309; the Details poster decodes at 480 px, so it asks for twice that.
QString Iyf::detailCover(const QString &url) const {
    return cover(url).replace(QLatin1String("?w=216&h=309&"), QLatin1String("?w=432&h=618&"));
}

// The library and history keep the address as it was fetched, including the "s" copies the server does not have.
QString Iyf::listCover(const QString &stored) const { return cover(stored); }

QList<ShowData> Iyf::search(Client *client, const QString &query, int page, int ) {
    const QString tag = QString::fromUtf8(QUrl::toPercentEncoding(query.toLower()));
    const QString url = QStringLiteral("https://rankv21.iyf.tv/v3/list/briefsearch"
                                       "?tags=%1&orderby=4&page=%2&size=36&desc=1&isserial=-1&%3")
                            .arg(tag, QString::number(page), session().query());
    const IyfApi::Reply reply = m_api.call(client, [&](const IyfApi::Keys &keys) {
        return client->post(url, {{"tag", tag}, {"vv", IyfApi::sign("tags=" + tag, keys)}, {"pub", keys.pub}}, m_headers);
    });
    const auto results = reply.info.at(0).toObject()["result"].toArray();

    QList<ShowData> shows;
    shows.reserve(results.size());
    for (const QJsonValue &value : results) {
        const QJsonObject show = value.toObject();
        shows.emplaceBack(show["title"].toString(), show["contxt"].toString(),
                          cover(show["imgPath"].toString()), this);
    }
    return shows;
}

QList<ShowData> Iyf::browse(Client *client, int page, bool latest, int typeIndex,
                            const QVariantMap &filters) {
    const int i = qBound(0, typeIndex, int(std::size(kCategoryIds)) - 1);
    const QString genre = filters.value("genre").toString();
    QString params = QStringLiteral("cinema=1&page=%1&size=36&orderby=%2&desc=1&cid=%3")
                         .arg(QString::number(page), latest ? "1" : "2",
                              genre.isEmpty() ? QLatin1String(kCategoryIds[i]) : genre);
    if (const QString year = filters.value("year").toString(); !year.isEmpty())
        params += "&year=" + year;
    const QString status = filters.value("status").toString();
    params += QStringLiteral("&isserial=%1").arg(status == "airing" ? "1" : status == "finished" ? "0" : "-1");
    const auto results = callApi(client, QStringLiteral("/api/list/Search"),
                                 params + "&isIndex=-1&isfree=-1")["result"].toArray();

    QList<ShowData> shows;
    shows.reserve(results.size());
    for (const QJsonValue &value : results) {
        const QJsonObject show = value.toObject();
        shows.emplaceBack(show["title"].toString(), show["key"].toString(),
                          cover(show["image"].toString()), this, "", kShowTypes[i]);
    }
    return shows;
}

int Iyf::loadShow(Client *client, ShowData &show, LoadParts parts) const {
    const QJsonObject info = callApi(
        client, QStringLiteral("/v3/video/detail"),
        QStringLiteral("cinema=1&device=1&player=CkPlayer&tech=HLS&country=HU&lang=cns&v=1&id=%1&region=UK")
            .arg(show.link));
    if (info.isEmpty()) return 0;

    const QString params = QStringLiteral("cinema=1&vid=%1&lsk=1&taxis=0&cid=%2&%3")
                               .arg(show.link, info["cid"].toString(), session().query());
    // Signed as it is, then sent with its commas escaped.
    const IyfApi::Reply reply = m_api.call(client, [&](const IyfApi::Keys &keys) {
        QString escaped = params;
        escaped.replace(",", "%2C");
        return client->get(m_api.hosts().api + "/v3/video/languagesplaylist?" + escaped
                           + "&vv=" + IyfApi::sign(params, keys) + "&pub=" + keys.pub);
    });
    const auto episodes = reply.info.at(0).toObject()["playList"].toArray();
    if (episodes.isEmpty()) return 0;
    if (parts.testFlag(CountOnly)) return episodes.size();

    if (parts.testFlag(Episodes)) {
        for (const QJsonValue &value : episodes) {
            const QJsonObject episode = value.toObject();
            show.addNumberedEpisode(0, episode["key"].toString(), episode["name"].toString());
        }
    }

    if (parts.testFlag(Details)) {
        show.description = info["contxt"].toString();
        show.status      = info["lastName"].toString();
        show.views       = QString::number(info["view"].toInt(-1));
        show.updateTime  = info["updateweekly"].toString();
        show.score       = info["score"].toString();
        show.releaseDate = info["add_date"].toString();
        show.genres.push_back(info["videoType"].toString());
    }
    return episodes.size();
}

Source Iyf::extractSource(Client *client, VideoServer server) {
    Source source;
    const IyfApi::Session account = session();
    const QJsonObject response = callApi(
        client, QStringLiteral("/v3/video/play"),
        QStringLiteral("cinema=1&id=%1&a=0&lang=none&usersign=1&region=UK&device=1&isMasterSupport=0&%2")
            .arg(server.link, account.query()));
    if (response.isEmpty()) return source;

    bool locked = false;
    for (const QJsonValue &value : response["clarity"].toArray()) {
        const QJsonObject clarity = value.toObject();
        if (clarity["path"].isNull()) {
            locked = locked || clarity["isVIP"].toBool();
            continue;
        }
        if (!source.videos.isEmpty()) continue;
        const QJsonObject path = clarity["path"].toObject();
        QString address = path["result"].toString();

        if (path["needSign"].toBool() || address.startsWith("https://hss5")) {
            const IyfApi::Keys keys = m_api.keys(client);
            address += QStringLiteral("&vv=%1&pub=%2").arg(IyfApi::sign(account.query(), keys), keys.pub);
        }
        // "title" is the height ("480") and "description" the site's name for it; "bitrate" is not one (480000 for 480p).
        source.videos.emplaceBack(address, clarity["description"].toString(),
                                    clarity["title"].toString().toInt());
    }
    const QJsonValue needsAccount = response["needLogin"];
    if (account.isEmpty() && (needsAccount.toBool() || needsAccount.toInt() != 0))
        MainWindow::instance().reportError(tr("This video needs an iyf account. Sign in under Settings > Accounts & sync."), name());
    else if (locked && !account.isEmpty())
        explainLockedQualities(client, account);

    // The web player asks for the comments by the play answer's uniqueKey, not the episode key.
    if (const QString uniqueKey = response["uniqueKey"].toString();
        !uniqueKey.isEmpty() && !source.videos.isEmpty()) {
        offerDanmaku(source, QStringLiteral("iyf-%1").arg(uniqueKey), {{"uniqueKey", uniqueKey}});
    }
    return source;
}

// The site hands a lapsed session the free qualities and no error, so a locked quality is the only sign.
void Iyf::explainLockedQualities(Client *client, const IyfApi::Session &account) const {
    const QString header = name();
    switch (m_api.profile(client, account).state) {
    case IyfApi::Standing::Expired:
        MainWindow::instance().reportError(tr("Your iyf session has ended, so only free qualities play. Sign in again "
                                            "under Settings > Accounts & sync."), header);
        break;
    case IyfApi::Standing::Elsewhere:
        MainWindow::instance().reportError(tr("Your iyf account was signed in elsewhere, so only free qualities play. "
                                            "Sign in again under Settings > Accounts & sync."), header);
        break;
    default:
        break;
    }
}

QList<DanmakuComment> Iyf::comments(Client *client, const QVariantMap &ids) {
    return fetchDanmaku(client, m_api, ids.value(QStringLiteral("uniqueKey")).toString());
}

QList<DanmakuComment> Iyf::fetchDanmaku(Client *client, const IyfApi &api, const QString &uniqueKey) {
    const QString query = QStringLiteral("cinema=1&page=1&size=12000&uniqueKey=%1").arg(uniqueKey);
    const IyfApi::Reply reply = api.call(client, [&](const IyfApi::Keys &keys) {
        return client->get(api.hosts().api + QStringLiteral("/api/video/getBarrage?") + query
                               + QStringLiteral("&vv=") + IyfApi::sign(query, keys) + QStringLiteral("&pub=") + keys.pub,
                           {{"referer", "https://www.iyf.tv"}, {"X-Requested-With", "XMLHttpRequest"}});
    });
    const QJsonArray items = reply.info;

    QList<DanmakuComment> comments;
    comments.reserve(items.size());
    QSet<QString> seenIds;
    int deleted = 0, again = 0;
    for (const QJsonValue &value : items) {
        const QJsonObject item = value.toObject();
        if (item["isDeleted"].toInt() != 0) {
            ++deleted;
            continue;
        }
        // "guid" is the comment's own id ("id" is always 0); the server ignores page and size, so a repeat is a duplicate.
        if (const QString guid = item["guid"].toString(); !guid.isEmpty() && seenIds.contains(guid)) {
            ++again;
            continue;
        } else if (!guid.isEmpty()) {
            seenIds.insert(guid);
        }
        DanmakuComment comment;
        comment.text = item["contxt"].toString().trimmed();
        if (comment.text.isEmpty()) continue;
        comment.timeMs = int(item["second"].toDouble() * 1000);
        const int position = item["position"].toInt();
        comment.mode = position == 1 ? 5 : position == 2 ? 4 : 1;
        bool ok = false;
        const quint32 rgb = item["color"].toString().remove(QLatin1Char('#')).toUInt(&ok, 16);
        if (ok) comment.color = rgb & 0xFFFFFFu;
        comments.append(comment);
    }
    logInfo() << "Danmaku" << QStringLiteral("iyf: %1 comments sent, %2 deleted, %3 sent twice")
                                 .arg(items.size()).arg(deleted).arg(again);
    return comments;
}

QVariantMap Iyf::filterOptions(Client *client, int typeIndex) {
    const int i = qBound(0, typeIndex, int(std::size(kCategoryIds)) - 1);
    const QJsonArray list = m_api.get(client, QStringLiteral("/api/list/") + QLatin1String(kGenreLists[i]),
                                      QStringLiteral("cinema=1")).info;
    QVariantList genres, years;
    for (const QJsonValue &genre : list)
        genres.append(filterOption(genre["path"].toString(), genre["className"].toString()));
    for (int year = QDate::currentDate().year(); year >= 2000; --year)
        years.append(filterOption(QString::number(year), QString::number(year)));
    QVariantMap options{{"genre", genres}, {"year", years}};
    if (kShowTypes[i] != ShowData::Movie) options.insert("status", statusOptions({"airing", "finished"}));
    return options;
}

QJsonObject Iyf::callApi(Client *client, const QString &path, const QString &query) const {
    return m_api.get(client, path, query).info.at(0).toObject();
}

// iyf.tv's own login page in a window with nothing kept: the user types into the site, not into this program.
// The token the page sends to /api/user/validate is seen on the way and handed on.
class IyfLogin {
public:
    // Runs on the network thread of the engine: only the token leaves it, queued to `owner`.
    class Interceptor : public QWebEngineUrlRequestInterceptor {
    public:
        Interceptor(QObject *owner, std::function<void(const QString &)> tokenSeen)
            : QWebEngineUrlRequestInterceptor(owner), m_owner(owner), m_tokenSeen(std::move(tokenSeen)) {}

        void interceptRequest(QWebEngineUrlRequestInfo &info) override {
            const QUrl url = info.requestUrl();
            // Only iyf's own hosts: another site in the page must not be able to sign this app in as someone else.
            const QString host = url.host();
            if (host != QLatin1String("iyf.tv") && !host.endsWith(QLatin1String(".iyf.tv"))) return;
            if (!url.path().endsWith(QLatin1String("/user/validate"), Qt::CaseInsensitive)) return;
            const QString token = QUrlQuery(url).queryItemValue(QStringLiteral("token"), QUrl::FullyDecoded);
            if (token.isEmpty()) return;
            QMetaObject::invokeMethod(m_owner, [fn = m_tokenSeen, token] { fn(token); }, Qt::QueuedConnection);
        }

    private:
        QObject *m_owner;
        std::function<void(const QString &)> m_tokenSeen;
    };

    IyfLogin(const QString &homeUrl, QObject *owner, std::function<void(const QString &)> tokenSeen)
        : m_home(homeUrl), m_owner(owner), m_tokenSeen(std::move(tokenSeen)) {}

    // The profile outlives the pages it made.
    ~IyfLogin() {
        delete m_view.data();
        m_profile.reset();
    }

    void show() {
        if (m_view) {
            m_view->show();
            m_view->raise();
            m_view->activateWindow();
            return;
        }
        m_profile.reset(QWebEngineProfileBuilder::createOffTheRecordProfile());
        delete m_interceptor;   // the earlier window's, its profile gone
        m_interceptor = new Interceptor(m_owner, m_tokenSeen);
        m_profile->setUrlRequestInterceptor(m_interceptor);
        auto *page = new QWebEnginePage(m_profile.get());
        page->settings()->setAttribute(QWebEngineSettings::JavascriptCanAccessClipboard, false);
        m_view = new QWebEngineView;
        m_view->setPage(page);
        m_view->setAttribute(Qt::WA_QuitOnClose, false);
        m_view->setAttribute(Qt::WA_DeleteOnClose);
        m_view->setWindowTitle(IyfAccount::tr("Sign in to iyf"));
        m_view->resize(520, 760);
        page->setParent(m_view);
        m_view->load(QUrl(m_home));
        m_view->show();
        m_view->raise();
        m_view->activateWindow();
    }

    void close() {
        if (m_view) m_view->close();
    }

private:
    QString m_home;
    QObject *m_owner;
    std::function<void(const QString &)> m_tokenSeen;
    std::unique_ptr<QWebEngineProfile> m_profile;
    Interceptor *m_interceptor = nullptr;
    QPointer<QWebEngineView> m_view;
};

IyfAccount::IyfAccount(IyfApi api, QObject *parent) : Account(Config::IyfSession, parent), m_api(std::move(api)) {
    look();
    logInfo() << "Sources" << QStringLiteral("爱壹帆:") << (signedIn() ? "account session loaded" : "no account signed in");
    QTimer::singleShot(1500, this, [this]() { refresh(); });
}

IyfAccount::~IyfAccount() {
    m_cancel.cancel();
    waitFor(m_signIn, "iyf sign-in");
    waitFor(m_check, "iyf account");
}

QString IyfAccount::hint() const {
    return tr("Sign in opens iyf's own sign-in page in a window. The password goes to iyf, not to this app. Browsing, "
              "search and free qualities work without an account.");
}

QString IyfAccount::offlineHint() const {
    return tr("Higher qualities and member videos need an account");
}

IyfApi::Session IyfAccount::session() const {
    const Fields stored = fields();
    return {stored.value("uid"), stored.value("expire"), stored.value("gid"), stored.value("sign"), stored.value("token")};
}

Account::Fields IyfAccount::fieldsOf(const IyfApi::Session &session, const QString &autoLoginKey, const QString &name) {
    return {{"uid", session.uid}, {"expire", session.expire}, {"gid", session.gid}, {"sign", session.sign},
            {"token", session.token}, {"autologin", autoLoginKey}, {"name", name}};
}

QDateTime IyfAccount::sessionEnd(const QString &expire) {
    bool numeric = false;
    const double number = expire.toDouble(&numeric);
    QDateTime end;
    if (numeric) end = QDateTime::fromSecsSinceEpoch(qint64(number > 1e11 ? number / 1000 : number));
    else end = QDateTime::fromString(expire.left(19), Qt::ISODate);
    // Only a time that is plausible: the site does not say what the value is.
    const QDateTime now = QDateTime::currentDateTime();
    return end.isValid() && end > now.addYears(-2) && end < now.addYears(30) ? end : QDateTime();
}

void IyfAccount::look() {
    const IyfApi::Session stored = session();
    Standing standing;
    standing.signedIn = !stored.isEmpty();
    standing.name = fields().value("name");
    standing.sessionEnds = sessionEnd(stored.expire);
    setStanding(standing);
}

void IyfAccount::adopted() {
    look();
    refresh();
}

void IyfAccount::signIn() {
    if (!m_login) m_login = std::make_unique<IyfLogin>(m_api.hosts().home, this, [this](const QString &token) { signInWithToken(token); });
    m_login->show();
}

void IyfAccount::signInWithToken(const QString &token) {
    if (m_signingIn || token.isEmpty()) return;
    m_signingIn = true;
    setBusy(true);
    m_signIn = QtConcurrent::run([this, token, cancel = m_cancel]() {
        Client client(cancel, false);
        client.setStrictTls(true);
        std::optional<IyfApi::SignIn> signedIn;
        try {
            signedIn = m_api.signIn(&client, token);
        } catch (const std::exception &error) {
            logWarn() << "Sources" << QStringLiteral("爱壹帆:") << "sign-in failed:" << error.what();
        }
        if (cancel.isCancelled()) return;
        QMetaObject::invokeMethod(this, [this, signedIn]() {
            m_signingIn = false;
            if (!signedIn) {
                setBusy(false);
                MainWindow::instance().reportError(tr("iyf did not accept that sign-in. Try again from the sign-in window."), label());
                return;
            }
            if (m_login) m_login->close();
            signedInWith(*signedIn);
        }, Qt::QueuedConnection);
    });
}

void IyfAccount::signedInWith(const IyfApi::SignIn &signedIn) {
    keep(fieldsOf(signedIn.session, signedIn.autoLoginKey, signedIn.nickName.isEmpty() ? signedIn.userName : signedIn.nickName));
    logInfo() << "Sources" << QStringLiteral("爱壹帆:") << "signed in";
    look();
    setBusy(false);
    MainWindow::instance().reportInfo(tr("Signed in to %1.").arg(label()), label());
    refresh();
}

void IyfAccount::refresh() {
    if (!signedIn()) return;
    if (m_checking) {
        m_recheck = true;
        return;
    }
    m_checking = true;
    setBusy(true);
    const Fields stored = fields();
    const IyfApi::Session current = session();
    m_check = QtConcurrent::run([this, stored, current, cancel = m_cancel]() {
        Client client(cancel, false);
        client.setStrictTls(true);
        Standing standing;
        standing.signedIn = true;
        standing.name = stored.value("name");
        standing.checkedAt = QDateTime::currentDateTime();
        std::optional<IyfApi::SignIn> renewed;
        IyfApi::Session session = current;
        IyfApi::Profile profile;
        QList<IyfApi::Permission> permissions;
        bool heard = false, partial = false;

        try {
            profile = m_api.profile(&client, session);
            heard = true;
            if (profile.state == IyfApi::Standing::Expired || profile.state == IyfApi::Standing::Elsewhere) {
                std::optional<QString> token;
                if (!stored.value("autologin").isEmpty()) token = m_api.tokenFor(&client, stored.value("autologin"));
                if (token) renewed = m_api.signIn(&client, *token);
                if (!renewed) renewed = m_api.signIn(&client, session.token);
                if (renewed) {
                    session = renewed->session;
                    const IyfApi::Profile again = m_api.profile(&client, session);
                    if (again.state == IyfApi::Standing::Good) profile = again;
                    else renewed.reset();
                }
            }
            // The permissions call checks the sign but not the age: it can say yes to a session the profile call refuses, and the videos play.
            permissions = m_api.permissions(&client, session);
            partial = profile.state != IyfApi::Standing::Good
                      && std::any_of(permissions.begin(), permissions.end(), [](const IyfApi::Permission &p) { return p.valid; });
        } catch (const std::exception &error) {
            logWarn() << "Sources" << QStringLiteral("爱壹帆:") << "check failed:" << error.what();
            heard = false;
        }

        switch (heard ? profile.state : IyfApi::Standing::Unknown) {
        case IyfApi::Standing::Good: {
            const IyfApi::Membership &member = *profile.member;
            standing.asked = true;
            standing.member = member.member();
            standing.plan = member.member() ? (member.tier.isEmpty() ? QStringLiteral("VIP") : member.tier) : QString();
            standing.lifetime = member.member() && (member.daysLeft >= 36500 || member.until.year() >= 2099);
            standing.until = standing.lifetime ? QDate() : member.until;
            if (!member.name.isEmpty()) standing.name = member.name;
            standing.details = detailsOf(&member, session.uid, permissions, !(renewed ? renewed->autoLoginKey : stored.value("autologin")).isEmpty());
            break;
        }
        case IyfApi::Standing::Elsewhere:
        case IyfApi::Standing::Expired:
            if (partial) {
                standing.caution = tr("The session has ended but still unlocks VIP videos. Sign in again to see "
                                      "account details.");
                standing.details = detailsOf(nullptr, session.uid, permissions, !stored.value("autologin").isEmpty());
            } else {
                standing.problem = profile.state == IyfApi::Standing::Elsewhere ? tr("The account was signed in elsewhere.")
                                                                                : tr("The session has ended.");
            }
            break;
        case IyfApi::Standing::Unknown:
            standing.problem = tr("iyf did not respond, so the account could not be checked.");
            break;
        }
        const bool silent = !standing.asked && standing.problem == tr("iyf did not respond, so the account could not be checked.");
        // "expire" is a time only when it reads as one; a past one on a session the site accepts says nothing.
        if (const QDateTime ends = sessionEnd(session.expire); ends > QDateTime::currentDateTime() || !standing.problem.isEmpty()
                                                                || !standing.caution.isEmpty())
            standing.sessionEnds = ends;
        if (cancel.isCancelled()) return;
        QMetaObject::invokeMethod(this, [this, standing, renewed, stored, silent]() {
            m_checking = false;
            if (fields() != stored) {
                // Pasted, edited or signed out meanwhile: what was asked about is not the session any more.
                setBusy(false);
                m_recheck = false;
                QTimer::singleShot(0, this, [this]() { refresh(); });
                return;
            }
            Fields again = stored;
            bool changed = false;
            if (renewed) {
                again = fieldsOf(renewed->session, renewed->autoLoginKey.isEmpty() ? stored.value("autologin") : renewed->autoLoginKey,
                                 stored.value("name"));
                changed = true;
                logInfo() << "Sources" << QStringLiteral("爱壹帆:") << "the session was renewed";
            }
            if (standing.asked && !standing.name.isEmpty() && standing.name != again.value("name")) {
                again.insert("name", standing.name);
                changed = true;
            }
            if (changed) keep(again);
            const QString said = !standing.problem.isEmpty() ? standing.problem : standing.caution;
            if (!said.isEmpty() && said != this->standing().problem && said != this->standing().caution)
                logWarn() << "Sources" << QStringLiteral("爱壹帆:") << said;
            Standing shown = standing;
            if (silent && this->standing().asked) {
                shown = this->standing();
                shown.problem = standing.problem;
                shown.checkedAt = standing.checkedAt;
            }
            setStanding(shown);
            setBusy(false);
            if (std::exchange(m_recheck, false)) QTimer::singleShot(0, this, [this]() { refresh(); });
        }, Qt::QueuedConnection);
    });
}

QList<QPair<QString, QString>> IyfAccount::detailsOf(const IyfApi::Membership *member, const QString &uid, const QList<IyfApi::Permission> &permissions, bool renews) {
    QList<QPair<QString, QString>> rows;
    const auto add = [&rows](const QString &label, const QString &value) { if (!value.isEmpty()) rows.append({label, value}); };
    add(tr("User ID"), member ? member->uid : uid);
    if (member) {
        add(tr("Tier"), member->tier);
        if (!member->member()) add(tr("Quality"), tr("Higher qualities need an iyf membership, and this account has none."));
        if (member->level >= 0)
            add(tr("Level"), member->experience >= 0 && member->nextLevel > 0
                                 ? tr("Lv %1 · %2 / %3 exp").arg(member->level).arg(member->experience).arg(member->nextLevel)
                                 : tr("Lv %1").arg(member->level));
        if (member->coins >= 0) add(tr("Coins"), QString::number(member->coins));
    }
    QStringList unlocked, locked;
    for (const IyfApi::Permission &permission : permissions) (permission.valid ? unlocked : locked) << permission.title;
    if (!unlocked.isEmpty()) add(tr("Unlocked (%1)").arg(unlocked.size()), unlocked.join(QStringLiteral("、")));
    if (!locked.isEmpty()) add(tr("Locked (%1)").arg(locked.size()), locked.join(QStringLiteral("、")));
    if (member) {
        add(tr("Member since"), member->since);
        add(tr("Last sign-in"), member->lastLogin);
        add(tr("Region"), member->region);
        if (member->phoneBound >= 0) add(tr("Phone bound"), member->phoneBound ? tr("Yes") : tr("No"));
    }
    add(tr("Renewal"), renews ? tr("Automatic: sign-in key stored") : tr("Manual: sign in again when the session ends"));
    return rows;
}

void IyfAccount::signOut() {
    m_cancel.cancel();
    m_cancel = CancelToken{};
    if (m_login) m_login->close();
    Account::signOut();
}

