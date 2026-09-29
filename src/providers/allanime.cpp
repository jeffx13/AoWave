#include "providers/allanime.h"
#include "core/exception.h"
#include "core/logger.h"
#include "core/settings.h"
#include "net/cloudflare.h"
#include <QDateTime>
#include <QFileInfo>
#include <QMap>
#include <QMessageAuthenticationCode>
#include <QMutex>
#include <QSet>
#include <QUrl>
#include <QUrlQuery>
#include <QCryptographicHash>
#include <QRegularExpression>
#include "platform/crypto.h"
#include "net/jsunpack.h"

namespace {
constexpr const char *kSearchHash  = "a24c500a1b765c68ae1d8dd85174931f661c71369c89b92b88b75a725afc471c";
constexpr const char *kPopularHash = "60f50b84bb545fa25ee7f7c8c0adbf8f5cea40f7b1ef8501cbbff70e38589489";
constexpr const char *kShowHash    = "043448386c7a686bc2aabfbb6b80f6074e795d350df48015023b079527b0848a";
constexpr const char *kEpisodeHash = "d405d0edd690624b66baba3068e0edc3ac90f1597d898a1ec8db4e5c43c00fec";

QByteArray b64urlDecode(const QString &s) {
    return QByteArray::fromBase64(s.toLatin1(), QByteArray::Base64UrlEncoding);
}

// Episode sources come from mkissa's API, which wants each request signed with a key that
// changes every few days and a scheme whose constants change every few weeks. The constants
// below are the scheme as last seen; when the API rejects them, captureSigning() reads the
// current ones off mkissa's own web client. See github.com/serplay/anidoku, whose port this is.
constexpr const char *kSigningKey = "allanime/signing";
constexpr const char *kBakedSigning = R"({
    "build_id": "176",
    "api_url": "https://api.mkissa.net/api",
    "bootstrap_url": "https://api.mkissa.net/client-crypto/v1/bootstrap",
    "referer": "https://mkissa.to",
    "referer_host": "mkissa.to",
    "key_group": "mkissa",
    "episode_lane": "k7",
    "qd_mask_hex": "03872f24d1c2938aeeb22fd115eebdec78dfa0e676b022126256ce13b650529d",
    "epoch_bucket_ms": 604800000,
    "boot_label": "MKoylY2Snz:",
    "boot_sig_template": "{build_id}/{lane}/{referer_host}/{key_group}/{epoch}"
})";
constexpr const char *kEpisodeQuery =
    "query($showId:String!,$translationType:VaildTranslationTypeEnumType!,$episodeString:String!)"
    "{episode(showId:$showId translationType:$translationType episodeString:$episodeString)"
    "{sourceUrls show{_id}}}";
constexpr const char *kRequestSeed = "{epoch}:{build_id}:{qh}:{ts}:{lane}";
// Any long-lived show: its page bootstraps the signing as it loads.
constexpr const char *kCapturePage = "/anime/ReooPAxPMsHM4KPMY";

// Records the HMAC keys and signatures the page computes, and its bootstrap request and
// answer. Runs before the page's own scripts, so it sees their first use of WebCrypto.
constexpr const char *kCaptureHook = R"JS((function(){
var c=window.__aaSigning={signs:[]};var keys=new WeakMap();
function bytes(b){return ArrayBuffer.isView(b)?new Uint8Array(b.buffer,b.byteOffset,b.byteLength):new Uint8Array(b);}
function hex(b){return Array.from(bytes(b),function(x){return x.toString(16).padStart(2,'0');}).join('');}
var S=SubtleCrypto.prototype,importKey=S.importKey,sign=S.sign;
S.importKey=function(format,raw,alg){var p=importKey.apply(this,arguments);
 try{var n=typeof alg==='string'?alg:alg&&alg.name;
  if(format==='raw'&&n==='HMAC'){var h=hex(raw);p.then(function(k){keys.set(k,h);},function(){});}}catch(e){}
 return p;};
S.sign=function(alg,key,data){
 try{c.signs.push({key:keys.get(key)||'',data:new TextDecoder().decode(bytes(data))});}catch(e){}
 return sign.apply(this,arguments);};
function isBoot(u){return String(u).indexOf('/client-crypto/')>=0;}
var fetch0=window.fetch;
window.fetch=function(input,init){var p=fetch0.apply(this,arguments);
 try{var u=typeof input==='string'?input:input instanceof URL?input.href:input.url;
  if(isBoot(u)){var h={};new Headers((init&&init.headers)||(input&&input.headers)||{}).forEach(function(v,k){h[k]=v;});
   c.boot={url:new URL(u,location.href).href,headers:h};
   p.then(function(r){if(r.ok)return r.clone().json().then(function(j){c.answer=j;});}).catch(function(){});}}catch(e){}
 return p;};
var X=XMLHttpRequest.prototype,open=X.open,setHeader=X.setRequestHeader,send=X.send;
X.open=function(m,u){this.__aa={url:String(u),headers:{}};return open.apply(this,arguments);};
X.setRequestHeader=function(k,v){if(this.__aa)this.__aa.headers[String(k).toLowerCase()]=v;return setHeader.apply(this,arguments);};
X.send=function(){var x=this,a=x.__aa;
 try{if(a&&isBoot(a.url)){c.boot={url:new URL(a.url,location.href).href,headers:a.headers};
  x.addEventListener('load',function(){if(x.status===200)try{c.answer=JSON.parse(x.responseText);}catch(e){}});}}catch(e){}
 return send.apply(this,arguments);};
})();)JS";
constexpr const char *kCaptureProbe =
    "(function(){var c=window.__aaSigning;return c&&c.boot&&c.answer?JSON.stringify("
    "{origin:location.origin,boot:c.boot,answer:c.answer,signs:c.signs}):'';})()";

