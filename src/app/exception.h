#pragma once
#include <QException>
#include "app/logger.h"
#include "app/mainwindow.h"

class AppException : public QException
{
public:
    AppException(const QString &message, const QString &header = "Error")
        : m_message(message), m_header(header), m_whatBuffer(message.toUtf8()) {}

    void raise() const override { throw *this; }
    QException* clone() const override { return new AppException(*this); }

    const char *what() const noexcept override {
        return m_whatBuffer.constData();
    }

    const QString &message() const { return m_message; }
    const QString &header() const { return m_header; }

    void report() const {
        MainWindow::instance().reportError(m_message, QObject::tr("%1 error").arg(m_header));
    }

    void log(const char *area) const {
        logWarn() << area << m_header + QLatin1Char(':') << m_message;
    }

private:
    QString m_message;
    QString m_header;
    QByteArray m_whatBuffer;
};
