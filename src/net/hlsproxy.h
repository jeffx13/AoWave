#pragma once
#include <QTcpServer>
#include <QThreadPool>
#include <QByteArray>
#include <QString>
#include "app/async.h"
#include <atomic>

class HlsProxy : public QTcpServer {
    Q_OBJECT
public:
    explicit HlsProxy(QObject *parent = nullptr);
    ~HlsProxy() override;
    static HlsProxy *instance() { return s_instance; }

    QString playlistUrl(const QString &m3u8Url, const QString &referer);

protected:
    void incomingConnection(qintptr handle) override;

private:
    void ensureListening();
    std::atomic<quint16> m_port{0};
    QByteArray  m_secret;
    CancelToken m_cancel;
    QThreadPool m_pool;
    static HlsProxy *s_instance;
};