QMutex g_signingMutex;
QJsonObject g_signing;   // seeded by the constructor, on the GUI thread
qint64 g_lastCaptureFailedMs = 0;

QByteArray hmac(const QByteArray &key, const QByteArray &data) {
    return QMessageAuthenticationCode::hash(data, key, QCryptographicHash::Sha256);
}

QString fill(QString pattern, const QMap<QString, QString> &fields) {
    for (auto it = fields.constBegin(); it != fields.constEnd(); ++it)
        pattern.replace(QLatin1Char('{') + it.key() + QLatin1Char('}'), it.value());
    return pattern;
}

QJsonObject signingConfig() {
    QMutexLocker lock(&g_signingMutex);
    return g_signing;
}

// Providers run on workers, so the write goes to Settings on its own thread.
void saveSigningConfig(const QJsonObject &config) {
    {
        QMutexLocker lock(&g_signingMutex);
        g_signing = config;
    }
    const QString text = QString::fromUtf8(QJsonDocument(config).toJson(QJsonDocument::Compact));
    QMetaObject::invokeMethod(&Settings::instance(), [text]() {
        Settings::instance().setValue(QLatin1String(kSigningKey), text);
    }, Qt::QueuedConnection);
}

QMap<QString, QString> signingHeaders(const QJsonObject &config, const QString &userAgent) {
    return {{"Origin", config["referer"].toString()},
            {"Referer", config["referer"].toString() + QLatin1Char('/')},
            {"User-Agent", userAgent},
            {"x-build-id", config["build_id"].toString()}};
}

struct Boot {
    qint64 epoch = 0;
    QByteArray key;      // empty when refused
    bool refused = false; // by the server, rather than lost on the way
    QString error;
};

// The epoch key, as partB XOR the mask. The request is signed with an epoch the client works
// out itself; early in a bucket the previous one is tried first, as the web client does.
Boot bootstrap(Client *client, const QJsonObject &config, const QString &userAgent) {
    Boot boot;
    const QByteArray mask = QByteArray::fromHex(config["qd_mask_hex"].toString().toLatin1());
    const qint64 bucket = config["epoch_bucket_ms"].toInteger();
    if (mask.size() != 32 || bucket <= 0) {
        boot.refused = true;
        boot.error = QStringLiteral("the saved signing constants are malformed");
        return boot;
    }
    const QString buildId = config["build_id"].toString();
    const QString lane = config["episode_lane"].toString();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 current = now / bucket;
    QList<qint64> epochs;
    if (current > 0 && now - current * bucket < 86400000) epochs << current - 1;
    epochs << current;

    const QByteArray inner = hmac(mask, (config["boot_label"].toString() + buildId).toUtf8());
    for (const qint64 epoch : std::as_const(epochs)) {
        const QString signature = fill(config["boot_sig_template"].toString(),
                                       {{"key_group", config["key_group"].toString()},
                                        {"lane", lane},
                                        {"epoch", QString::number(epoch)},
                                        {"referer_host", config["referer_host"].toString()},
                                        {"build_id", buildId}});
        auto headers = signingHeaders(config, userAgent);
        headers["x-aa-boot"] = QString::fromLatin1(hmac(inner, signature.toUtf8()).toHex());
        const auto response = client->get(config["bootstrap_url"].toString(), headers,
                                          {{"buildId", buildId}, {"k", lane}});
        if (response.code != 200) {
            boot.refused = response.code >= 400 && response.code < 500;
            boot.error = response.code > 0 ? QStringLiteral("HTTP %1 %2").arg(response.code)
                                                 .arg(response.body.left(200)).trimmed()
                                           : response.error;
            if (!boot.refused) return boot;
            continue;
        }
        const QJsonObject answer = response.toJsonObject();
        const QByteArray partB = QByteArray::fromBase64(answer["partB"].toString().toLatin1());
        if (partB.size() != 32) {
            boot.refused = true;
            boot.error = QStringLiteral("the bootstrap answer has no 32-byte partB");
            return boot;
        }
        boot.key.resize(32);
        for (int i = 0; i < 32; ++i) boot.key[i] = char(partB[i] ^ mask[i]);
        boot.epoch = answer["epoch"].toInteger();
        return boot;
    }
    return boot;
}

// aaReq: AES-GCM over a small claim about the query, with a nonce derived from the same claim.
QString signRequest(const QJsonObject &config, const Boot &boot, const QString &queryHash) {
    const QString ts = QString::number(QDateTime::currentMSecsSinceEpoch() / 300000 * 300000);
    const QString epoch = QString::number(boot.epoch);
    const QString buildId = config["build_id"].toString();
    const QString lane = config["episode_lane"].toString();
    const QByteArray claim =
        QStringLiteral(R"({"v":1,"ts":%1,"epoch":%2,"buildId":"%3","qh":"%4","k":"%5"})")
            .arg(ts, epoch, buildId, queryHash, lane).toUtf8();
    const QString seed = fill(QString::fromLatin1(kRequestSeed),
                              {{"epoch", epoch}, {"build_id", buildId}, {"qh", queryHash},
                               {"ts", ts}, {"lane", lane}});
    const QByteArray nonce =
        QCryptographicHash::hash(seed.toUtf8(), QCryptographicHash::Sha256).left(12);
    return QString::fromLatin1((QByteArray(1, '\x01') + nonce
                                + Aes::gcmEncrypt(boot.key, nonce, claim)).toBase64());
}

