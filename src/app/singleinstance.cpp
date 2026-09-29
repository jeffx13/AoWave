#include "app/singleinstance.h"

#include <QCryptographicHash>
#include <QLocalSocket>
#include "platform/platform.h"

SingleInstance::SingleInstance(const QString &key, QObject *parent) : QObject(parent) {
    // Pipe names are machine-wide, so another user's copy of the same folder must not answer.
    const QByteArray seed = (key.toLower() + QLatin1Char('|') + qEnvironmentVariable("USERNAME")).toUtf8();
    m_name = QStringLiteral(APP_NAME "-")
             + QString::fromLatin1(QCryptographicHash::hash(seed, QCryptographicHash::Sha1).toHex().left(16));
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&m_server, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket *socket = m_server.nextPendingConnection()) {
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
                if (!socket->canReadLine()) return;
                emit argumentReceived(QString::fromUtf8(socket->readLine()).chopped(1));
                socket->disconnectFromServer();
            });
        }
    });
}

bool SingleInstance::handOff(const QString &argument) {
    QLocalSocket socket;
    socket.connectToServer(m_name);
    if (!socket.waitForConnected(500)) return false;
    // This launch holds the foreground right; the running copy needs it to come to the front.
    Platform::allowForegroundHandoff();
    socket.write(argument.toUtf8() + '\n');
    // Not waitForBytesWritten: a write the pipe took at once leaves nothing to wait for, and
    // that reads as failure. Disconnecting flushes, then settles.
    socket.disconnectFromServer();
    return socket.state() == QLocalSocket::UnconnectedState || socket.waitForDisconnected(2000);
}

bool SingleInstance::listen() {
    if (m_server.listen(m_name)) return true;
    // A crash can leave the name behind where the OS keeps it as a file.
    QLocalServer::removeServer(m_name);
    return m_server.listen(m_name);
}
