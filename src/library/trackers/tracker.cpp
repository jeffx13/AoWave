#include "library/trackers/tracker.h"
#include "app/logger.h"
#include "app/mainwindow.h"
#include "app/settings.h"
#include "system/platform.h"
#include <QDateTime>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrlQuery>
#include <memory>
#include <QtConcurrent/QtConcurrentRun>

QString Tracker::settingsGroup() const { return QStringLiteral("trackers/") + name(); }

void Tracker::storeToken(const Grant &grant) {
    Settings &s = Settings::instance();
    const QString sealed = Platform::protect(grant.token);
    const QString sealedRefresh = Platform::protect(grant.refresh);
    if (!grant.token.isEmpty() && sealed.isEmpty()) {
        // No plain-text fallback: a token the user cannot tell is exposed is worse.
        logWarn() << "Sync" << name() + QLatin1Char(':') << "could not protect the token; not storing it. "
                               "Sign-in will not survive a restart.";
    } else {
        s.setValue(settingsGroup() + QStringLiteral("/tokenEnc"), sealed);
        s.setValue(settingsGroup() + QStringLiteral("/refreshEnc"), sealedRefresh);
        s.setValue(settingsGroup() + QStringLiteral("/expires"), grant.expiresAt);
    }
    {
        QMutexLocker lock(&m_authMutex);
        m_token = grant.token;
        m_refreshToken = grant.refresh;
        m_expiresAt = grant.expiresAt;
    }
    emit authChanged();
}

void Tracker::loadToken() {
    Settings &s = Settings::instance();
    m_accountName = s.value(settingsGroup() + QStringLiteral("/account")).toString();
    QMutexLocker lock(&m_authMutex);
    m_clientId = s.value(settingsGroup() + QStringLiteral("/clientId")).toString().trimmed();
    m_clientSecret = Platform::unprotect(
        s.value(settingsGroup() + QStringLiteral("/clientSecretEnc")).toString());
    m_expiresAt = s.value(settingsGroup() + QStringLiteral("/expires")).toLongLong();
    m_token = Platform::unprotect(s.value(settingsGroup() + QStringLiteral("/tokenEnc")).toString());
    m_refreshToken = Platform::unprotect(s.value(settingsGroup() + QStringLiteral("/refreshEnc")).toString());
}

QString Tracker::accessToken() const {
    QMutexLocker lock(&m_authMutex);
    return m_token;
}

QString Tracker::clientId() const {
    QMutexLocker lock(&m_authMutex);
    return m_clientId;
}

QString Tracker::clientSecret() const {
    QMutexLocker lock(&m_authMutex);
    return m_clientSecret;
}

QString Tracker::freshToken(Client *client) {
    QMutexLocker refreshing(&m_refreshMutex);
    QString refresh;
    {
        QMutexLocker lock(&m_authMutex);
        const bool expiring = m_expiresAt > 0 && QDateTime::currentSecsSinceEpoch() > m_expiresAt - 300;
        if (m_token.isEmpty() || !expiring || m_refreshToken.isEmpty()) return m_token;
        refresh = m_refreshToken;
    }
    const Grant grant = refreshGrant(client, refresh);
    if (grant.token.isEmpty()) {
        logWarn() << "Sync" << name() + QLatin1Char(':') << "token refresh failed; sign in again";
        return {};
    }
    QMetaObject::invokeMethod(this, [this, grant]() { storeToken(grant); }, Qt::QueuedConnection);
    QMutexLocker lock(&m_authMutex);
    m_token = grant.token;
    m_refreshToken = grant.refresh;
    m_expiresAt = grant.expiresAt;
    return m_token;
}

void Tracker::shutdown() {
    m_cancel.cancel();
    m_runs.waitAll("Tracker request");
}

void Tracker::setClientId(const QString &id) {
    const QString trimmed = id.trimmed();
    {
        QMutexLocker lock(&m_authMutex);
        if (trimmed == m_clientId) return;
        m_clientId = trimmed;
    }
    Settings::instance().setValue(settingsGroup() + QStringLiteral("/clientId"), trimmed);
    if (isAuthenticated()) signOut();
    else                   emit authChanged();
}

void Tracker::setClientSecret(const QString &secret) {
    const QString trimmed = secret.trimmed();
    {
        QMutexLocker lock(&m_authMutex);
        if (trimmed == m_clientSecret) return;
        m_clientSecret = trimmed;
    }
    Settings::instance().setValue(settingsGroup() + QStringLiteral("/clientSecretEnc"),
                                  Platform::protect(trimmed));
    if (isAuthenticated()) signOut();
    else                   emit authChanged();
}

