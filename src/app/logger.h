#pragma once
#include <QAbstractListModel>
#include <QByteArray>
#include <QDebug>
#include <QSortFilterProxyModel>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <qqmlintegration.h>
#include <string>

class LogListModel : public QAbstractListModel {
    Q_OBJECT
    QML_ANONYMOUS
public:
    enum Role { TimeRole = Qt::UserRole, TypeRole, MessageRole, LevelRole, RequestRole };

    explicit LogListModel(QObject *parent = nullptr) : QAbstractListModel(parent) {}

    static constexpr int kMaxEntries = 5000;

    Q_INVOKABLE void append(const QString &type, const QString &message, const QString &level, bool request);
    Q_INVOKABLE void clear();

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

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
    enum Level {
        Error   = 0x1,
        Warn    = 0x2,
        Info    = 0x4,
        Ok      = 0x8,
        Step    = 0x10,
        Raw     = 0x20,
        AllLevels = Error | Warn | Info | Ok | Step | Raw,
    };
    Q_ENUM(Level)

    explicit LogFilterModel(QObject *parent = nullptr);

    static int bitFor(const QString &level);

    int  levels() const   { return m_levels; }
    bool requests() const { return m_requests; }
    bool mpv() const      { return m_mpv; }

    void setLevels(int levels);
    void setRequests(bool enabled);
    void setMpv(bool enabled);

    Q_INVOKABLE bool levelEnabled(const QString &level) const;
    Q_INVOKABLE void setLevelEnabled(const QString &level, bool enabled);
    Q_INVOKABLE QVariantMap entry(int row) const;

signals:
    void filtersChanged();
    void countChanged();

protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override;

private:
    template <typename T, typename U>
    void applyFilter(T &field, const U &value);

    int  m_levels   = AllLevels;
    bool m_requests = true;
    bool m_mpv      = true;
};

class Log {
public:
    enum Colour { Red = 31, Green = 32, Orange = 33, Magenta = 35, Cyan = 36, Yellow = 93 };

    static LogListModel logListModel;

    explicit Log(Colour colour);
    ~Log();

    Log(const Log &) = delete;
    Log &operator=(const Log &) = delete;

    static void forwardQtMessages();
    static bool isClipboardRetry(const QMessageLogContext &context, const QString &message);
    static bool isPageConsole(const QMessageLogContext &context);

    // Credentials never reach the console, log page or file: url keys, tokens, signatures, sign-in cookies.
    static QString maskSecrets(QString text);
    static QString oneLine(const QString &text);

    // waitMs >= 0 gives up after that long: the crash handler may run while this thread holds the lock.
    static void flushLog(int waitMs = -1);

    template<typename T>
    Log &operator<<(const T &value) {
        add(toString(value));
        return *this;
    }

    Log &operator<<(const char *v)        { return *this << QString::fromUtf8(v); }
    Log &operator<<(QStringView v)        { return *this << v.toString(); }
    Log &operator<<(const QByteArray &v)  { return *this << QString::fromUtf8(v); }
    Log &operator<<(const std::string &v) { return *this << QString::fromStdString(v); }

private:
    template<typename T>
    static QString toString(const T &value) {
        QString s;
        QDebug(&s).noquote().nospace() << value;
        return s;
    }

    void add(const QString &text);

    QDebug      m_debug;
    QString     m_level;
    QString     m_type;
    QStringList m_fields;
    bool        m_haveType = false;
};

#define logError() Log(Log::Red)
#define logWarn()  Log(Log::Orange)
#define logInfo()  Log(Log::Cyan)
#define logOk()    Log(Log::Green)
#define logStep()  Log(Log::Yellow)
#define logRaw()   Log(Log::Magenta)
