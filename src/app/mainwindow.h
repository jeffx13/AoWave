#pragma once
#include <QString>
#include <QObject>
#include <QEvent>
#include <QMouseEvent>
#include <QWindow>
#include <QQuickWindow>
#include <QQuickItem>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QDateTime>
#include <QThread>
#include <QTimer>
#include "app/qmlsingleton.h"
#include "app/logger.h"

class MainWindow : public QObject
{
    Q_OBJECT
public:
    enum Page {
        Explore = 0,
        Details = 1,
        Library = 2,
        Player = 3,
        Downloads = 4,
        Logs = 5,
        Settings = 6,
        History = 7
    };
    Q_ENUM(Page)

public:
    void reportError(const QString &message, const QString &header = "Error") {
        report(message, header, Error);
    }

    void reportInfo(const QString &message, const QString &header = "Info") {
        report(message, header, Info);
    }

    void reportWarning(const QString &message, const QString &header, const QString &action = QString()) {
        report(message, header, Warning, action);
    }

    Q_INVOKABLE void notificationClosed() { m_onScreen.clear(); }

    void navigateTo(Page page) {
        emit navigateRequested(page);
    }

    void installPointerFilter() { QCoreApplication::instance()->installEventFilter(this); }

    Q_INVOKABLE void replayRightClick(qint64 openedAt) {
        if (m_rightPress.at <= openedAt || QDateTime::currentMSecsSinceEpoch() - m_rightPress.at > 1000) return;
        m_rightPress.at = 0;
        // the window under the press, since a menu in a window of its own takes the press itself
        QTimer::singleShot(0, this, [global = m_rightPress.global] {
            QWindow *window = QGuiApplication::topLevelAt(global.toPoint());
            if (!window) return;
            const QPointF position = window->mapFromGlobal(global);
            QMouseEvent down(QEvent::MouseButtonPress, position, global, Qt::RightButton, Qt::RightButton, Qt::NoModifier);
            QMouseEvent up(QEvent::MouseButtonRelease, position, global, Qt::RightButton, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(window, &down);
            QCoreApplication::sendEvent(window, &up);
            if (!up.isAccepted()) {
                QContextMenuEvent menu(QContextMenuEvent::Mouse, position.toPoint(), global.toPoint());
                QCoreApplication::sendEvent(window, &menu);
            }
        });
    }

private:
    enum Kind { Info, Warning, Error };
    void report(const QString &message, const QString &header, Kind kind, const QString &action = QString()) {
        if (QThread::currentThread() != thread()) {
            QMetaObject::invokeMethod(this, [this, message, header, kind, action]() {
                report(message, header, kind, action);
            }, Qt::QueuedConnection);
            return;
        }
        const QString key = header + QLatin1Char('\x1f') + message;
        if (key == m_onScreen) return;
        m_onScreen = key;
        if (kind == Info) logInfo() << "Shown" << header + QStringLiteral(":") << message;
        else              logWarn() << "Shown" << header + QStringLiteral(":") << message;
        switch (kind) {
        case Error:   emit errorReported(message, header); break;
        case Warning: emit warningReported(message, header, action); break;
        case Info:    emit infoReported(message, header); break;
        }
    }

    QString m_onScreen;
    struct RightPress {
        QPointF global;
        qint64 at = 0;
    } m_rightPress;

public:
    static MainWindow &instance() {
        static MainWindow shell;
        return shell;
    }

signals:
    void errorReported(const QString &message, const QString &header);
    void infoReported(const QString &message, const QString &header);
    void warningReported(const QString &message, const QString &header, const QString &action);
    void navigateRequested(MainWindow::Page page);
    void historyStepRequested(int delta);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (event->type() != QEvent::MouseButtonPress || !qobject_cast<QWindow *>(watched))
            return false;
        const auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() == Qt::RightButton)
            m_rightPress = {mouse->globalPosition(), QDateTime::currentMSecsSinceEpoch()};
        if (mouse->button() == Qt::BackButton || mouse->button() == Qt::ForwardButton) {
            emit historyStepRequested(mouse->button() == Qt::ForwardButton ? 1 : -1);
            return true;
        }
        releaseTextFocus(qobject_cast<QQuickWindow *>(watched), mouse->scenePosition());
        return false;
    }

private:
    static void releaseTextFocus(QQuickWindow *window, const QPointF &scenePos) {
        QQuickItem *focus = window ? window->activeFocusItem() : nullptr;
        if (!focus || (!focus->inherits("QQuickTextInput") && !focus->inherits("QQuickTextEdit"))) return;
        if (!focus->contains(focus->mapFromScene(scenePos))) focus->setFocus(false);
    }

    MainWindow() = default;
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;
    ~MainWindow() = default;
};

DECLARE_QML_SINGLETON(MainWindow);
