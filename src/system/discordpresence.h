#pragma once
#include <QObject>
#include <QLocalSocket>
#include <QTimer>
#include <QByteArray>
#include <QDateTime>
#include <QJsonObject>

class PlaylistItem;

class DiscordPresence : public QObject {
    Q_OBJECT
public:
    explicit DiscordPresence(QObject *parent = nullptr);
    ~DiscordPresence();

    enum class Playback { Stopped, Playing, Paused };

    void onCurrentItemChanged(PlaylistItem *item, qint64 now = QDateTime::currentSecsSinceEpoch());
    void setPlayback(Playback state, qint64 now = QDateTime::currentSecsSinceEpoch());
    QJsonObject activity() const;

private:
    bool presenceEnabled() const;
    void tryConnect();
    void closeConnection();
    void scheduleReconnect();
    void onConnected();
    void onReadyRead();
    void onDisconnected();
    void onSocketError(QLocalSocket::LocalSocketError);
    void sendFrame(qint32 op, const QJsonObject &payload);
    void updateActivity();

    QLocalSocket m_socket;
    QTimer       m_reconnectTimer;
    QByteArray   m_buffer;
    QString      m_clientId;
    int          m_pipeIndex = 0;
    bool         m_ready = false;

    QString  m_details;
    QString  m_state;
    Playback m_playback = Playback::Stopped;
    qint64   m_playedSecs = 0;
    qint64   m_segmentStart = 0;
    bool     m_private = false;
};
