#include "app/logger.h"
#include "app/settings.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QMutex>
#include <QRegularExpression>
#include <mutex>

void LogListModel::append(const QString &type, const QString &message, const QString &level, bool request) {
    if (m_entries.size() >= kMaxEntries) {
        beginRemoveRows({}, 0, 0);
        m_entries.removeFirst();
        endRemoveRows();
    }
    beginInsertRows({}, m_entries.size(), m_entries.size());
    m_entries.append({QDateTime::currentDateTime().toString("hh:mm:ss"), type, message, level, request});
    endInsertRows();
}

void LogListModel::clear() {
    beginResetModel();
    m_entries.clear();
    endResetModel();
}

int LogListModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : m_entries.size();
}

QVariant LogListModel::data(const QModelIndex &index, int role) const {
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

QHash<int, QByteArray> LogListModel::roleNames() const {
    return {{TimeRole, "time"}, {TypeRole, "type"}, {MessageRole, "message"},
            {LevelRole, "level"}, {RequestRole, "request"}};
}

LogFilterModel::LogFilterModel(QObject *parent) : QSortFilterProxyModel(parent) {
    for (auto signal : {&QAbstractItemModel::rowsInserted, &QAbstractItemModel::rowsRemoved})
        connect(this, signal, this, &LogFilterModel::countChanged);
    connect(this, &QAbstractItemModel::modelReset, this, &LogFilterModel::countChanged);
}

int LogFilterModel::bitFor(const QString &level) {
    if (level == QLatin1String("error")) return Error;
    if (level == QLatin1String("warn"))  return Warn;
    if (level == QLatin1String("ok"))    return Ok;
    if (level == QLatin1String("step"))  return Step;
    if (level == QLatin1String("raw"))   return Raw;
    return Info;
}

template <typename T, typename U>
void LogFilterModel::applyFilter(T &field, const U &value) {
    if (field == T(value)) return;
    beginFilterChange();
    field = T(value);
    emit filtersChanged();
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
}

void LogFilterModel::setLevels(int levels)     { applyFilter(m_levels, levels); }
void LogFilterModel::setRequests(bool enabled) { applyFilter(m_requests, enabled); }
void LogFilterModel::setMpv(bool enabled)      { applyFilter(m_mpv, enabled); }

bool LogFilterModel::levelEnabled(const QString &level) const { return (m_levels & bitFor(level)) != 0; }

void LogFilterModel::setLevelEnabled(const QString &level, bool enabled) {
    const int bit = bitFor(level);
    applyFilter(m_levels, enabled ? (m_levels | bit) : (m_levels & ~bit));
}

QVariantMap LogFilterModel::entry(int row) const {
    const QModelIndex at = index(row, 0);
    return {{QStringLiteral("time"),    at.data(LogListModel::TimeRole)},
            {QStringLiteral("type"),    at.data(LogListModel::TypeRole)},
            {QStringLiteral("message"), at.data(LogListModel::MessageRole)},
            {QStringLiteral("level"),   at.data(LogListModel::LevelRole)},
            {QStringLiteral("request"), at.data(LogListModel::RequestRole)}};
}

bool LogFilterModel::filterAcceptsRow(int row, const QModelIndex &parent) const {
    const QModelIndex entry = sourceModel()->index(row, 0, parent);
    if (entry.data(LogListModel::RequestRole).toBool()) return m_requests;
    if (entry.data(LogListModel::LevelRole).toString() == QLatin1String("raw")) return m_mpv;
    return (m_levels & bitFor(entry.data(LogListModel::LevelRole).toString())) != 0;
}

LogListModel Log::logListModel{};

namespace {

constexpr int kLabelWidth       = 14;
constexpr int kFlushEvery       = 64;
constexpr int kFlushIntervalMs  = 250;

int s_unflushed = 0;
QElapsedTimer s_flushClock = [] { QElapsedTimer t; t.start(); return t; }();
QtMessageHandler s_previousHandler = nullptr;

QMutex &fileMutex() { static QMutex mutex; return mutex; }
QFile  &logFile()   { static QFile file; return file; }

const char *levelName(Log::Colour c) {
    switch (c) {
    case Log::Green:   return "ok";
    case Log::Red:     return "error";
    case Log::Yellow:  return "step";
    case Log::Orange:  return "warn";
    case Log::Magenta: return "raw";
    case Log::Cyan:    return "info";
    }
    return "info";
}

QString centred(const QString &text, int width) {
    const int left = qMax(0, (width - int(text.size())) / 2);
    const int right = qMax(0, width - left - int(text.size()));
    return QString(left, ' ') + text + QString(right, ' ');
}

void rotate(const QString &path) {
    if (!QFile::exists(path)) return;
    const QString previous = path + QLatin1String(".1");
    QFile::remove(previous);
    QFile::rename(path, previous);
}

void writeToFile(const QString &type, const QString &message) {
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
    if (++s_unflushed >= kFlushEvery || s_flushClock.elapsed() >= kFlushIntervalMs) {
        file.flush();
        s_unflushed = 0;
        s_flushClock.restart();
    }
}

// Log writes through qDebug, so debug and info pass on untouched, or every log line would come back here.
// A hidden web page's console is its own noise: its errors reach the previous handler, nothing else does.
void onQtMessage(QtMsgType type, const QMessageLogContext &context, const QString &message) {
    if (type == QtWarningMsg && Log::isClipboardRetry(context, message)) return;
    const bool page = Log::isPageConsole(context);
    if (page && type != QtCriticalMsg && type != QtFatalMsg) return;
    if (s_previousHandler) s_previousHandler(type, context, message);
    if ((type == QtWarningMsg || type == QtCriticalMsg) && !page && QCoreApplication::instance())
        Log(Log::Orange) << "Qt" << message;
}

}

Log::Log(Colour colour)
    : m_debug(qDebug().noquote().nospace())
    , m_level(QString::fromLatin1(levelName(colour)))
{
    m_debug << QString("\033[%1m[").arg(colour);
}

Log::~Log() {
    m_debug << " \033[0m";
    const QString plain = m_fields.join(' ');
    bool isRequest = false;
    for (const char *verb : {"GET", "POST", "HEAD", "PUT"})
        isRequest = isRequest || m_type.startsWith(QLatin1String(verb));
    QMetaObject::invokeMethod(&logListModel, "append", Q_ARG(QString, m_type),
                              Q_ARG(QString, isRequest ? m_fields.value(0) : plain),
                              Q_ARG(QString, m_level), Q_ARG(bool, isRequest));
    writeToFile(m_type, plain);
}

void Log::add(const QString &text) {
    const QString str = oneLine(maskSecrets(text));
    if (!m_haveType) {
        m_haveType = true;
        m_type = str;
        m_debug << centred(str, kLabelWidth) << "]";
    } else {
        m_fields << str;
        m_debug << " " << str;
    }
}

void Log::forwardQtMessages() { s_previousHandler = qInstallMessageHandler(onQtMessage); }

bool Log::isClipboardRetry(const QMessageLogContext &context, const QString &message) {
    return context.category && qstrcmp(context.category, "qt.qpa.mime") == 0
           && message.startsWith(QLatin1String("Retrying to obtain clipboard"));
}

bool Log::isPageConsole(const QMessageLogContext &context) {
    return context.category && qstrcmp(context.category, "js") == 0;
}

QString Log::maskSecrets(QString text) {
    if (!text.contains(QLatin1Char('=')) && !text.contains(QLatin1Char('"')) && !text.contains(QLatin1String("Bearer"))) return text;
    static const QRegularExpression secret(
        QStringLiteral("((?:^|[?&;\\s])(?:api_?key|access_key|access_token|refresh_token|token|sign|sessdata|bili_jct|password|autologin)=)[^&;\\s\"']+"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression json(
        QStringLiteral("(\"(?:api_?key|access_token|refresh_token|token|sign|sessdata|bili_jct|password|autologinkey)\"\\s*:\\s*\")[^\"]*"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression bearer(QStringLiteral("(Bearer\\s+)[A-Za-z0-9._~+/=-]+"), QRegularExpression::CaseInsensitiveOption);
    return text.replace(secret, QStringLiteral("\\1***")).replace(json, QStringLiteral("\\1***")).replace(bearer, QStringLiteral("\\1***"));
}

QString Log::oneLine(const QString &text) {
    for (QChar c : text)
        if (c == u'\n' || c == u'\r' || c == QChar::LineSeparator || c == QChar::ParagraphSeparator)
            return text.simplified();
    return text;
}

void Log::flushLog(int waitMs) {
    std::unique_lock<QMutex> lock(fileMutex(), std::defer_lock);
    if (waitMs < 0) lock.lock();
    else if (!lock.try_lock_for(std::chrono::milliseconds(waitMs))) return;
    QFile &file = logFile();
    if (file.isOpen()) file.flush();
    s_unflushed = 0;
}
