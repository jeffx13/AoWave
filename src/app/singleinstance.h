#pragma once
#include <QLocalServer>
#include <QObject>

// One running copy per install: a second launch hands its argument to the first and exits.
class SingleInstance : public QObject {
    Q_OBJECT
public:
    // Keyed per install folder, since each keeps its own settings and library.
    explicit SingleInstance(const QString &key, QObject *parent = nullptr);

    // True when a running copy took `argument`; this process should then exit.
    bool handOff(const QString &argument);
    // Otherwise, become the copy later launches hand off to.
    bool listen();

signals:
    // Empty when the second launch had nothing to open: just come to the front.
    void argumentReceived(const QString &argument);

private:
    QString m_name;
    QLocalServer m_server;
};
