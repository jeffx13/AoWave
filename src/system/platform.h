#pragma once
#include <QList>
#include <QString>
#include <QtGlobal>
#include <functional>
#include <QByteArray>
#include <QLocalServer>
#include <QObject>

namespace Platform {

// Secrets at rest: DPAPI on Windows, keyed to the logged-in user, worthless on another account or machine.

QString protect(const QString &plain);

QString unprotect(const QString &blob);

// AOWAVE_QUIET=1: no sound, Discord, media keys, notifications, jump list, auto-PiP or shader warm-up; nothing raised.
bool quiet();

void keepDisplayAwake();
void allowDisplaySleep();

bool onBattery();

void closeSocket(qintptr handle);
// Makes the next close of the socket a reset, so the peer sees an error and not a clean end.
void resetOnClose(qintptr handle);

void allowForegroundHandoff();

void notify(const QString &title, const QString &message, std::function<void()> onClicked = {});

struct JumpItem {
    QString title, argument;
    bool operator==(const JumpItem &) const = default;
};
void setJumpList(const QList<JumpItem> &tasks, const QString &category, const QList<JumpItem> &recent);

void setOpenWith(const QStringList &extensions, bool on);
bool openWithRegistered();

double coveredFraction(quintptr window);
void setTopmost(quintptr window, bool topmost);
void restoreWithoutFocus(quintptr window);
void setRoundedCorners(quintptr window, bool rounded);
bool focusPreviousWindow(quintptr window);

QString exeDir();

bool replaceFile(const QString &from, const QString &to, QString *error = nullptr);

// Qt, Chromium, yt-dlp, deno and CUDA caches go under `cacheDir`, not AppData; call first (QML reads its path once).
void keepStateInAppFolder(const QString &cacheDir);
QString moduleDir();

}

namespace Aes {

QByteArray ctr(const QByteArray &key, QByteArray counter, const QByteArray &input);

QByteArray gcmDecrypt(const QByteArray &key, const QByteArray &iv, const QByteArray &ctWithTag);
QByteArray gcmEncrypt(const QByteArray &key, const QByteArray &iv, const QByteArray &plaintext);

QByteArray cbcDecrypt(const QByteArray &key, const QByteArray &iv, const QByteArray &input);

}

namespace CrashHandler {

void install();

void reportPending();

QString crashDir();

}

class SingleInstance : public QObject {
    Q_OBJECT
public:
    explicit SingleInstance(const QString &key, QObject *parent = nullptr);

    bool handOff(const QString &argument);
    bool listen();

signals:
    void argumentReceived(const QString &argument);

private:
    QString m_name;
    QLocalServer m_server;
};