void Tracker::lookUpAccount(std::function<QString(Client &)> ask) {
    runInBackground([this, ask = std::move(ask)](const CancelToken &cancel) {
        Client client(cancel);
        const QString name = ask(client);
        if (name.isEmpty()) return;
        QMetaObject::invokeMethod(this, [this, name]() { setAccountName(name); }, Qt::QueuedConnection);
    });
}

void Tracker::setAccountName(const QString &accountName) {
    if (accountName == m_accountName) return;
    m_accountName = accountName;
    Settings::instance().setValue(settingsGroup() + QStringLiteral("/account"), accountName);
    emit authChanged();
}

void Tracker::signOut() {
    Settings &s = Settings::instance();
    for (const char *key : {"/tokenEnc", "/refreshEnc", "/expires", "/account"})
        s.setValue(settingsGroup() + QLatin1String(key), QString());
    {
        QMutexLocker lock(&m_authMutex);
        m_token.clear();
        m_refreshToken.clear();
        m_expiresAt = 0;
    }
    m_accountName.clear();
    emit authChanged();
}


QStringList Tracker::statusLabels() const {
    return statusVocabulary();
}

bool Tracker::advance(Entry &entry, int episode, const QStringList &vocabulary, QDate today) {
    if (episode <= 0 || vocabulary.size() < 3) return false;
    const QString &watching = vocabulary.at(0), &planning = vocabulary.at(1), &completed = vocabulary.at(2);
    const bool rewatch = vocabulary.size() > 5 && entry.status == vocabulary.at(5);
    const int reached = entry.total > 0 ? qMin(episode, entry.total) : episode;
    if (entry.onList && (reached <= entry.progress || entry.status == completed)) return false;

    entry.progress = reached;
    if (!entry.onList || entry.status.isEmpty() || entry.status == planning) {
        entry.status = watching;
        if (entry.started.isEmpty()) entry.started = today.toString(Qt::ISODate);
    }
    entry.onList = true;
    if (entry.total > 0 && reached >= entry.total) {
        if (rewatch) ++entry.repeat;
        entry.status = completed;
        if (entry.finished.isEmpty()) entry.finished = today.toString(Qt::ISODate);
    }
    return true;
}

LoopbackAuth *LoopbackAuth::start(QObject *parent, QPointer<LoopbackAuth> &pending, int port,
                                  const QString &service) {
    if (pending) {
        delete pending.data();
        pending = nullptr;
    }
    auto *loopback = new LoopbackAuth(parent);
    pending = loopback;
    connect(loopback, &LoopbackAuth::failed, loopback, [loopback, service](const QString &why) {
        logWarn() << "Sync" << service + QLatin1Char(':') << "sign-in failed:" << why;
        MainWindow::instance().reportError(
            tr("%1 sign-in did not complete (%2).").arg(service, why), service);
        loopback->deleteLater();
    });
    if (loopback->listen(port) == 0) {
        loopback->deleteLater();
        return nullptr;
    }
    return loopback;
}

LoopbackAuth::LoopbackAuth(QObject *parent) : QObject(parent) {
    quint32 words[4];
    QRandomGenerator::system()->fillRange(words);
    m_state = QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(words), sizeof words).toHex());
}

void LoopbackAuth::serve(QTcpSocket *socket, const QByteArray &contentType,
                         const QByteArray &body) {
    socket->write("HTTP/1.1 200 OK\r\nContent-Type: " + contentType
                  + "\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: "
                  + QByteArray::number(body.size()) + "\r\n\r\n" + body);
    socket->disconnectFromHost();
}

namespace {

// An implicit grant's token rides in the fragment, which a browser never sends; this page reposts it as a query.
QByteArray bridgePage() {
    return QByteArrayLiteral(
        "<!doctype html><meta charset=\"utf-8\"><title>" APP_NAME "</title>"
        "<body style=\"font:16px system-ui;padding:3rem;text-align:center\">"
        "<p id=\"m\">Finishing sign-in...</p><script>"
        "var h=location.hash.slice(1);"
        "if(h){location.replace('/?'+h);}"
        "else{document.getElementById('m').textContent="
        "'That redirect carried no token. You can close this tab.';}"
        "</script></body>");
}

QByteArray donePage(const QString &message) {
    return QByteArrayLiteral("<!doctype html><meta charset=\"utf-8\"><title>" APP_NAME "</title>"
                             "<body style=\"font:16px system-ui;padding:3rem;text-align:center\"><p>")
           + message.toHtmlEscaped().toUtf8() + QByteArrayLiteral("</p></body>");
}

QUrl targetOf(const QByteArray &request) {
    const QString text = QString::fromUtf8(request);
    const int start = text.indexOf(QLatin1String("GET "));
    const int end = text.indexOf(QLatin1String(" HTTP"), start);
    if (start < 0 || end <= start) return {};
    return QUrl(QStringLiteral("http://127.0.0.1") + text.mid(start + 4, end - start - 4));
}

}

