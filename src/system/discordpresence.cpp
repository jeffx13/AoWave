#include "system/discordpresence.h"
#include "system/platform.h"
#include "providers/showdata.h"
#include "app/settings.h"
#include "app/logger.h"
#include <QCoreApplication>
#include <QDataStream>
#include <QDateTime>
#include <QJsonDocument>
#include <cmath>

DiscordPresence::DiscordPresence(QObject *parent) : QObject(parent) {
    m_clientId = Settings::instance().get(Config::DiscordClientId);

    connect(&m_socket, &QLocalSocket::connected,     this, &DiscordPresence::onConnected);
    connect(&m_socket, &QLocalSocket::readyRead,     this, &DiscordPresence::onReadyRead);
    connect(&m_socket, &QLocalSocket::disconnected,  this, &DiscordPresence::onDisconnected);
    connect(&m_socket, &QLocalSocket::errorOccurred, this, &DiscordPresence::onSocketError);

    m_reconnectTimer.setInterval(15000);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &DiscordPresence::tryConnect);

    auto &settings = Settings::instance();
    connect(&settings, &Settings::discordEnabledChanged, this, [this]() {
        if (presenceEnabled()) tryConnect();
        else                   closeConnection();
    });
    m_private = settings.get(Config::DiscordPrivate);
    connect(&settings, &Settings::settingsChanged, this, [this]() {
        const bool hidden = Settings::instance().get(Config::DiscordPrivate);
        if (hidden != std::exchange(m_private, hidden)) updateActivity();
        const QString id = Settings::instance().get(Config::DiscordClientId);
        if (id == m_clientId) return;
        m_clientId = id;
        closeConnection();
        if (presenceEnabled()) tryConnect();
    });

    if (presenceEnabled()) tryConnect();
}

DiscordPresence::~DiscordPresence() {
    disconnect(&m_socket, nullptr, this, nullptr);
}

bool DiscordPresence::presenceEnabled() const {
    return !Platform::quiet() && Settings::instance().get(Config::DiscordEnabled) && !m_clientId.isEmpty();
}

void DiscordPresence::tryConnect() {
    if (!presenceEnabled()) return;
    if (m_socket.state() != QLocalSocket::UnconnectedState) return;
    m_reconnectTimer.stop();
    m_pipeIndex = 0;
    m_socket.connectToServer(QStringLiteral("discord-ipc-0"));
}

void DiscordPresence::closeConnection() {
    m_reconnectTimer.stop();
    m_ready = false;
    m_buffer.clear();
    m_pipeIndex = 0;
    if (m_socket.state() != QLocalSocket::UnconnectedState)
        m_socket.abort();
}

void DiscordPresence::scheduleReconnect() {
    m_ready = false;
    m_buffer.clear();
    if (presenceEnabled() && !m_reconnectTimer.isActive())
        m_reconnectTimer.start();
}

void DiscordPresence::onConnected() {
    m_reconnectTimer.stop();
    m_ready = false;
    m_buffer.clear();
    sendFrame(0, QJsonObject{{"v", 1}, {"client_id", m_clientId}});
}

void DiscordPresence::onReadyRead() {
    m_buffer.append(m_socket.readAll());
    while (m_buffer.size() >= 8) {
        qint32 op = 0, len = 0;
        { QDataStream hs(m_buffer.left(8)); hs.setByteOrder(QDataStream::LittleEndian); hs >> op >> len; }
        if (len < 0 || m_buffer.size() < 8 + len) break;
        const QByteArray payload = m_buffer.mid(8, len);
        m_buffer.remove(0, 8 + len);

        if (op == 2) {
            logWarn() << "App" << QStringLiteral("Discord:") << "presence rejected:"
                   << QJsonDocument::fromJson(payload).object().value("message").toString();
            closeConnection();
            return;
        }
        if (!m_ready) {
            m_ready = true;
            updateActivity();
        }
    }
}

void DiscordPresence::onDisconnected() {
    scheduleReconnect();
}

void DiscordPresence::onSocketError(QLocalSocket::LocalSocketError) {
    if (!m_ready && m_pipeIndex < 9) {
        m_socket.connectToServer(QStringLiteral("discord-ipc-") + QString::number(++m_pipeIndex));
        return;
    }
    scheduleReconnect();
}

void DiscordPresence::sendFrame(qint32 op, const QJsonObject &payload) {
    if (m_socket.state() != QLocalSocket::ConnectedState) return;
    const QByteArray data = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    QByteArray header;
    { QDataStream hs(&header, QIODevice::WriteOnly); hs.setByteOrder(QDataStream::LittleEndian);
      hs << op << static_cast<qint32>(data.size()); }
    m_socket.write(header);
    m_socket.write(data);
    m_socket.flush();
}

void DiscordPresence::onCurrentItemChanged(PlaylistItem *item, qint64 now) {
    if (!item || item->isList()) {
        m_details.clear();
        m_state.clear();
    } else {
        auto parent = item->parent();
        m_details = parent && !parent->name.isEmpty() ? parent->name : item->displayName.simplified();
        if (item->number > 0) {
            const float n = item->number;
            m_state = tr("Episode %1").arg(std::floor(n) == n ? QString::number(static_cast<int>(n))
                                                              : QString::number(n, 'f', 1));
            if (item->season > 0)
                m_state = tr("Season %1").arg(item->season) + QLatin1Char(' ') + m_state;
            const QString episodeName = item->name.simplified();
            if (!episodeName.isEmpty())
                m_state += QStringLiteral(" - ") + episodeName;
        } else {
            m_state = item->displayName.simplified();
        }
    }
    m_playedSecs = 0;
    m_segmentStart = now;

    updateActivity();
    if (m_socket.state() == QLocalSocket::UnconnectedState) tryConnect();
}

void DiscordPresence::setPlayback(Playback state, qint64 now) {
    if (state == m_playback) return;
    if (m_playback == Playback::Playing) m_playedSecs += now - m_segmentStart;
    if (state == Playback::Playing) m_segmentStart = now;
    m_playback = state;
    updateActivity();
    if (m_socket.state() == QLocalSocket::UnconnectedState) tryConnect();
}

QJsonObject DiscordPresence::activity() const {
    if (m_details.isEmpty() || m_playback == Playback::Stopped) return {};
    QJsonObject activity;
    activity["details"] = m_private ? tr("Watching anime") : m_details;
    if (!m_private && !m_state.isEmpty()) activity["state"] = m_state;
    activity["assets"] = QJsonObject{{"large_image", "logo"}, {"large_text", APP_NAME}};
    if (m_playback == Playback::Playing)
        activity["timestamps"] = QJsonObject{{"start", m_segmentStart - m_playedSecs}};
    return activity;
}

void DiscordPresence::updateActivity() {
    if (!m_ready) return;

    const QJsonObject current = activity();
    QJsonObject args{{"pid", static_cast<int>(QCoreApplication::applicationPid())},
                     {"activity", current.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(current)}};
    QJsonObject frame{
        {"cmd", "SET_ACTIVITY"},
        {"args", args},
        {"nonce", QString::number(QDateTime::currentMSecsSinceEpoch())}
    };
    sendFrame(1, frame);
}
