#pragma once
#include <QString>
#include <QByteArray>
#include <QMap>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrl>
#include "net/html.h"
#include "net/canceltoken.h"

inline constexpr char kFirefoxUserAgent[] =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:143.0) Gecko/20100101 Firefox/143.0";

class Client {
public:
    Client(CancelToken cancel = {}, bool verbose = true)
        : m_cancel(std::move(cancel)), m_verbose(verbose) {}

    Client(const Client &other) = default;
    Client &operator=(const Client &other) = default;

    bool isCancelled() const { return m_cancel.isCancelled(); }
    const CancelToken &cancelToken() const { return m_cancel; }

    // Why the most recent request on this client failed, for a caller that only got an empty
    // result back from a provider. Empty after a success.
    QString lastError() const { return m_lastError; }

    static QString urlWithParams(const QString &url, const QMap<QString, QString> &params);

    Client withCancel(const CancelToken &secondary) const {
        Client c = *this;
        c.m_cancel = m_cancel.composeWith(secondary);
        return c;
    }

    // Off for endpoints that must not recurse into the solver.
    Client &setBypassEnabled(bool enabled) { m_bypass = enabled; return *this; }

    Client &setVerbose(bool verbose) { m_verbose = verbose; return *this; }

    Client withSession(const QString &owner) const {
        Client copy = *this;
        copy.m_session = owner;
        return copy;
    }

    // Idle-transfer limit, not a deadline.
    Client &setTimeout(int ms) { m_timeoutMs = ms; return *this; }

    // A host that ignores Range would stream the whole file into memory.
    Client &setMaxBodyBytes(qint64 bytes) { m_maxBodyBytes = bytes; return *this; }

    struct Response {
        int code = -1;
        // Repeats identically; code <= 0 alone also covers timeouts.
        bool deadHost = false;
        QString error;
        QUrl finalUrl;
        QMap<QString, QString> headers;
        QString body;
        QByteArray bytes;   // getBytes() only

        QString header(const QString &name) const {
            for (auto it = headers.constBegin(); it != headers.constEnd(); ++it)
                if (it.key().compare(name, Qt::CaseInsensitive) == 0)
                    return it.value();
            return {};
        }

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

    // QString::fromUtf8 would fill a binary body with U+FFFD.
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

    Response head(const QString &url,
                  const QMap<QString, QString> &headers = {}) {
        return request(HEAD, url, headers);
    }

private:
    enum RequestType { GET, POST, HEAD };
    struct Raw;
    Response request(int type, const QString &url,
                     const QMap<QString, QString> &headers,
                     const QByteArray &postData = {},
                     bool binary = false);
    Response requestOnce(int type, const QString &url, const QMap<QString, QString> &headers,
                         const QByteArray &postData, bool binary);
    // `address` connects to that IP instead of resolving the url's host.
    Raw fetchViaQt(int type, const QString &url, const QMap<QString, QString> &headers,
                   const QByteArray &postData, const QString &address = {});
    Raw fetchInBrowser(const QString &url, bool binary, const QMap<QString, QString> &headers);

    CancelToken m_cancel;
    QString     m_lastError;
    QString     m_session;
    int         m_timeoutMs = 10000;
    qint64      m_maxBodyBytes = 0;
    bool        m_verbose = true;
    bool        m_bypass = true;
};