int LoopbackAuth::listen(int port, int timeoutMs) {
    auto *server = new QTcpServer(this);
    if (!server->listen(QHostAddress::LocalHost, quint16(port))) {
        emit failed(port == 0
                        ? tr("could not open a local port")
                        : tr("port %1 is in use by another sign-in or program").arg(port));
        server->deleteLater();
        return 0;
    }
    port = server->serverPort();

    connect(server, &QTcpServer::newConnection, this, [this, server]() {
        QTcpSocket *socket = server->nextPendingConnection();
        auto buffer = std::make_shared<QByteArray>();
        connect(socket, &QTcpSocket::readyRead, this, [this, socket, server, buffer]() {
            buffer->append(socket->readAll());
            if (!buffer->contains('\n')) return;

            const QUrl target = targetOf(*buffer);
            // A browser asks for /favicon.ico by itself; treating it as an empty redirect would end the sign-in.
            const QString path = target.path();
            if (!path.isEmpty() && path != QLatin1String("/")) {
                socket->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
                              "Connection: close\r\n\r\n");
                socket->disconnectFromHost();
                return;
            }

            const QUrlQuery query(target);
            const QString code  = query.queryItemValue(QStringLiteral("code"));
            const QString token = query.queryItemValue(QStringLiteral("access_token"));
            QString error = query.queryItemValue(QStringLiteral("error_description"),
                                                 QUrl::FullyDecoded);
            if (error.isEmpty()) error = query.queryItemValue(QStringLiteral("error"));

            if ((!code.isEmpty() || !token.isEmpty() || !error.isEmpty())
                && query.queryItemValue(QStringLiteral("state")) != m_state) {
                socket->write("HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n"
                              "Connection: close\r\n\r\n");
                socket->disconnectFromHost();
                return;
            }
            if (!code.isEmpty() || !token.isEmpty()) {
                serve(socket, "text/html; charset=utf-8",
                      donePage(QStringLiteral("Signed in. You can close this tab and return to "
                                              APP_NAME ".")));
                server->close();
                if (!code.isEmpty()) emit codeReceived(code);
                else emit tokenReceived(token,
                                        query.queryItemValue(QStringLiteral("expires_in")).toLongLong());
                return;
            }
            if (!error.isEmpty()) {
                serve(socket, "text/html; charset=utf-8",
                      donePage(QStringLiteral("Sign-in was refused. You can close this tab.")));
                server->close();
                emit failed(error);
                return;
            }
            if (!m_servedBridge) {
                m_servedBridge = true;
                serve(socket, "text/html; charset=utf-8", bridgePage());
                return;
            }
            serve(socket, "text/html; charset=utf-8",
                  donePage(QStringLiteral("That redirect carried no sign-in. You can close this "
                                          "tab.")));
            server->close();
            emit failed(tr("the redirect carried no code or token"));
        });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    });

    QTimer::singleShot(timeoutMs, server, [this, server, timeoutMs]() {
        if (!server->isListening()) return;
        server->close();
        emit failed(tr("the browser did not return to %2 within %1 minutes")
                        .arg(qMax(1, timeoutMs / 60000)).arg(QStringLiteral(APP_NAME)));
    });
    return port;
}

TrackerList::~TrackerList() {
    m_cancel.cancel();
    m_runs.waitAll("TrackerList request");
    for (Tracker *tracker : std::as_const(m_trackers)) tracker->shutdown();
}

void TrackerList::setTrackers(QList<Tracker *> &&trackers) {
    beginResetModel();
    m_trackers = std::move(trackers);
    s_byName.clear();
    for (Tracker *tracker : std::as_const(m_trackers)) {
        tracker->setParent(this);
        s_byName.insert(tracker->name(), tracker);
        connect(tracker, &Tracker::authChanged, this, [this]() {
            emit changed();
            if (!m_trackers.isEmpty())
                emit dataChanged(index(0), index(int(m_trackers.size()) - 1));
        });
    }
    endResetModel();
    emit changed();
}

int TrackerList::authenticatedCount() const {
    int n = 0;
    for (Tracker *tracker : m_trackers) if (tracker->isAuthenticated()) ++n;
    return n;
}

