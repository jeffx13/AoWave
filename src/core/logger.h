#pragma once
#include <QAbstractListModel>
#include <QSortFilterProxyModel>
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QDebug>
#include <QFile>
#include <QMutex>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <qqmlintegration.h>
#include "core/settings.h"
#include <string>

class LogListModel : public QAbstractListModel {
    Q_OBJECT
    QML_ANONYMOUS
public:
    // level: error, warn, info, ok, step or raw. request: the message is a bare url.
    enum Role { TimeRole = Qt::UserRole, TypeRole, MessageRole, LevelRole, RequestRole };

    explicit LogListModel(QObject *parent = nullptr) : QAbstractListModel(parent) {}

    // Every network call logs a line, so an unbounded list is a session-long leak.
    static constexpr int kMaxEntries = 5000;

    Q_INVOKABLE void append(const QString &type, const QString &message, const QString &level, bool request) {
        if (m_entries.size() >= kMaxEntries) {
            beginRemoveRows({}, 0, 0);
            m_entries.removeFirst();
            endRemoveRows();
        }
        beginInsertRows({}, m_entries.size(), m_entries.size());
        m_entries.append({QDateTime::currentDateTime().toString("hh:mm:ss"), type, message, level, request});
        endInsertRows();
    }

    Q_INVOKABLE void clear() {
        beginResetModel();
        m_entries.clear();
        endResetModel();
    }

    int rowCount(const QModelIndex &parent = {}) const override {
        return parent.isValid() ? 0 : m_entries.size();
    }

    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override {
        if (index.row() < 0 || index.row() >= m_entries.size()) return {};
        const Entry &entry = m_entries.at(index.row());
        switch (role) {
        case TimeRole:    return entry.time;
        case TypeRole:    return entry.type;
        case MessageRole: return entry.message;
        case LevelRole:   return entry.level;
        case RequestRole: return entry.request;
        default:          return {};
        }
    }

    QHash<int, QByteArray> roleNames() const override {
        return {{TimeRole, "time"}, {TypeRole, "type"}, {MessageRole, "message"},
                {LevelRole, "level"}, {RequestRole, "request"}};
    }

private:
    struct Entry { QString time, type, message, level; bool request; };
    QList<Entry> m_entries;
};

class LogFilterModel : public QSortFilterProxyModel {
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(int  levels   READ levels   WRITE setLevels   NOTIFY filtersChanged)
    Q_PROPERTY(bool requests READ requests WRITE setRequests NOTIFY filtersChanged)
    Q_PROPERTY(bool mpv      READ mpv      WRITE setMpv      NOTIFY filtersChanged)
    Q_PROPERTY(int  count    READ rowCount NOTIFY countChanged)
public:
    // A bitmask, so the log page offers one checkbox per level.
    enum Level {
        NoLevel = 0x0,
        Error   = 0x1,
        Warn    = 0x2,
        Info    = 0x4,
        Ok      = 0x8,
        Step    = 0x10,
        Raw     = 0x20,
        AllLevels = Error | Warn | Info | Ok | Step | Raw,
    };
    Q_ENUM(Level)

    explicit LogFilterModel(QObject *parent = nullptr) : QSortFilterProxyModel(parent) {
        for (auto signal : {&QAbstractItemModel::rowsInserted, &QAbstractItemModel::rowsRemoved})
            connect(this, signal, this, &LogFilterModel::countChanged);
        connect(this, &QAbstractItemModel::modelReset, this, &LogFilterModel::countChanged);
    }

    static int bitFor(const QString &level) {
        if (level == QLatin1String("error")) return Error;
        if (level == QLatin1String("warn"))  return Warn;
        if (level == QLatin1String("ok"))    return Ok;
        if (level == QLatin1String("step"))  return Step;
        if (level == QLatin1String("raw"))   return Raw;
        return Info;
    }

    int  levels() const   { return m_levels; }
    bool requests() const { return m_requests; }
    bool mpv() const      { return m_mpv; }

    void setLevels(int levels)     { applyFilter(m_levels, levels); }
    void setRequests(bool enabled) { applyFilter(m_requests, enabled); }
    void setMpv(bool enabled)      { applyFilter(m_mpv, enabled); }

    // The model is QML_ANONYMOUS, so QML has no type to read an enum off.
    Q_INVOKABLE bool levelEnabled(const QString &level) const { return (m_levels & bitFor(level)) != 0; }
    Q_INVOKABLE void setLevelEnabled(const QString &level, bool enabled) {
        const int bit = bitFor(level);
        applyFilter(m_levels, enabled ? (m_levels | bit) : (m_levels & ~bit));
    }

signals:
    void filtersChanged();
    void countChanged();

protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override {
        const QModelIndex entry = sourceModel()->index(row, 0, parent);
        if (entry.data(LogListModel::RequestRole).toBool()) return m_requests;
        // mpv relays are the one source noisy enough to want their own switch.
        if (!m_mpv && entry.data(LogListModel::TypeRole).toString() == QLatin1String("MPV"))
            return false;
        return (m_levels & bitFor(entry.data(LogListModel::LevelRole).toString())) != 0;
    }

private:
    template <typename T, typename U>
    void applyFilter(T &field, const U &value) {
        if (field == T(value)) return;
        beginFilterChange();
        field = T(value);
        emit filtersChanged();
        endFilterChange(QSortFilterProxyModel::Direction::Rows);
    }

