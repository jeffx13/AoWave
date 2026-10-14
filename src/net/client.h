#pragma once
#include "net/html.h"
#include "app/async.h"
#include <QString>
#include <QByteArray>
#include <QMap>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrl>
#include <functional>
#include <QVariantMap>

inline constexpr char kFirefoxUserAgent[] =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:148.0) Gecko/20100101 Firefox/148.0";
inline constexpr char kChromeUserAgent[] =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/140.0.0.0 Safari/537.36";

class Client {
public:
    Client(CancelToken cancel = {}, bool verbose = true)
        : m_cancel(std::move(cancel)), m_verbose(verbose) {}

    Client(const Client &other) = default;
    Client &operator=(const Client &other) = default;

    bool isCancelled() const { return m_cancel.isCancelled(); }
    const CancelToken &cancelToken() const { return m_cancel; }

    QString lastError() const { return m_lastError; }

    static QString urlWithParams(const QString &url, const QMap<QString, QString> &params);
    static QString headerValue(const QMap<QString, QString> &headers, QAnyStringView name) {
        for (auto it = headers.constBegin(); it != headers.constEnd(); ++it)
            if (QAnyStringView::compare(it.key(), name, Qt::CaseInsensitive) == 0) return it.value();
        return {};
    }

    Client withCancel(const CancelToken &secondary) const {
        Client c = *this;
        c.m_cancel = m_cancel.composeWith(secondary);
        return c;
    }

    Client &setBypassEnabled(bool enabled) { m_bypass = enabled; return *this; }

    Client &setStrictTls(bool strict) { m_strictTls = strict; return *this; }

    Client &setVerbose(bool verbose) { m_verbose = verbose; return *this; }

    Client withSession(const QString &owner) const {
        Client copy = *this;
        copy.m_session = owner;
        return copy;
    }

    // The provider whose Settings badge counts these requests; the session names the cookie jar and browser profile instead.
    Client &setProvider(const QString &name) { m_provider = name; return *this; }

    Client &setTimeout(int ms) { m_timeoutMs = ms; return *this; }

    Client &setMaxBodyBytes(qint64 bytes) { m_maxBodyBytes = bytes; return *this; }

    using Sink = std::function<bool(int code, const QMap<QString, QString> &headers, const QByteArray &chunk)>;
    Client &setSink(Sink sink) { m_sink = std::move(sink); return *this; }

    struct Response {
        int code = -1;
        bool deadHost = false;
        QString error;
        QUrl finalUrl;
        QMap<QString, QString> headers;
        QString body;
        QByteArray bytes;
        QString errorBody;

        QString header(QAnyStringView name) const { return headerValue(headers, name); }

        QJsonDocument toJson() const {
            if (body.isEmpty()) return {};
            QJsonParseError parseError;
            const auto doc = QJsonDocument::fromJson(body.toUtf8(), &parseError);
            return parseError.error == QJsonParseError::NoError ? doc : QJsonDocument();
        }

        QJsonObject toJsonObject() const { return toJson().object(); }
        QJsonArray  toJsonArray()  const { return toJson().array(); }

        Html toHtml() const { return Html::parse(body); }
    };

    Response get(const QString &url,
                 const QMap<QString, QString> &headers = {},
                 const QMap<QString, QString> &params = {});

    Response getBytes(const QString &url,
                      const QMap<QString, QString> &headers = {},
                      const QMap<QString, QString> &params = {});

    Response post(const QString &url,
                  const QMap<QString, QString> &data,
                  const QMap<QString, QString> &headers = {});

    Response post(const QString &url,
                  const QByteArray &data,
                  const QMap<QString, QString> &headers = {}) {
        return request(POST, url, headers, data);
    }

    Response put(const QString &url,
                 const QMap<QString, QString> &data,
                 const QMap<QString, QString> &headers = {});

    Response head(const QString &url,
                  const QMap<QString, QString> &headers = {}) {
        return request(HEAD, url, headers);
    }

private:
    enum RequestType { GET, POST, HEAD, PUT };
    struct Raw;
    Response request(int type, const QString &url,
                     const QMap<QString, QString> &headers,
                     const QByteArray &postData = {},
                     bool binary = false);
    Response requestOnce(int type, const QString &url, const QMap<QString, QString> &headers,
                         const QByteArray &postData, bool binary);
    Raw fetchViaQt(int type, const QString &url, const QMap<QString, QString> &headers,
                   const QByteArray &postData, const QString &address = {});
    Raw fetchInBrowser(const QString &url, bool binary, const QMap<QString, QString> &headers);

    CancelToken m_cancel;
    QString     m_lastError;
    QString     m_session;
    QString     m_provider;
    int         m_timeoutMs = 10000;
    qint64      m_maxBodyBytes = 0;
    Sink        m_sink;
    bool        m_verbose = true;
    bool        m_bypass = true;
    bool        m_strictTls = false;
};

// Empty on any error, and past 32 MB out: these are a site's answers, never files.
namespace Decompress {
QByteArray gzipOrZlib(const QByteArray &input);
QByteArray rawDeflate(const QByteArray &input);
}

namespace ProviderHealth {

void record(const QString &owner, bool ok, qint64 elapsedMs);

QVariantMap statsFor(const QString &owner);

void reset(const QString &owner);

}

class QNetworkRequest;

namespace DnsOverHttps {

QString resolve(const QString &host, const CancelToken &cancel = {});

QString cachedAddress(const QString &host);

QString addressIfSystemFails(const QString &host, const CancelToken &cancel);

void waitForLookups();

void pin(QNetworkRequest &request, const QString &address);

}

// What a QML image asks with: Chrome's User-Agent or the clearance's, the disk cache preferred for a host
// Cloudflare fronts, an address pinned where DNS fails.
QNetworkRequest imageRequest(const QNetworkRequest &request);