QStringList TrackerList::authenticatedNames() const {
    QStringList names;
    for (Tracker *tracker : m_trackers) if (tracker->isAuthenticated()) names << tracker->name();
    return names;
}

void TrackerList::authenticate(const QString &name) {
    if (Tracker *tracker = byName(name)) tracker->authenticate();
}

void TrackerList::signOut(const QString &name) {
    if (Tracker *tracker = byName(name)) tracker->signOut();
}

QStringList TrackerList::statusVocabulary(const QString &name) const {
    Tracker *tracker = byName(name);
    return tracker ? tracker->statusVocabulary() : QStringList{};
}

QVariantMap TrackerList::features(const QString &name) const {
    Tracker *tracker = byName(name);
    if (!tracker) return {};
    QVariantMap out = tracker->features();
    out["statuses"] = tracker->statusVocabulary();
    out["labels"] = tracker->statusLabels();
    out["scale"] = scoreScale(name);
    return out;
}

QVariantMap TrackerList::toMap(const Tracker::Entry &entry) {
    return {{"remoteId", entry.remoteId}, {"valid", entry.valid}, {"onList", entry.onList},
            {"title", entry.title}, {"siteUrl", entry.siteUrl}, {"total", entry.total},
            {"status", entry.status}, {"score", entry.score}, {"progress", entry.progress},
            {"remoteProgress", entry.remoteProgress}, {"repeat", entry.repeat},
            {"started", entry.started}, {"finished", entry.finished}, {"notes", entry.notes},
            {"favourite", entry.favourite}};
}

Tracker::Entry TrackerList::fromMap(const QVariantMap &map) {
    Tracker::Entry entry;
    entry.remoteId = map.value("remoteId").toString();
    entry.valid = map.value("valid").toBool();
    entry.onList = map.value("onList").toBool();
    entry.title = map.value("title").toString();
    entry.siteUrl = map.value("siteUrl").toString();
    entry.total = map.value("total").toInt();
    entry.status = map.value("status").toString();
    entry.score = map.value("score").toDouble();
    entry.progress = map.value("progress").toInt();
    entry.repeat = map.value("repeat").toInt();
    entry.remoteProgress = map.value("remoteProgress").toInt();
    entry.started = map.value("started").toString();
    entry.finished = map.value("finished").toString();
    entry.notes = map.value("notes").toString();
    entry.favourite = map.value("favourite").toBool();
    return entry;
}

QString TrackerList::clientId(const QString &name) const {
    Tracker *tracker = byName(name);
    return tracker ? tracker->clientId() : QString{};
}

void TrackerList::setClientId(const QString &name, const QString &id) {
    if (Tracker *tracker = byName(name)) tracker->setClientId(id);
}

bool TrackerList::needsClientSecret(const QString &name) const {
    Tracker *tracker = byName(name);
    return tracker && tracker->needsClientSecret();
}

QString TrackerList::clientSecret(const QString &name) const {
    Tracker *tracker = byName(name);
    return tracker ? tracker->clientSecret() : QString{};
}

void TrackerList::setClientSecret(const QString &name, const QString &secret) {
    if (Tracker *tracker = byName(name)) tracker->setClientSecret(secret);
}

bool TrackerList::canAuthenticate(const QString &name) const {
    Tracker *tracker = byName(name);
    return tracker && tracker->canAuthenticate();
}

QString TrackerList::registrationUrl(const QString &name) const {
    const Tracker *tracker = byName(name);
    return tracker ? tracker->registrationUrl() : QString{};
}

QString TrackerList::registrationHint(const QString &name) const {
    const Tracker *tracker = byName(name);
    return tracker ? tracker->registrationHint() : QString{};
}

QVariantMap TrackerList::scoreScale(const QString &name) const {
    Tracker *tracker = byName(name);
    if (!tracker) return {{"max", 10}, {"step", 1}, {"decimals", 0}};
    switch (tracker->scoreFormat()) {
    case Tracker::ScoreFormat::Point100:       return {{"max", 100}, {"step", 1},  {"decimals", 0}};
    case Tracker::ScoreFormat::Point5:         return {{"max", 5},   {"step", 1},  {"decimals", 0}};
    case Tracker::ScoreFormat::Point3:         return {{"max", 3},   {"step", 1},  {"decimals", 0}};
    case Tracker::ScoreFormat::Point10Decimal: return {{"max", 10},  {"step", 1},  {"decimals", 1}};
    case Tracker::ScoreFormat::Point10:        break;
    }
    return {{"max", 10}, {"step", 1}, {"decimals", 0}};
}