    int  m_levels   = AllLevels;
    bool m_requests = true;
    bool m_mpv      = true;
};

class QLog {
public:
    enum Colour { Red = 31, Green = 32, Orange = 33, Magenta = 35, Cyan = 36, Yellow = 93 };

    inline static LogListModel logListModel{};

    explicit QLog(Colour colour)
        : m_debug(qDebug().noquote().nospace())
        , m_level(QString::fromLatin1(levelName(colour)))
    {
        m_debug << QString("\033[%1m[").arg(colour);
    }

    ~QLog() {
        m_debug << " \033[0m";
        const QString plain = m_fields.join(' ');
        // A request line is just a url.
        const bool isRequest = m_type.startsWith("GET") || m_type.startsWith("POST");
        QMetaObject::invokeMethod(&logListModel, "append", Q_ARG(QString, m_type),
                                  Q_ARG(QString, isRequest ? m_fields.value(0) : plain),
                                  Q_ARG(QString, m_level), Q_ARG(bool, isRequest));
        writeToFile(m_type, plain);
    }

    QLog(const QLog &) = delete;
    QLog &operator=(const QLog &) = delete;

    // Credentials never reach the console, the log page or the file: a url's key, token or
    // signature, or a cookie that signs in. Network errors quote whole urls too.
    static QString maskSecrets(QString text) {
        if (!text.contains(QLatin1Char('='))) return text;
        static const QRegularExpression secret(
            QStringLiteral("((?:^|[?&;\\s])(?:api_?key|access_key|access_token|token|sign|sessdata|bili_jct|password)=)[^&;\\s\"']+"),
            QRegularExpression::CaseInsensitiveOption);
        return text.replace(secret, QStringLiteral("\\1***"));
    }

    template<typename T>
    QLog &operator<<(const T &value) {
        const QString str = maskSecrets(toString(value));
        if (!m_haveType) {
            m_haveType = true;
            m_type = str;
            m_debug << centred(str, kLabelWidth) << "]";
        } else {
            m_fields << str;
            m_debug << " " << str;
        }
        return *this;
    }

    QLog &operator<<(const char *v)        { return *this << QString::fromUtf8(v); }
    QLog &operator<<(QStringView v)        { return *this << v.toString(); }
    QLog &operator<<(const QByteArray &v)  { return *this << QString::fromUtf8(v); }
    QLog &operator<<(const std::string &v) { return *this << QString::fromStdString(v); }

private:
    static constexpr int kLabelWidth = 14;

    // One generation kept: a crash report names a log, and truncating destroys it.
    static void rotate(const QString &path) {
        if (!QFile::exists(path)) return;
        const QString previous = path + QLatin1String(".1");
        QFile::remove(previous);
        QFile::rename(path, previous);
    }

    static void writeToFile(const QString &type, const QString &message) {
        QMutexLocker lock(&fileMutex());
        QFile &file = logFile();
        if (!file.isOpen()) {
            const QString path = Settings::logDir() + QLatin1Char('/')
                                 + QCoreApplication::applicationName() + QStringLiteral(".log");
            rotate(path);
            file.setFileName(path);
            if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
                return;
        }
        file.write((QDateTime::currentDateTime().toString("hh:mm:ss") + " " +
                    type + " " + message + "\n").toUtf8());
        // A flush per line would be a flush per request.
        if (++s_unflushed >= kFlushEvery || s_flushClock.elapsed() >= kFlushIntervalMs) {
            file.flush();
            s_unflushed = 0;
            s_flushClock.restart();
        }
    }

public:
    // Called from the crash handler.
    static void flushLog() {
        QMutexLocker lock(&fileMutex());
        QFile &file = logFile();
        if (file.isOpen()) file.flush();
        s_unflushed = 0;
    }

private:
    static QMutex &fileMutex() { static QMutex mutex; return mutex; }
    static QFile  &logFile()   { static QFile file; return file; }

    static constexpr int kFlushEvery      = 64;
    static constexpr int kFlushIntervalMs = 250;
    static inline int s_unflushed = 0;
    static inline QElapsedTimer s_flushClock = [] { QElapsedTimer t; t.start(); return t; }();

    static const char *levelName(Colour c) {
        switch (c) {
        case Green:   return "ok";
        case Red:     return "error";
        case Yellow:  return "step";
        case Orange:  return "warn";
        case Magenta: return "raw";
        case Cyan:    return "info";
        }
        return "info";
    }

    static QString centred(const QString &text, int width) {
        const int left = qMax(0, (width - int(text.size())) / 2);
        const int right = qMax(0, width - left - int(text.size()));
        return QString(left, ' ') + text + QString(right, ' ');
    }

    template<typename T>
    static QString toString(const T &value) {
        QString s;
        QDebug(&s).noquote().nospace() << value;
        return s;
    }

    QDebug      m_debug;
    QString     m_level;
    QString     m_type;
    QStringList m_fields;
    bool        m_haveType = false;
};

#define logError() QLog(QLog::Red)
#define logWarn()  QLog(QLog::Orange)
#define logInfo()  QLog(QLog::Cyan)
#define logOk()    QLog(QLog::Green)
#define logStep()  QLog(QLog::Yellow)   // a stage of a slow external workaround
#define logRaw()   QLog(QLog::Magenta)  // relayed verbatim from an external process