// version[1] | nonce[12] | ciphertext | tag[16], read as CTR from counter 2 like ani-cli does.
QJsonObject decryptSources(const QString &payload, const QByteArray &key) {
    const QByteArray raw = QByteArray::fromBase64(payload.toLatin1());
    if (raw.size() <= 1 + 12 + 16) return {};
    const QByteArray counter = raw.mid(1, 12) + QByteArray::fromHex("00000002");
    return QJsonDocument::fromJson(Aes::ctr(key, counter, raw.mid(13, raw.size() - 13 - 16)))
        .object();
}

// Works the signing constants out of what the web client did: the inner HMAC's key is the mask
// and its data is label + build id; the outer HMAC's data is the signature, whose one field
// that is not otherwise known is the key group. Empty unless the chain reproduces the
// x-aa-boot header the client sent.
QJsonObject deriveSigning(const QJsonObject &captured, const QJsonObject &current) {
    const QJsonObject boot = captured["boot"].toObject();
    const QJsonObject headers = boot["headers"].toObject();
    const QJsonObject answer = captured["answer"].toObject();
    const QUrl bootUrl(boot["url"].toString());
    const QUrlQuery query(bootUrl);
    const QString buildId = headers["x-build-id"].toString(query.queryItemValue("buildId"));
    const QString lane = query.queryItemValue("k");
    const QByteArray sent = headers["x-aa-boot"].toString().toLatin1();
    const QString epoch = QString::number(answer["epoch"].toInteger());
    const QUrl origin(captured["origin"].toString());
    QString host = origin.host();
    if (host.startsWith(QLatin1String("www."))) host.remove(0, 4);
    if (buildId.isEmpty() || lane.isEmpty() || sent.isEmpty() || host.isEmpty()) return {};

    const QJsonArray signs = captured["signs"].toArray();
    for (const QJsonValue &innerValue : signs) {
        const QJsonObject inner = innerValue.toObject();
        const QByteArray mask = QByteArray::fromHex(inner["key"].toString().toLatin1());
        if (mask.size() != 32) continue;
        const QString innerData = inner["data"].toString();
        const QByteArray chained = hmac(mask, innerData.toUtf8());
        for (const QJsonValue &outerValue : signs) {
            const QJsonObject outer = outerValue.toObject();
            const QString signature = outer["data"].toString();
            if (QByteArray::fromHex(outer["key"].toString().toLatin1()) != chained
                || hmac(chained, signature.toUtf8()).toHex() != sent)
                continue;
            if (!innerData.endsWith(buildId)) return {};

            const QList<QPair<QString, QString>> known{
                {"build_id", buildId}, {"epoch", epoch}, {"lane", lane}, {"referer_host", host}};
            // Fields and their order both change between builds, and so does the separator
            // (':' until build 166, '/' in 176), so whatever lies between fields is kept as is.
            static const QRegularExpression token(QStringLiteral("[A-Za-z0-9._-]+"));
            QString sigTemplate;
            QStringList unknown;
            qsizetype last = 0;
            for (auto it = token.globalMatch(signature); it.hasNext();) {
                const auto match = it.next();
                const QString part = match.captured();
                sigTemplate += signature.mid(last, match.capturedStart() - last);
                last = match.capturedEnd();
                auto field = std::find_if(known.begin(), known.end(),
                                          [&](const auto &k) { return k.second == part; });
                if (field != known.end()) {
                    sigTemplate += QLatin1Char('{') + field->first + QLatin1Char('}');
                } else {
                    unknown << part;
                    sigTemplate += QStringLiteral("{key_group}");
                }
            }
            sigTemplate += signature.mid(last);
            for (const auto &field : known)
                if (!sigTemplate.contains(QLatin1Char('{') + field.first + QLatin1Char('}')))
                    return {};
            if (unknown.size() != 1) return {};

            QJsonObject config = current;
            config["build_id"] = buildId;
            config["episode_lane"] = lane;
            config["qd_mask_hex"] = QString::fromLatin1(mask.toHex());
            config["boot_label"] = innerData.chopped(buildId.size());
            config["boot_sig_template"] = sigTemplate;
            config["key_group"] = unknown.first();
            config["referer"] = origin.toString(QUrl::RemovePath);
            config["referer_host"] = host;
            config["api_url"] = bootUrl.toString(QUrl::RemovePath | QUrl::RemoveQuery) + "/api";
            config["bootstrap_url"] = bootUrl.toString(QUrl::RemoveQuery);
            if (const qint64 bucket = answer["epochMs"].toInteger(); bucket > 0)
                config["epoch_bucket_ms"] = bucket;
            return config;
        }
    }
    return {};
}

// Loads a show page in the web engine and derives the constants from what its client does.
// A failure is remembered for ten minutes, so every episode click does not wait on it again.
QJsonObject captureSigning(Client *client, const QJsonObject &current) {
    {
        QMutexLocker lock(&g_signingMutex);
        if (QDateTime::currentMSecsSinceEpoch() - g_lastCaptureFailedMs < 10 * 60 * 1000)
            return {};
    }
    const QUrl page(current["referer"].toString() + QLatin1String(kCapturePage));
    logInfo() << "AllAnime" << "reading the current request signing off" << page.toString();
    const QString captured = Cloudflare::captureInBrowser(
        page, QString::fromLatin1(kCaptureHook), QString::fromLatin1(kCaptureProbe),
        client->cancelToken(), 30000, QStringLiteral("AllAnime"));
    const QJsonObject derived =
        deriveSigning(QJsonDocument::fromJson(captured.toUtf8()).object(), current);
    if (derived.isEmpty()) {
        logWarn() << "AllAnime" << "could not derive the request signing from" << page.toString()
                  << (captured.isEmpty() ? "(nothing captured)" : captured.left(400));
        if (!client->isCancelled()) {
            QMutexLocker lock(&g_signingMutex);
            g_lastCaptureFailedMs = QDateTime::currentMSecsSinceEpoch();
        }
    }
    return derived;
}

