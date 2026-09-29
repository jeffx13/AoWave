#pragma once
#include <QString>
#include "net/canceltoken.h"

class QNetworkRequest;

// Filtered networks answer SERVFAIL for some hosts. This asks resolvers addressed by IP
// (1.1.1.1, then 8.8.8.8), so the lookup needs no DNS of its own.
namespace DnsOverHttps {

// An IPv4 address, or empty. Hits are remembered for ten minutes, misses for one.
QString resolve(const QString &host, const CancelToken &cancel = {});

// The address a lookup already gave for `host`, or empty. Never blocks.
QString cachedAddress(const QString &host);

// For callers that cannot retry after a failed request: blocks on the system resolver the
// first time a host is seen, and only looks it up over HTTPS when that fails, or when another
// host of its domain already needed to. Empty when the system resolver finds the host.
QString addressIfSystemFails(const QString &host);

// Sends `request` to `address` while its certificate check, SNI and Host header keep naming
// the url's own host.
void pin(QNetworkRequest &request, const QString &address);

}