void TrackerList::autoPush(const QString &showLink, int episodeNumber,
                           const std::function<QVariantMap(QString, QString)> &linkLookup) {
    if (episodeNumber <= 0 || showLink.isEmpty()) return;
    if (!Settings::instance().trackerAutoPush()) return;

    for (Tracker *tracker : std::as_const(m_trackers)) {
        if (!tracker->isAuthenticated()) continue;
        const QVariantMap link = linkLookup(showLink, tracker->name());
        if (!link.value(QStringLiteral("valid")).toBool()) continue;
        const QString remoteId = link.value(QStringLiteral("remoteId")).toString();
        if (remoteId.isEmpty()) continue;
        if (link.value(QStringLiteral("progress")).toInt() >= episodeNumber) continue;

        const QString name = tracker->name();
        m_runs.add(QtConcurrent::run([this, tracker, name, showLink, remoteId, episodeNumber, cancel = m_cancel]() {
            Client client(cancel, false);
            Tracker::Entry entry = tracker->entryFor(&client, remoteId);
            if (cancel.isCancelled()) return;
            QString error;
            bool changed = false;
            if (!entry.valid) {
                error = tr("%1 did not respond.").arg(name);
                entry.progress = episodeNumber;
            }
            else if ((changed = Tracker::advance(entry, episodeNumber, tracker->statusVocabulary(),
                                                 QDate::currentDate())))
                error = tracker->update(&client, entry);
            if (cancel.isCancelled() || (error.isEmpty() && !changed)) return;
            logInfo() << "Sync" << name + QLatin1Char(':') << "auto-sync episode" << episodeNumber << "for" << remoteId
                      << (error.isEmpty() ? QStringLiteral("saved") : error);
            QMetaObject::invokeMethod(this, [this, name, showLink, error, out = toMap(entry)]() {
                emit pushFinished(name, showLink, error.isEmpty(), error, out, true);
            }, Qt::QueuedConnection);
        }));
    }
}

void TrackerList::search(const QString &name, const QString &query) {
    Tracker *tracker = byName(name);
    if (!tracker || query.trimmed().isEmpty()) return;
    m_runs.add(QtConcurrent::run([this, tracker, name, query, cancel = m_cancel]() {
        Client client(cancel, false);
        QVariantList out;
        for (const Tracker::Result &result : tracker->search(&client, query))
            out.append(QVariantMap{{"remoteId", result.remoteId}, {"title", result.title},
                                   {"cover", result.cover}, {"episodes", result.episodes},
                                   {"year", result.year}});
        if (cancel.isCancelled()) return;
        QMetaObject::invokeMethod(this, [this, name, out]() {
            emit searchFinished(name, out);
        }, Qt::QueuedConnection);
    }));
}

void TrackerList::link(const QString &name, const QString &showLink, const QString &remoteId) {
    Tracker *tracker = byName(name);
    if (!tracker) return;
    m_runs.add(QtConcurrent::run([this, tracker, name, showLink, remoteId, cancel = m_cancel]() {
        Client client(cancel, false);
        const Tracker::Entry entry = tracker->entryFor(&client, remoteId);
        if (cancel.isCancelled()) return;
        QMetaObject::invokeMethod(this, [this, name, showLink, out = toMap(entry)]() {
            emit entryLoaded(name, showLink, out);
        }, Qt::QueuedConnection);
    }));
}

void TrackerList::pushEntry(const QString &name, const QString &showLink, const QVariantMap &map) {
    Tracker *tracker = byName(name);
    if (!tracker) return;
    m_runs.add(QtConcurrent::run([this, tracker, name, showLink, map, cancel = m_cancel]() {
        Client client(cancel, false);
        const QString error = tracker->update(&client, fromMap(map));
        if (cancel.isCancelled()) return;
        QMetaObject::invokeMethod(this, [this, name, showLink, error, map]() {
            emit pushFinished(name, showLink, error.isEmpty(), error, map, false);
        }, Qt::QueuedConnection);
    }));
}

int TrackerList::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : int(m_trackers.size());
}

QVariant TrackerList::data(const QModelIndex &index, int role) const {
    if (index.row() < 0 || index.row() >= m_trackers.size()) return {};
    Tracker *tracker = m_trackers.at(index.row());
    switch (role) {
    case NameRole:          return tracker->name();
    case AuthenticatedRole: return tracker->isAuthenticated();
    case AccountRole:       return tracker->accountName();
    default:                return {};
    }
}

QHash<int, QByteArray> TrackerList::roleNames() const {
    return {{NameRole, "name"}, {AuthenticatedRole, "authenticated"}, {AccountRole, "account"}};
}