// Filemoon (Fm-mp4): AES-256-CTR JSON {iv, payload, key_parts}.
void extractFilemoon(const QJsonObject &json, PlayInfo &playItem) {
    const auto keyParts = json["key_parts"].toArray();
    if (keyParts.size() < 2 || !json.contains("payload")) return;

    QByteArray key     = b64urlDecode(keyParts[0].toString()) + b64urlDecode(keyParts[1].toString());
    QByteArray counter = b64urlDecode(json["iv"].toString());
    counter.append(QByteArray::fromHex("00000002"));
    QByteArray payload = b64urlDecode(json["payload"].toString());
    if (key.size() != 32 || counter.size() != 16 || payload.size() <= 16) return;
    payload.chop(16);   // CTR ignores the trailing auth tag.

    QString text = QString::fromUtf8(Aes::ctr(key, counter, payload));
    if (text.isEmpty()) return;
    text.replace("\\u0026", "&").replace("\\u003D", "=").replace("\\/", "/");

    static const QRegularExpression re(
        R"RX("url":"([^"]+)"[^}]*?"height":(\d+)|"height":(\d+)[^}]*?"url":"([^"]+)")RX");
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        const auto m = it.next();
        const QString url = !m.captured(1).isEmpty() ? m.captured(1) : m.captured(4);
        const QString h   = !m.captured(2).isEmpty() ? m.captured(2) : m.captured(3);
        if (!url.isEmpty())
            playItem.videos.emplaceBack(url, h.isEmpty() ? QString() : h + "p", h.toInt());
    }
}

// ok.ru: flashvars.metadata. Signed urls are IP+UA-bound.
void parseOkRu(const QString &page, PlayInfo &playItem, const QString &userAgent) {
    static const QRegularExpression re(QStringLiteral("data-options=\"([^\"]+)\""));
    const auto m = re.match(page);
    if (!m.hasMatch()) return;

    QString opts = m.captured(1);
    opts.replace("&quot;", "\"").replace("&amp;", "&").replace("&#39;", "'")
        .replace("&lt;", "<").replace("&gt;", ">");

    const QJsonObject flashvars = QJsonDocument::fromJson(opts.toUtf8())
                                      .object().value("flashvars").toObject();
    const QJsonValue metaVal = flashvars.value("metadata");
    const QJsonObject meta = metaVal.isString()
        ? QJsonDocument::fromJson(metaVal.toString().toUtf8()).object()
        : metaVal.toObject();
    if (meta.isEmpty()) return;

    QString hls = meta.value("hlsManifestUrl").toString();
    if (hls.isEmpty()) hls = meta.value("ondemandHls").toString();
    if (hls.isEmpty()) hls = meta.value("hlsMasterPlaylistUrl").toString();
    if (!hls.isEmpty()) {
        playItem.videos.emplaceBack(hls);
    } else {
        for (const QJsonValue &v : meta.value("videos").toArray()) {
            const QJsonObject o = v.toObject();
            const QString url = o.value("url").toString();
            if (!url.isEmpty()) playItem.videos.emplaceBack(url, o.value("name").toString());
        }
    }
    if (!playItem.videos.isEmpty() && !userAgent.isEmpty())
        playItem.addHeader("user-agent", userAgent);
}

// byse: key = key_parts[version] + key_parts[31-version].
void parseByse(const QJsonObject &resp, PlayInfo &playItem, const QString &userAgent) {
    const QJsonObject pb = resp.value("playback").toObject();
    const QJsonArray keyParts = pb.value("key_parts").toArray();
    const int n = keyParts.size();
    if (n == 0) return;

    const int version = pb.value("version").toString().toInt();
    const int i = version, s = 31 - version;
    QByteArray key;
    if (i >= 1 && i <= n && s >= 1 && s <= n) {
        key = b64urlDecode(keyParts[i - 1].toString()) + b64urlDecode(keyParts[s - 1].toString());
    } else {
        for (const QJsonValue &p : keyParts) key += b64urlDecode(p.toString());
    }

    const QByteArray plain = Aes::gcmDecrypt(key, b64urlDecode(pb.value("iv").toString()),
                                                b64urlDecode(pb.value("payload").toString()));
    if (plain.isEmpty()) return;

    const QJsonObject obj = QJsonDocument::fromJson(plain).object();
    for (const QJsonValue &v : obj.value("sources").toArray()) {
        const QJsonObject src = v.toObject();
        const QString url = src.value("url").toString();
        if (!url.isEmpty())
            playItem.videos.emplaceBack(url, src.value("label").toString(), src.value("height").toInt());
    }
    for (const QJsonValue &v : obj.value("tracks").toArray()) {
        const QJsonObject tr = v.toObject();
        const QString url = tr.value("url").toString();
        if (!url.isEmpty()) playItem.subtitles.emplaceBack(url, tr.value("title").toString());
    }
    if (!playItem.videos.isEmpty() && !userAgent.isEmpty())
        playItem.addHeader("user-agent", userAgent);
}
}

AllAnime::AllAnime(QObject *parent) : ShowProvider(parent) {
    setPreferredServer("Luf-mp4");
    // Whichever build is newer: a capture saved since the last release, or the release's own.
    const QJsonObject baked = QJsonDocument::fromJson(kBakedSigning).object();
    const QJsonObject saved = QJsonDocument::fromJson(
        Settings::instance().value(QLatin1String(kSigningKey)).toString().toUtf8()).object();
    QMutexLocker lock(&g_signingMutex);
    g_signing = saved["build_id"].toString().toInt() > baked["build_id"].toString().toInt()
                    ? saved : baked;
}

