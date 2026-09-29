#include "net/dnsoverhttps.h"
#include "core/logger.h"
#include "net/client.h"

#include <QDateTime>
#include <QHash>
#include <QHostAddress>
#include <QHostInfo>
#include <QMutex>
#include <QNetworkRequest>
#include <QSet>

namespace {

struct Entry {
    QString address;   // empty: the system resolver finds it, or nothing does
    qint64  expiresAt = 0;
};

QMutex g_mutex;
QHash<QString, Entry> g_overHttps;     // what a lookup over HTTPS gave
QHash<QString, Entry> g_systemChecks;  // hosts whose system lookup has been tried
// Filtering is by domain and its failures come and go, so once one host of a domain has
// needed this, its siblings skip the system resolver rather than gamble on it.
QSet<QString> g_filteredDomains;

QString domainOf(const QString &host) {
    return host.section(QLatin1Char('.'), -2);
}

bool lookup(const QHash<QString, Entry> &cache, const QString &host, QString &address) {
    QMutexLocker lock(&g_mutex);
    const auto it = cache.constFind(host);
    if (it == cache.constEnd() || it->expiresAt <= QDateTime::currentMSecsSinceEpoch()) return false;
    address = it->address;
    return true;
}

void remember(QHash<QString, Entry> &cache, const QString &host, const QString &address, int minutes) {
    QMutexLocker lock(&g_mutex);
    cache.insert(host, {address, QDateTime::currentMSecsSinceEpoch() + minutes * 60 * 1000});
}

}

QString DnsOverHttps::resolve(const QString &host, const CancelToken &cancel) {
    QString address;
    if (lookup(g_overHttps, host, address)) return address;

    Client resolver(cancel, false);
    resolver.setBypassEnabled(false);
    for (const char *endpoint : {"https://1.1.1.1/dns-query", "https://8.8.8.8/resolve"}) {
        const auto answer = resolver.get(QLatin1String(endpoint), {{"Accept", "application/dns-json"}},
                                         {{"name", host}, {"type", "A"}});
        for (const QJsonValue &record : answer.toJsonObject().value("Answer").toArray())
            if (record.toObject().value("type").toInt() == 1) {   // not the CNAME chain
                address = record.toObject().value("data").toString();
                break;
            }
        if (!address.isEmpty() || resolver.isCancelled()) break;
    }
    if (resolver.isCancelled()) return {};
    if (!address.isEmpty()) {
        logStep() << "Network" << host << "does not resolve here; reaching it at" << address
                  << "from DNS over HTTPS";
        QMutexLocker lock(&g_mutex);
        g_filteredDomains.insert(domainOf(host));
    }
    // A miss is remembered briefly too, or every request to a dead host asks twice more.
    remember(g_overHttps, host, address, address.isEmpty() ? 1 : 10);
    return address;
}

QString DnsOverHttps::cachedAddress(const QString &host) {
    QString address;
    return lookup(g_overHttps, host, address) ? address : QString();
}

QString DnsOverHttps::addressIfSystemFails(const QString &host) {
    if (host.isEmpty() || !QHostAddress(host).isNull()) return {};
    QString address;
    if (lookup(g_systemChecks, host, address)) return address;
    bool filtered = false;
    {
        QMutexLocker lock(&g_mutex);
        filtered = g_filteredDomains.contains(domainOf(host));
    }
    if (filtered) return resolve(host);
    const QHostInfo system = QHostInfo::fromName(host);
    // SERVFAIL surfaces as a temporary failure, not HostNotFound, so any failure counts.
    const bool failed = system.error() != QHostInfo::NoError || system.addresses().isEmpty();
    address = failed ? resolve(host) : QString();
    remember(g_systemChecks, host, address, 10);
    return address;
}

void DnsOverHttps::pin(QNetworkRequest &request, const QString &address) {
    QUrl target = request.url();
    const QString host = target.host();
    const QByteArray authority = target.authority(QUrl::FullyEncoded).toUtf8();
    target.setHost(address);
    request.setUrl(target);
    request.setPeerVerifyName(host);
    request.setRawHeader("Host", authority);
}
