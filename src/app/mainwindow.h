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
#include <QDateTime>
#include <QPointer>
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
    // Any thread. A message already on screen is not raised again, so a repeating error cannot stack.
    void reportError(const QString &message, const QString &header = "Error") {
        report(message, header, true);
    }

    void reportInfo(const QString &message, const QString &header = "Info") {
        report(message, header, false);
    }

    Q_INVOKABLE void notificationClosed() { m_onScreen.clear(); }

    void navigateTo(Page page) {
        emit navigateRequested(page);
    }

    void installPointerFilter() { QCoreApplication::instance()->installEventFilter(this); }

    Q_INVOKABLE void replayRightClick(qint64 openedAt) {
        if (!m_rightPress.window || m_rightPress.at <= openedAt
            || QDateTime::currentMSecsSinceEpoch() - m_rightPress.at > 1000)
            return;
        m_rightPress.at = 0;
        QTimer::singleShot(0, this, [press = m_rightPress] {
            if (!press.window) return;
            const QPointF global = press.window->mapToGlobal(press.position);
            QMouseEvent down(QEvent::MouseButtonPress, press.position, global, Qt::RightButton, Qt::RightButton, Qt::NoModifier);
            QMouseEvent up(QEvent::MouseButtonRelease, press.position, global, Qt::RightButton, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(press.window, &down);
            QCoreApplication::sendEvent(press.window, &up);
            if (!up.isAccepted()) {
                QContextMenuEvent menu(QContextMenuEvent::Mouse, press.position.toPoint(), global.toPoint());
                QCoreApplication::sendEvent(press.window, &menu);
            }
        });
    }

private:
    void report(const QString &message, const QString &header, bool error) {
        if (QThread::currentThread() != thread()) {
            QMetaObject::invokeMethod(this, [this, message, header, error]() {
                report(message, header, error);
            }, Qt::QueuedConnection);
            return;
        }
        const QString key = header + QLatin1Char('\x1f') + message;
        if (key == m_onScreen) return;
        m_onScreen = key;
        if (error) logWarn() << "Shown" << header + QStringLiteral(":") << message;
        else       logInfo() << "Shown" << header + QStringLiteral(":") << message;
        if (error) emit errorReported(message, header);
        else       emit infoReported(message, header);
    }

    QString m_onScreen;
    struct RightPress {
        QPointer<QWindow> window;
        QPointF position;
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
    void navigateRequested(MainWindow::Page page);
    void historyStepRequested(int delta);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (event->type() != QEvent::MouseButtonPress || !qobject_cast<QWindow *>(watched))
            return false;
        const auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() == Qt::RightButton)
            m_rightPress = {qobject_cast<QWindow *>(watched), mouse->position(), QDateTime::currentMSecsSinceEpoch()};
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