QList<ShowData> AllAnime::search(Client *client, const QString &query, int page, int /*typeIndex*/) {
    QString variables = QString(
                            "{%22search%22:{%22query%22:%22%1%22},%22limit%22:26,%22page%22:%2"
                            ",%22translationType%22:%22sub%22,%22countryOrigin%22:%22ALL%22}")
                            .arg(QUrl::toPercentEncoding(query), QString::number(page));

    auto data = client->get(apiUrl(variables, kSearchHash), m_headers)
                    .toJsonObject()["data"].toObject();
    return parseJsonArray(data["shows"].toObject()["edges"].toArray());
}

QList<ShowData> AllAnime::popular(Client *client, int page, int /*typeIndex*/) {
    QString variables = QString(
                            "{%22type%22:%22anime%22,%22size%22:20,%22dateRange%22:0,%22page%22:%1"
                            ",%22allowAdult%22:false,%22allowUnknown%22:false}")
                            .arg(page);

    auto data = client->get(apiUrl(variables, kPopularHash), m_headers)
                    .toJsonObject()["data"].toObject();
    return parseJsonArray(data["queryPopular"].toObject()["recommendations"].toArray(), true);
}

QList<ShowData> AllAnime::latest(Client *client, int page, int /*typeIndex*/) {
    QString variables = QString(
                            "{%22search%22:{},%22limit%22:26,%22page%22:%1"
                            ",%22translationType%22:%22sub%22,%22countryOrigin%22:%22JP%22}")
                            .arg(page);

    auto data = client->get(apiUrl(variables, kSearchHash), m_headers)
                    .toJsonObject()["data"].toObject();
    return parseJsonArray(data["shows"].toObject()["edges"].toArray());
}

QVariantMap AllAnime::filterOptions(Client * /*client*/, int /*typeIndex*/) {
    // mkissa.to's anime genres, as its search page offers them.
    static const QStringList kGenres = {
        "Action", "Adventure", "Cars", "Comedy", "Dementia", "Demons", "Drama", "Ecchi", "Fantasy",
        "Game", "Harem", "Historical", "Horror", "Isekai", "Josei", "Kids", "Magic", "Martial Arts",
        "Mecha", "Military", "Music", "Mystery", "Parody", "Police", "Psychological", "Romance",
        "Samurai", "School", "Sci-Fi", "Seinen", "Shoujo", "Shoujo Ai", "Shounen", "Shounen Ai",
        "Slice of Life", "Space", "Sports", "Super Power", "Supernatural", "Thriller", "Vampire",
        "Yaoi", "Yuri"};
    QVariantList genres, years;
    for (const QString &genre : kGenres) genres.append(filterOption(genre, genre));
    for (int year = QDate::currentDate().year() + 1; year >= 1970; --year)
        years.append(filterOption(QString::number(year), QString::number(year)));
    return {{"genre", genres}, {"year", years}};
}

QList<ShowData> AllAnime::filtered(Client *client, const QString &query, int page, int /*typeIndex*/,
                                   const QVariantMap &filters, bool latest) {
    QJsonObject search{{"allowAdult", false}, {"allowUnknown", false}};
    // The site drops the words from a sorted search, so words go unsorted.
    if (!query.trimmed().isEmpty()) search["query"] = query;
    else search["sortBy"] = latest ? "Latest_Update" : "Popular";
    if (const QString genre = filters.value("genre").toString(); !genre.isEmpty())
        search["genres"] = QJsonArray{genre};
    if (const int year = filters.value("year").toInt(); year > 0) search["year"] = year;
    const QJsonObject variables{{"search", search}, {"limit", 26}, {"page", page},
                                {"translationType", "sub"}, {"countryOrigin", "ALL"}};
    const QString encoded = QString::fromLatin1(
        QUrl::toPercentEncoding(QString::fromUtf8(QJsonDocument(variables).toJson(QJsonDocument::Compact))));
    auto data = client->get(apiUrl(encoded, kSearchHash), m_headers).toJsonObject()["data"].toObject();
    return parseJsonArray(data["shows"].toObject()["edges"].toArray());
}

QList<ShowData> AllAnime::parseJsonArray(const QJsonArray &shows, bool isPopular) {
    QList<ShowData> results;
    results.reserve(shows.size());

    for (const QJsonValue &val : shows) {
        QJsonObject item = val.toObject();
        if (item.isEmpty()) continue;
        if (isPopular) item = item["anyCard"].toObject();

        QString title = item["name"].toString();
        QString link = item["_id"].toString();
        if (title.isEmpty() && link.isEmpty()) continue;

        results.emplaceBack(title, link, coverImage(item), this, "", ShowData::Anime);
    }
    return results;
}

int AllAnime::loadShow(Client *client, ShowData &show, LoadParts parts) const {
    QString variables = QString("{%22_id%22:%22%1%22}").arg(show.link);
    auto json = client->get(apiUrl(variables, kShowHash), m_headers)
                    .toJsonObject()["data"].toObject()["show"].toObject();
    if (json.isEmpty()) return 0;

    QJsonArray subEps = json["availableEpisodesDetail"].toObject()["sub"].toArray();
    QJsonArray dubEps = json["availableEpisodesDetail"].toObject()["dub"].toArray();

    // Must match the sub+dub merge below.
    QSet<float> unique;
    unique.reserve(subEps.size() + dubEps.size());
    for (const QJsonValue &v : std::as_const(subEps)) unique.insert(v.toString().toFloat());
    for (const QJsonValue &v : std::as_const(dubEps)) unique.insert(v.toString().toFloat());
    const int episodeCount = unique.size();
    if (parts.testFlag(CountOnly)) return episodeCount;

    if (parts.testFlag(Episodes)) {
        QMap<float, QPair<QString, QString>> episodeMap;

        for (const QJsonValue &v : std::as_const(subEps)) {
            QString ep = v.toString();
            float num = ep.toFloat();
            episodeMap[num].first = QString(R"({"showId":"%1","translationType":"sub","episodeString":"%2"})").arg(show.link, ep);
        }
        for (const QJsonValue &v : std::as_const(dubEps)) {
            QString ep = v.toString();
            float num = ep.toFloat();
            episodeMap[num].second = QString(R"({"showId":"%1","translationType":"dub","episodeString":"%2"})").arg(show.link, ep);
        }

        for (auto it = episodeMap.constBegin(); it != episodeMap.constEnd(); ++it) {
            const auto &[sub, dub] = it.value();
            QString vars;
            if (!sub.isEmpty()) vars = sub;
            if (!dub.isEmpty()) {
                if (!vars.isEmpty()) vars += ";";
                vars += dub;
            }
            show.addEpisode(0, it.key(), vars, "");
        }
    }

    if (!parts.testFlag(Details)) return episodeCount;

    show.description = json["description"].toString();
    show.status = json["status"].toString();
    show.views = json["pageStatus"].toObject()["views"].toString();
    show.coverUrl = coverImage(json);

    QJsonValue malScore = json["score"];
    QJsonValue aniScore = json["averageScore"];
    QStringList scores;
    if (!malScore.isUndefined() && !malScore.isNull())
        scores << QString::number(malScore.toDouble(), 'f', 1) + " (MAL)";
    if (!aniScore.isUndefined() && !aniScore.isNull())
        scores << QString::number(aniScore.toInt()) + " (Anilist)";
    show.score = scores.join("; ");

    for (const QJsonValue &g : json["genres"].toArray())
        show.genres.push_back(g.toString());

    QJsonObject aired = json["airedStart"].toObject();
    int day   = aired["date"].toInt(-1);
    int month = aired["month"].toInt(-1) + 1;
    int year  = aired["year"].toInt(-1);
    QDate airedDate(year, month, day);
    if (airedDate.isValid()) {
        show.releaseDate = airedDate.toString("MMMM d, yyyy");
        show.updateTime = airedDate.toString("'Every' dddd");
    }
    if (aired.contains("hour")) {
        int hour = aired["hour"].toInt();
        int minute = aired["minute"].toInt(0);
        show.updateTime += QString(" at %1:%2")
                               .arg(hour, 2, 10, QLatin1Char('0'))
                               .arg(minute, 2, 10, QLatin1Char('0'));
    }

    return episodeCount;
}

QList<VideoServer> AllAnime::loadServers(Client *client, const PlaylistItem *episode) const {
    const QString userAgent = m_headers["User-Agent"];
    QJsonObject config = signingConfig();
    Boot boot = bootstrap(client, config, userAgent);
    if (boot.key.isEmpty() && boot.refused && !client->isCancelled()) {
        logInfo() << name() << "the episode signing was refused:" << boot.error;
        if (const QJsonObject fresh = captureSigning(client, config); !fresh.isEmpty()) {
            config = fresh;
            saveSigningConfig(config);
            boot = bootstrap(client, config, userAgent);
        }
    }
    if (client->isCancelled()) return {};
    if (boot.key.isEmpty())
        throw AppException(boot.refused
                               ? tr("AllAnime changed how it signs episode requests, and "
                                    "the app could not read the new scheme off %1 (%2). "
                                    "Search still works; pick the show on another "
                                    "provider.").arg(config["referer"].toString(), boot.error)
                               : tr("AllAnime could not be reached: %1").arg(boot.error),
                           name());

    const QString queryHash = QString::fromLatin1(
        QCryptographicHash::hash(kEpisodeQuery, QCryptographicHash::Sha256).toHex());
    auto headers = signingHeaders(config, userAgent);
    headers["Content-Type"] = "application/json";

    QList<VideoServer> servers;
    for (const QString &vars : episode->link.split(";")) {
        const QJsonObject request{
            {"query", kEpisodeQuery},
            {"variables", QJsonDocument::fromJson(vars.toUtf8()).object()},
            {"extensions", QJsonObject{{"persistedQuery", QJsonObject{{"version", 1},
                                                                      {"sha256Hash", queryHash}}},
                                       {"k", config["episode_lane"]},
                                       {"aaReq", signRequest(config, boot, queryHash)}}}};
        const QJsonObject json =
            client->post(config["api_url"].toString(),
                         QJsonDocument(request).toJson(QJsonDocument::Compact), headers)
                .toJsonObject();
        QJsonObject data = json["data"].toObject();
        if (data.contains("tobeparsed"))
            data = decryptSources(data["tobeparsed"].toString(), boot.key);
        const QString refusal = json["errors"].toArray().at(0).toObject()["message"].toString();
        if (!refusal.isEmpty() && data["episode"].toObject().isEmpty()) {
            if (refusal == QLatin1String("NEED_CAPTCHA"))
                throw AppException(tr("AllAnime wants a captcha solved before it serves "
                                      "more episodes to this network, which it asks for "
                                      "after a burst of requests. Try again later, or "
                                      "pick the show on another provider."),
                                   name());
            throw AppException(refusal.startsWith(QLatin1String("AA_CRYPTO"))
                                   ? tr("AllAnime refused the signed episode request "
                                        "(%1): its signing scheme has changed beyond "
                                        "what the app can follow.").arg(refusal)
                                   : tr("AllAnime refused the episode request: %1")
                                         .arg(refusal),
                               name());
        }

        auto sourceUrls = data["episode"].toObject()["sourceUrls"].toArray();
        if (sourceUrls.isEmpty())
            logWarn() << name() << "no sources for" << vars << "in"
                      << QJsonDocument(json).toJson(QJsonDocument::Compact).left(300);
        // By content, not position: a dub-only episode has one entry.
        const bool isSub = !vars.contains(QLatin1String(R"("translationType":"dub")"));
        QString suffix = isSub ? " Sub" : " Dub";
        auto tr = isSub ? VideoServer::Sub : VideoServer::Dub;
        for (const QJsonValue &val : std::as_const(sourceUrls)) {
            QJsonObject src = val.toObject();
            servers.emplaceBack(src["sourceName"].toString() + suffix, src["sourceUrl"].toString(), tr);
        }
    }

    std::sort(servers.begin(), servers.end(),
              [](const VideoServer &a, const VideoServer &b) {
                  if (a.translation != b.translation) return a.translation < b.translation;
                  return a.name < b.name;
              });
    return servers;
}

PlayInfo AllAnime::extractSource(Client *client, VideoServer server) {
    PlayInfo playItem;
    auto decryptedLink = decryptSource(server.link);

    if (server.name.startsWith("Mp4")) {
        auto response = client->get(decryptedLink, m_headers).body;
        static QRegularExpression regex(R"(src: "([^"]+))");
        auto match = regex.match(response);
        if (match.hasMatch()) {
            playItem.videos.emplaceBack(match.captured(1));
            playItem.addHeader("referer", "https://mp4upload.com/");
            playItem.addHeader("user-agent", m_headers["User-Agent"]);
        }
    }
    else if (server.name.startsWith("Fm-Hls")) {
        // byse: /api/videos/<code>/ -> AES-256-GCM config.
        const QUrl embedUrl(server.link);
        const QString code = embedUrl.path().section('/', -1, -1, QString::SectionSkipEmpty);
        if (!code.isEmpty()) {
            const QString api = QString("%1://%2/api/videos/%3/")
                                    .arg(embedUrl.scheme(), embedUrl.host(), code);
            const QJsonObject resp = client->get(api, m_headers).toJsonObject();
            if (resp.contains("playback")) {
                parseByse(resp, playItem, m_headers["User-Agent"]);
                // The byse CDN rejects requests without a Referer.
                if (!playItem.videos.isEmpty())
                    playItem.addHeader("referer", QString("%1://%2/").arg(embedUrl.scheme(), embedUrl.host()));
                return playItem;
            }
        }
        const QString body = client->get(server.link).body;
        int p = body.indexOf("iframe src=\"");
        if (p < 0) return playItem;
        const QString iframeSrc = body.mid(p + 12).section('"', 0, 0);
        if (!iframeSrc.startsWith("http")) return playItem;
        const QString unpacked = Js::unpack(client->get(iframeSrc).body);
        static const QRegularExpression re(R"(:\[\{file:"([^"]+)\")");
        auto m = re.match(unpacked);
        if (m.hasMatch()) playItem.videos.emplaceBack(m.captured(1));
    }
    else if (server.name.startsWith("Sw")) {
        // streamwish and clones: packed JS with sources:[{file}].
        const QString html = client->get(decryptedLink, m_headers).body;
        QString hay = Js::unpack(html);
        if (hay.isEmpty()) hay = html;
        static const QRegularExpression re(QStringLiteral("file:\"(https?://[^\"]+\\.m3u8[^\"]*)\""));
        auto m = re.match(hay);
        if (m.hasMatch()) {
            playItem.videos.emplaceBack(m.captured(1));
            const QUrl u(decryptedLink);
            playItem.addHeader("referer", QString("%1://%2/").arg(u.scheme(), u.host()));
            playItem.addHeader("user-agent", m_headers["User-Agent"]);
        }
    }
    else if (server.name.startsWith("Sl")) {
        // streamlare: POST /api/video/stream/get.
        const QUrl u(decryptedLink);
        const QString id = u.path().section('/', -1, -1, QString::SectionSkipEmpty);
        if (!id.isEmpty()) {
            QMap<QString, QString> headers = m_headers;
            headers["Content-Type"] = "application/json";
            headers["Referer"] = decryptedLink;
            const QByteArray body = QByteArray("{\"id\":\"") + id.toUtf8() + "\"}";
            const QString api = QString("%1://%2/api/video/stream/get").arg(u.scheme(), u.host());
            const QJsonObject result = client->post(api, body, headers).toJsonObject()
                                           .value("result").toObject();
            for (auto it = result.constBegin(); it != result.constEnd(); ++it) {
                const QString file = it.value().toObject().value("file").toString();
                if (!file.isEmpty()) { playItem.videos.emplaceBack(file, it.key()); break; }
            }
            if (!playItem.videos.isEmpty())
                playItem.addHeader("user-agent", m_headers["User-Agent"]);
        }
    }
    else if (server.name.startsWith("Yt") && decryptedLink.startsWith("http")) {
        // A clock-path Yt falls through to /apivtwo.
        playItem.videos.emplaceBack(decryptedLink);
        playItem.addHeader("referer", "https://allanime.day/");
        playItem.addHeader("user-agent", m_headers["User-Agent"]);
    }
    else if (server.name.startsWith("Ok")) {
        parseOkRu(client->get(decryptedLink, m_headers).body, playItem, m_headers["User-Agent"]);
    }
    else if (server.name.startsWith("Vn")) {
        // vidnest: POST /dl -> JWPlayer config.
        const QUrl embedUrl(server.link);
        const QString code = embedUrl.path().section('/', -1, -1, QString::SectionSkipEmpty);
        if (!code.isEmpty()) {
            const QString dl = QString("%1://%2/dl").arg(embedUrl.scheme(), embedUrl.host());
            const QMap<QString, QString> form{
                {"op", "embed"}, {"file_code", code}, {"auto", "1"}, {"referer", kEndPoint}};
            QMap<QString, QString> headers = m_headers;
            headers["Referer"] = server.link;
            const QString page = client->post(dl, form, headers).body;

            static const QRegularExpression srcRe(R"RX(file:"([^"]+)"(?:,label:"([^"]*)")?)RX");
            static const QRegularExpression hRe(QStringLiteral("x(\\d+)"));
            auto it = srcRe.globalMatch(page);
            while (it.hasNext()) {
                const auto m = it.next();
                const QString url = m.captured(1);
                if (!url.startsWith("http") || (!url.contains(".mp4") && !url.contains(".m3u8")))
                    continue;
                const QString label = m.captured(2);
                const auto hm = hRe.match(label);
                playItem.videos.emplaceBack(url, label, hm.hasMatch() ? hm.captured(1).toInt() : 0);
            }
            if (!playItem.videos.isEmpty()) {
                playItem.addHeader("user-agent", m_headers["User-Agent"]);
                playItem.addHeader("referer", QString("%1://%2/").arg(embedUrl.scheme(), embedUrl.host()));
            }
        }
    }
    else if (server.name.startsWith("Fm-mp4")) {
        QString path = decryptedLink;
        if (path.contains("/clock") && !path.contains("/clock.json"))
            path.replace("/clock", "/clock.json");
        QString url = path.startsWith("http") ? path : QString(kEndPoint) + path;
        extractFilemoon(client->get(url, m_headers).toJsonObject(), playItem);
    }
    else if (decryptedLink.startsWith("/apivtwo")) {
        auto url = QString(kEndPoint) + decryptedLink.insert(14, ".json");
        auto response = client->get(url, m_headers).toJsonObject();
        auto links = response["links"].toArray();

        for (const QJsonValue &linkVal : std::as_const(links)) {
            QJsonObject link = linkVal.toObject();

            if (link.contains("dash")) {
                auto rawUrls = link["rawUrls"].toObject();
                for (const QJsonValue &v : rawUrls["vids"].toArray()) {
                    QJsonObject vid = v.toObject();
                    int h = vid["height"].toInt();
                    int bw = vid["bandwidth"].toInt();
                    QString label = QString("%1p %2").arg(h).arg(Track::formatBitrate(bw));
                    playItem.videos.emplaceBack(vid["url"].toString(), label, h, bw);
                }
                for (const QJsonValue &a : rawUrls["audios"].toArray()) {
                    QJsonObject aud = a.toObject();
                    int bw = aud["bandwidth"].toInt();
                    playItem.audios.emplaceBack(aud["url"].toString(), Track::formatBitrate(bw), "", bw);
                }
            } else {
                playItem.videos.emplaceBack(link["link"].toString());
            }

            if (!link.contains("subtitles")) continue;
            for (const QJsonValue &s : link["subtitles"].toArray()) {
                QJsonObject sub = s.toObject();
                QString subUrl = sub["src"].toString();
                QString label = sub["label"].toString();

                if (subUrl.startsWith("https://allanime.pro/apiak/sk.json")) {
                    auto subResp = client->get(subUrl);
                    if (subResp.body.startsWith("{\"font")) {
                        const QString path = convertJsonSubToSrt(subResp.toJsonObject(), subUrl);
                        if (path.isEmpty()) continue;
                        // A drive letter parses as the scheme.
                        playItem.subtitles.emplaceBack(QUrl::fromLocalFile(path), label);
                        continue;
                    }
                }
                playItem.subtitles.emplaceBack(subUrl, label);
            }
        }

        // Some sources expose only a top-level HLS stream.
        auto hls = response["hls"].toObject();
        if (hls.contains("url"))
            playItem.videos.emplaceBack(hls["url"].toString());
    }
    return playItem;
}

QString AllAnime::coverImage(const QJsonObject &json) const {
    QString url = json["thumbnail"].toString();
    if (!url.startsWith("https"))
        url = "https://wp.youtube-anime.com/aln.youtube-anime.com/" + url;
    return url + "?w=250";
}

QString AllAnime::decryptSource(const QString &input) const {
    if (!input.startsWith('-')) return input;

    QString hexString = input.section('-', -1);
    QString result;
    result.reserve(hexString.length() / 2);
    for (int i = 0; i + 1 < hexString.length(); i += 2) {
        bool ok;
        int byte = hexString.mid(i, 2).toInt(&ok, 16);
        if (ok) result += QChar(byte ^ 56);
    }
    return result;
}

QString AllAnime::convertJsonSubToSrt(const QJsonObject &json, const QString &sourceUrl) const {
    QString fileName = sourceUrl.split("?").last();
    QString filePath = Settings::tempDir() + "/" + fileName;

    QFile outputFile(filePath);
    if (!outputFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        logWarn() << name() << "Failed to write subtitle file:" << filePath;
        return {};
    }

    QTextStream out(&outputFile);
    int index = 1;
    for (const QJsonValue &val : json["body"].toArray()) {
        QJsonObject line = val.toObject();
        double from = line["from"].toDouble();
        double to = line["to"].toDouble();
        QString content = line["content"].toString();

        out << index++ << "\n"
            << msToSrtTime(from) << " --> " << msToSrtTime(to) << "\n"
            << content << "\n\n";
    }
    outputFile.close();
    return QFileInfo(outputFile).absoluteFilePath();
}