#include "library/library.h"
#include <QSqlRecord>
#include "library/store.h"
#include "shows/providerlist.h"
#include "shows/showprovider.h"
#include "net/client.h"
#include "core/appshell.h"
#include "core/async.h"
#include "core/logger.h"
#include "core/settings.h"
#include "core/exception.h"
#include <QDir>
#include <QFileInfo>
#include <QCoreApplication>
#include <QVariant>
#include <QDateTime>
#include <QMutex>
#include <QSet>
#include <QThreadPool>
#include <algorithm>
#include <QRegularExpression>
#include <cmath>

// Qt keys its connection pool on this; it must not follow the display name.
static const QString kConnectionName = QStringLiteral("library");

void HistoryModel::setRows(QList<Row> rows) {
    bool sameShape = rows.size() == m_rows.size();
    for (qsizetype i = 0; sameShape && i < rows.size(); ++i)
        sameShape = rows[i].link == m_rows[i].link;
    if (!sameShape) {
        beginResetModel();
        m_rows = std::move(rows);
        endResetModel();
        emit countChanged();
        return;
    }
    for (qsizetype i = 0; i < rows.size(); ++i) {
        if (rows[i] == m_rows[i]) continue;
        m_rows[i] = rows[i];
        emit dataChanged(index(int(i)), index(int(i)));
    }
}

void HistoryModel::removeLink(const QString &link) {
    for (qsizetype i = 0; i < m_rows.size(); ++i) {
        if (m_rows[i].link != link) continue;
        beginRemoveRows({}, int(i), int(i));
        m_rows.removeAt(i);
        endRemoveRows();
        emit countChanged();
        return;
    }
}

void HistoryModel::clear() {
    if (m_rows.isEmpty()) return;
    beginResetModel();
    m_rows.clear();
    endResetModel();
    emit countChanged();
}

QString HistoryModel::linkAt(int row) const {
    return row >= 0 && row < m_rows.size() ? m_rows[row].link : QString();
}

int HistoryModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : int(m_rows.size());
}

QVariant HistoryModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= m_rows.size()) return {};
    const Row &row = m_rows[index.row()];
    switch (role) {
    case Link: return row.link;
    case Title: return row.title;
    case Cover: return row.cover;
    case Episode: return row.episode;
    case Total: return row.total;
    case Progress: return row.progress;
    case Provider: return row.provider;
    case EpisodeTitle: return row.episodeTitle;
    case PlayedAt: return row.playedAt;
    case SearchText: return row.title + QLatin1Char('\n') + row.episodeTitle;
    }
    return {};
}

QHash<int, QByteArray> HistoryModel::roleNames() const {
    return {{Link, "link"}, {Title, "title"}, {Cover, "cover"},
            {Episode, "episode"}, {Total, "total"}, {Progress, "progress"}, {Provider, "provider"},
            {EpisodeTitle, "episodeTitle"}, {PlayedAt, "playedAt"}};
}

namespace {

double progressFraction(double value) {
    return std::isfinite(value) ? std::clamp(value, 0.0, 1.0) : 0.0;
}

double progressPercentage(double value) { return progressFraction(value) * 100.0; }
double progressFromPercentage(double value) { return progressFraction(value / 100.0); }

// Shared with the trim below, so it cannot drop a row HistoryPage still shows.
constexpr int kHistoryRows = 100;

QString selectEntries(const char *whereClause) {
    return QLatin1String("SELECT link, title, cover, provider, library_type, last_watched_index, "
                         "total_episodes, show_type, progress FROM shows ")
           + QLatin1String(whereClause);
}

QSqlQuery prepared(const QSqlDatabase &db, const QString &sql, const QVariantList &binds = {}) {
    QSqlQuery query(db);
    query.prepare(sql);
    for (const QVariant &bind : binds) query.addBindValue(bind);
    return query;
}

bool runQuery(QSqlQuery &query, const char *what) {
    if (query.exec()) return true;
    logError() << "Library" << what << query.lastError().text();
    return false;
}

QVariant firstValue(const QSqlDatabase &db, const QString &sql, const QVariantList &binds = {}) {
    QSqlQuery query = prepared(db, sql, binds);
    return (query.exec() && query.next()) ? query.value(0) : QVariant();
}

}

Library::Library(QObject *parent)
    : QAbstractListModel(parent)
{
    m_historyView.setSourceModel(&m_history);
    for (auto signal : {&QAbstractItemModel::rowsInserted, &QAbstractItemModel::rowsRemoved})
        connect(this, signal, this, &Library::displayedCountChanged);
    connect(this, &QAbstractItemModel::modelReset, this, &Library::displayedCountChanged);
    initDatabase();
    refreshDisplayCache();
    m_watchedFraction = Settings::instance().watchedFraction();
    reloadHistory();
    connect(&Settings::instance(), &Settings::watchedPercentChanged, this, [this]() {
        m_watchedFraction = Settings::instance().watchedFraction();
        if (!m_displayCache.isEmpty())
            emit dataChanged(index(0), index(m_displayCache.size() - 1));
        reloadHistory();
    });
    connect(&m_fetchWatcher, &QFutureWatcher<void>::finished, this, [this]() {
        if (m_pendingFetchLibraryType >= -1) {
            int lt = m_pendingFetchLibraryType;
            bool forced = m_pendingFetchForced;
            m_pendingFetchLibraryType = kNoPendingFetch;
            m_pendingFetchForced = false;
            fetchUnwatchedEpisodes(lt, forced);
        }
    });
}

int Library::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : m_displayCache.size();
}

QVariant Library::data(const QModelIndex &index, int role) const {
    if (index.row() < 0 || index.row() >= m_displayCache.size()) return {};
    const LibraryEntry &e = m_displayCache[index.row()];
    switch (role) {
    case Role::Title: return e.title;
    case Role::Cover: return e.cover;
    case Role::UnwatchedEpisodes: {
        if (e.totalEpisodes <= 0) return -1;   // -1 = count unknown, distinct from 0 (caught up)
        int unwatched = e.totalEpisodes - e.lastWatchedIndex - 1;
        if (e.lastWatchedIndex >= 0 && e.progress < m_watchedFraction)
            unwatched += 1;
        return qMax(0, unwatched);
    }
    case Role::ShowType: return e.showType;
    case Role::Provider: return e.provider;
    case Role::Type: return e.libraryType;
    case Role::TotalEpisodes: return e.totalEpisodes;
    default: return {};
    }
}

QHash<int, QByteArray> Library::roleNames() const {
    return {
        {Role::Title, "title"}, {Role::Cover, "cover"},
        {Role::UnwatchedEpisodes, "unwatchedEpisodes"}, {Role::ShowType, "type"},
        {Role::Provider, "provider"}, {Role::Type, "libraryType"},
        {Role::TotalEpisodes, "totalEpisodes"},
    };
}

Library::~Library() {
    disconnect(&m_fetchWatcher, nullptr, this, nullptr);
    m_cancel.cancel();
    waitFor(m_fetchWatcher, "Library episode-count fetch");
    m_db.close();
    m_db = QSqlDatabase();
    QSqlDatabase::removeDatabase(kConnectionName);
}

void Library::initDatabase() {
    m_db = Store::open(kConnectionName);
    Store::prune(m_db);
    if (!m_db.isOpen()) {
        const QString err = m_db.lastError().text();
        logError() << "Library" << "Failed to open the database:" << err;
        QMetaObject::invokeMethod(qApp, [err]() {
            AppShell::instance().reportError(tr("Library database could not be opened:\n%1\nYour library and "
                                                "history will not be saved this session.").arg(err),
                                             tr("Library"));
        }, Qt::QueuedConnection);
    }
}

LibraryEntry Library::entryFromQuery(const QSqlQuery &query) {
    LibraryEntry entry;
    entry.link             = query.value(0).toString();
    entry.title            = query.value(1).toString();
    entry.cover            = query.value(2).toString();
    entry.provider         = query.value(3).toString();
    entry.libraryType      = query.value(4).toInt();
    entry.lastWatchedIndex = query.value(5).toInt();
    entry.totalEpisodes    = query.value(6).toInt();
    entry.showType         = query.value(7).toInt();
    entry.progress         = progressFromPercentage(query.value(8).toDouble());
    entry.valid            = true;
    return entry;
}

QVariantList Library::findShows(const QString &text, int limit) const {
    const QString needle = text.trimmed();
    if (needle.isEmpty()) return {};
    QString pattern = needle;
    pattern.replace(QLatin1Char('\\'), QLatin1String("\\\\")).replace(QLatin1Char('%'), QLatin1String("\\%"))
           .replace(QLatin1Char('_'), QLatin1String("\\_"));
    pattern = QLatin1Char('%') + pattern + QLatin1Char('%');
    // A compound select orders only by its columns, so the union is wrapped to order by an expression.
    QSqlQuery query = prepared(m_db, R"(
        SELECT * FROM (
            SELECT title, link, provider, library_type FROM shows WHERE title LIKE ? ESCAPE '\'
            UNION ALL
            SELECT title, link, provider, -1 FROM history
                WHERE title LIKE ? ESCAPE '\' AND link NOT IN (SELECT link FROM shows))
        ORDER BY instr(lower(title), lower(?)), title LIMIT ?)", {pattern, pattern, needle, limit});
    QVariantList found;
    if (!runQuery(query, "Failed to find shows:")) return found;
    while (query.next())
        found.append(QVariantMap{{"title", query.value(0)}, {"link", query.value(1)},
                                 {"provider", query.value(2)}, {"libraryType", query.value(3)}});
    return found;
}

QVariantList Library::notifications() const {
    QSqlQuery query = prepared(m_db, "SELECT id, kind, title, message, link, provider, at, seen "
                                     "FROM notifications ORDER BY id DESC");
    QVariantList list;
    if (!runQuery(query, "Failed to read notifications:")) return list;
    while (query.next())
        list.append(QVariantMap{{"id", query.value(0)}, {"kind", query.value(1)}, {"title", query.value(2)},
                                {"message", query.value(3)}, {"link", query.value(4)}, {"provider", query.value(5)},
                                {"at", query.value(6)}, {"seen", query.value(7).toBool()}});
    return list;
}

void Library::addNotification(const QString &kind, const QString &title, const QString &message,
                              const QString &link, const QString &provider) {
    QSqlQuery insert = prepared(m_db, "INSERT INTO notifications (kind, title, message, link, provider, at) "
                                      "VALUES (?, ?, ?, ?, ?, ?)",
                                {kind, title, message, link, provider, QDateTime::currentSecsSinceEpoch()});
    if (!runQuery(insert, "Failed to add a notification:")) return;
    QSqlQuery prune = prepared(m_db, "DELETE FROM notifications WHERE id NOT IN "
                                     "(SELECT id FROM notifications ORDER BY id DESC LIMIT 100)");
    runQuery(prune, "Failed to prune notifications:");
    emit notificationsChanged();
}

int Library::unseenNotifications() const {
    return firstValue(m_db, "SELECT COUNT(*) FROM notifications WHERE seen = 0").toInt();
}

void Library::markNotificationsSeen() {
    if (unseenNotifications() == 0) return;
    QSqlQuery update = prepared(m_db, "UPDATE notifications SET seen = 1 WHERE seen = 0");
    if (runQuery(update, "Failed to mark notifications seen:")) emit notificationsChanged();
}

void Library::clearNotifications() {
    QSqlQuery clear = prepared(m_db, "DELETE FROM notifications");
    if (runQuery(clear, "Failed to clear notifications:")) emit notificationsChanged();
}

void Library::setSearchAllLists(bool on) {
    if (m_allLists == on) return;
    m_allLists = on;
    beginResetModel();
    refreshDisplayCache();
    endResetModel();
    emit displayedCountChanged();
    emit searchAllListsChanged();
}

void Library::refreshDisplayCache() {
    m_displayCache.clear();
    QSqlQuery query = m_allLists ? prepared(m_db, selectEntries("ORDER BY library_type, sort_order"))
                                 : prepared(m_db, selectEntries("WHERE library_type = ? ORDER BY sort_order"),
                                            {m_displayLibraryType});
    if (!runQuery(query, "Failed to load display cache:")) return;
    while (query.next())
        m_displayCache.push_back(entryFromQuery(query));
}

LibraryEntry Library::entryForLink(const QString &link) const {
    QSqlQuery query = prepared(m_db, selectEntries("WHERE link = ?"), {link});
    if (query.exec() && query.next())
        return entryFromQuery(query);
    return {};
}

bool Library::migrate(const QString &oldLink, const QString &newLink, const QString &title,
                             const QString &cover, const QString &provider, int showType,
                             int lastWatchedIndex, int totalEpisodes) {
    if (oldLink.isEmpty() || newLink.isEmpty()) return false;
    if (newLink != oldLink && linkExists(newLink)) return false;

    if (!m_db.transaction()) return false;
    QSqlQuery shows = prepared(m_db, "UPDATE shows SET link=?, title=?, cover=?, provider=?, "
                                     "show_type=?, last_watched_index=?, total_episodes=? WHERE link=?",
                               {newLink, title, cover, provider, showType,
                                lastWatchedIndex, totalEpisodes, oldLink});
    if (!shows.exec() || shows.numRowsAffected() == 0) { m_db.rollback(); return false; }

    QSqlQuery dropStale = prepared(m_db, "DELETE FROM history WHERE link = ?", {newLink});
    if (!dropStale.exec()) { m_db.rollback(); return false; }

    QSqlQuery history = prepared(m_db, "UPDATE history SET link=?, title=?, cover=?, provider=?, "
                                       "last_watched_index=?, total_episodes=? WHERE link=?",
                                 {newLink, title, cover, provider,
                                  lastWatchedIndex, totalEpisodes, oldLink});
    if (!history.exec()) { m_db.rollback(); return false; }
    QSqlQuery notes = prepared(m_db, "UPDATE OR REPLACE notes SET link = ? WHERE link = ?", {newLink, oldLink});
    if (!notes.exec()) { m_db.rollback(); return false; }
    if (!m_db.commit()) { m_db.rollback(); return false; }

    m_historyMeta.remove(oldLink);
    beginResetModel();
    refreshDisplayCache();
    endResetModel();
    emit libraryChanged();
    reloadHistory();
    return true;
}

QVariantMap Library::trackerLink(const QString &showLink, const QString &service) const {
    QSqlQuery query = prepared(m_db, "SELECT remote_id, status, score, progress, updated_at "
                                     "FROM tracker_links WHERE show_link = ? AND service = ?",
                               {showLink, service});
    if (!query.exec() || !query.next()) return {{"valid", false}};
    return {{"valid", true}, {"remoteId", query.value(0).toString()},
            {"status", query.value(1).toString()}, {"score", query.value(2).toDouble()},
            {"progress", query.value(3).toInt()}, {"updatedAt", query.value(4).toLongLong()}};
}

void Library::setTrackerLink(const QString &showLink, const QString &service,
                             const QString &remoteId, const QString &status,
                             double score, int progress) {
    QSqlQuery query = prepared(m_db,
        "INSERT INTO tracker_links (show_link, service, remote_id, status, score, progress, updated_at) "
        "VALUES (?,?,?,?,?,?,?) ON CONFLICT(show_link, service) DO UPDATE SET "
        "remote_id=excluded.remote_id, status=excluded.status, score=excluded.score, "
        "progress=excluded.progress, updated_at=excluded.updated_at",
        {showLink, service, remoteId, status, score, progress, QDateTime::currentSecsSinceEpoch()});
    if (!query.exec()) logWarn() << "Library" << "tracker link write failed:" << query.lastError().text();
}

void Library::clearTrackerLink(const QString &showLink, const QString &service) {
    QSqlQuery query = prepared(m_db, "DELETE FROM tracker_links WHERE show_link = ? AND service = ?",
                               {showLink, service});
    query.exec();
}

Library::SyncState Library::syncState(const QString &episodeLink, const QString &service) const {
    QSqlQuery query = prepared(m_db, "SELECT base_pos, base_server_at, pushed_pos FROM sync_state "
                                     "WHERE episode_link = ? AND service = ?", {episodeLink, service});
    SyncState state;
    if (query.exec() && query.next()) {
        state.basePos      = query.value(0).toDouble();
        state.baseServerAt = query.value(1).toLongLong();
        state.pushedPos    = query.value(2).toDouble();
        state.valid = true;
    }
    return state;
}

void Library::setSyncState(const QString &episodeLink, const QString &service, const SyncState &state) {
    QSqlQuery query = prepared(m_db,
        "INSERT INTO sync_state (episode_link, service, base_pos, base_server_at, pushed_pos) "
        "VALUES (?,?,?,?,?) ON CONFLICT(episode_link, service) DO UPDATE SET "
        "base_pos=excluded.base_pos, base_server_at=excluded.base_server_at, "
        "pushed_pos=excluded.pushed_pos",
        {episodeLink, service, state.basePos, state.baseServerAt, state.pushedPos});
    if (!query.exec()) logWarn() << "Library" << "sync state write failed:" << query.lastError().text();
}

void Library::queueSyncOutbox(const QString &episodeLink, const QString &service,
                              double seconds, double duration) {
    QSqlQuery query = prepared(m_db,
        "INSERT INTO sync_outbox (episode_link, service, seconds, duration, queued_at) "
        "VALUES (?,?,?,?,?) ON CONFLICT(episode_link, service) DO UPDATE SET "
        "seconds=excluded.seconds, duration=excluded.duration, queued_at=excluded.queued_at",
        {episodeLink, service, seconds, duration, QDateTime::currentSecsSinceEpoch()});
    if (query.exec()) emit syncOutboxChanged();
}

QVariantMap Library::syncOutboxCounts() const {
    QVariantMap counts;
    QSqlQuery query(m_db);
    if (query.exec(QStringLiteral("SELECT service, COUNT(*) FROM sync_outbox GROUP BY service")))
        while (query.next()) counts.insert(query.value(0).toString(), query.value(1).toInt());
    return counts;
}

QList<Library::OutboxEntry> Library::syncOutbox(const QString &service) const {
    QList<OutboxEntry> out;
    QSqlQuery query = prepared(m_db, "SELECT episode_link, seconds, duration, queued_at FROM sync_outbox "
                                     "WHERE service = ? ORDER BY queued_at LIMIT 50", {service});
    if (!runQuery(query, "Failed to read the sync outbox:")) return out;
    while (query.next())
        out.append({query.value(0).toString(), query.value(1).toDouble(),
                    query.value(2).toDouble(), query.value(3).toLongLong()});
    return out;
}

void Library::removeFromSyncOutbox(const QString &service, const OutboxEntry &entry) {
    QSqlQuery query = prepared(m_db, "DELETE FROM sync_outbox WHERE episode_link = ? AND service = ? "
                                     "AND queued_at = ?", {entry.episodeLink, service, entry.queuedAt});
    if (runQuery(query, "Failed to clear a delivered push:")) emit syncOutboxChanged();
}

void Library::clearSyncOutbox(const QString &service) {
    QSqlQuery query = prepared(m_db, "DELETE FROM sync_outbox WHERE service = ?", {service});
    if (runQuery(query, "Failed to clear the sync outbox:")) emit syncOutboxChanged();
}

void Library::reloadHistory() {
    QSqlQuery query = prepared(m_db, QString("SELECT link, title, cover, last_watched_index, "
                                             "total_episodes, progress, provider, episode_title, last_played_at FROM history "
                                             "ORDER BY last_played_at DESC LIMIT %1").arg(kHistoryRows));
    if (!query.exec()) return;
    QList<HistoryModel::Row> rows;
    while (query.next()) {
        const int lwi   = query.value(3).toInt();
        const int total = query.value(4).toInt();
        const double raw = progressFromPercentage(query.value(5).toDouble());
        const double watched = raw >= m_watchedFraction ? 1.0 : raw;
        rows.append({query.value(0).toString(), query.value(1).toString(), query.value(2).toString(),
                     query.value(6).toString(), lwi >= 0 ? lwi + 1 : 0, total,
                     (total > 0 && lwi >= 0) ? qBound(0.0, (lwi + watched) / total, 1.0) : 0.0,
                     query.value(7).toString(), query.value(8).toLongLong()});
    }
    m_history.setRows(std::move(rows));
}

int Library::indexOf(const QString &link) const {
    if (link.isEmpty()) return -1;
    for (int i = 0; i < m_displayCache.size(); ++i)
        if (m_displayCache[i].link == link)
            return i;
    return -1;
}

bool Library::add(const ShowData& show, int libraryType) {
    QSqlQuery check = prepared(m_db, "SELECT library_type FROM shows WHERE link = ?", {show.link});
    if (!runQuery(check, "DB check error:")) return false;

    if (check.next()) {
        int oldLibraryType = check.value(0).toInt();
        if (oldLibraryType == libraryType) {
            return true;
        }
        changeLibraryType(show.link, libraryType);
    } else {
        auto playlist = show.playlist();
        auto lastWatchedIndex = playlist ? playlist->currentIndex() : -1;
        auto totalEpisodes = playlist ? playlist->episodeCount() : 0;
        auto currentItem = (lastWatchedIndex != -1 && playlist) ? playlist->currentItem() : nullptr;
        auto progress = currentItem ? currentItem->progress() : 0.0;

        bool affectsDisplay = m_allLists || libraryType == m_displayLibraryType;

        const QString providerName = show.provider ? show.provider->name() : QString();
        QSqlQuery insert = prepared(m_db, R"(
            INSERT INTO shows (link, title, provider, cover, library_type, last_watched_index, progress, total_episodes, show_type, sort_order)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, (
                SELECT IFNULL(MAX(sort_order), -1) + 1 FROM shows WHERE library_type = ?
            ))
        )", {show.link, show.title, providerName, show.coverUrl, libraryType,
             lastWatchedIndex, progressPercentage(progress), totalEpisodes, show.type, libraryType});
        if (!runQuery(insert, "Failed to insert show to DB:")) return false;
        if (affectsDisplay) {
            LibraryEntry e;
            e.link             = show.link;
            e.title            = show.title;
            e.cover            = show.coverUrl;
            e.provider         = providerName;
            e.libraryType      = libraryType;
            e.lastWatchedIndex = lastWatchedIndex;
            e.progress         = progress;
            e.totalEpisodes    = totalEpisodes;
            e.showType         = show.type;
            e.valid            = true;
            int insertIndex = m_displayCache.size();
            beginInsertRows(QModelIndex(), insertIndex, insertIndex);
            m_displayCache.push_back(e);
            endInsertRows();
        }
        if (totalEpisodes <= 0)
            fetchUnwatchedEpisodes(libraryType, true);
        emit libraryChanged();
    }

    return true;
}

bool Library::linkExists(const QString &link) const {
    return !link.isEmpty()
           && firstValue(m_db, "SELECT 1 FROM shows WHERE link = ? LIMIT 1", {link}).isValid();
}

QString Library::linkAtIndex(int index, int libraryType) const {
    // While every list shows, an index is into what is shown.
    if (m_allLists || libraryType == m_displayLibraryType)
        return (index >= 0 && index < m_displayCache.size()) ? m_displayCache[index].link : QString();
    return firstValue(m_db, "SELECT link FROM shows WHERE library_type = ? "
                            "ORDER BY sort_order, link LIMIT 1 OFFSET ?",
                      {libraryType, index}).toString();
}

int Library::count(int libraryType) const {
    if (libraryType == -1) libraryType = m_displayLibraryType;
    if (libraryType == m_displayLibraryType)
        return m_displayCache.size();
    return firstValue(m_db, "SELECT COUNT(*) FROM shows WHERE library_type = ?",
                      {libraryType}).toInt();
}

void Library::remove(const QString &link) {
    QSqlQuery query = prepared(m_db, "DELETE FROM shows WHERE link = ?", {link});
    if (!runQuery(query, "Failed to remove a show:")) return;

    // Trust the cache, not the row's type: they disagree after a type change.
    if (const int row = indexOf(link); row >= 0) {
        beginRemoveRows(QModelIndex(), row, row);
        m_displayCache.removeAt(row);
        endRemoveRows();
    }
    emit libraryChanged();
}

QVariantList Library::removeAt(int index, int libraryType) {
    if (libraryType == -1) libraryType = m_displayLibraryType;
    const QString link = linkAtIndex(index, libraryType);
    if (link.isEmpty()) return {};
    const QVariantList rows = rowsOf(QStringLiteral("shows"), link);
    remove(link);
    return rows;
}

QVariantList Library::rowsOf(const QString &table, const QString &link) const {
    QSqlQuery query = link.isEmpty() ? prepared(m_db, QStringLiteral("SELECT * FROM %1").arg(table))
                                     : prepared(m_db, QStringLiteral("SELECT * FROM %1 WHERE link = ?").arg(table), {link});
    QVariantList rows;
    if (!runQuery(query, "Failed to read rows for undo:")) return rows;
    while (query.next()) {
        const QSqlRecord record = query.record();
        QVariantMap row;
        for (int i = 0; i < record.count(); ++i) row.insert(record.fieldName(i), record.value(i));
        rows.append(row);
    }
    return rows;
}

void Library::restoreRows(const QString &table, const QVariantList &rows) {
    // Names come from the caller and go into the SQL: only the two tables a removal hands
    // back, and only plain column names.
    if ((table != QLatin1String("shows") && table != QLatin1String("history")) || rows.isEmpty()) return;
    static const QRegularExpression plainName(QStringLiteral("^[a-z_]+$"));
    m_db.transaction();
    for (const QVariant &value : rows) {
        const QVariantMap row = value.toMap();
        const QStringList columns = row.keys();
        if (columns.isEmpty() || !std::all_of(columns.cbegin(), columns.cend(),
                                              [](const QString &name) { return plainName.match(name).hasMatch(); })) {
            m_db.rollback();
            return;
        }
        QSqlQuery insert(m_db);
        insert.prepare(QStringLiteral("INSERT OR REPLACE INTO %1 (%2) VALUES (%3)")
                           .arg(table, columns.join(QLatin1Char(',')),
                                QStringList(columns.size(), QStringLiteral("?")).join(QLatin1Char(','))));
        for (const QString &column : columns) insert.addBindValue(row.value(column));
        if (!runQuery(insert, "Failed to restore a row:")) {
            m_db.rollback();
            return;
        }
    }
    m_db.commit();
    if (table == QLatin1String("history")) {
        reloadHistory();
        return;
    }
    beginResetModel();
    refreshDisplayCache();
    endResetModel();
    emit displayedCountChanged();
    emit libraryChanged();
}

void Library::move(int from, int to) {
    // Order is per list; across all of them it means nothing.
    if (m_allLists || from == to || from < 0 || to < 0) return;
    if (from >= m_displayCache.size() || to >= m_displayCache.size()) return;

    // Persist first, or a failed write leaves the cache reordered and the DB untouched.
    QList<LibraryEntry> reordered = m_displayCache;
    reordered.move(from, to);

    if (!m_db.transaction()) return;
    QSqlQuery update(m_db);
    update.prepare("UPDATE shows SET sort_order = ? WHERE link = ?");
    for (int i = 0; i < reordered.size(); ++i) {
        update.addBindValue(i);
        update.addBindValue(reordered[i].link);
        if (!update.exec()) {
            logError() << "Library" << "Failed to persist move:" << update.lastError().text();
            m_db.rollback();
            return;
        }
    }
    if (!m_db.commit()) {
        logError() << "Library" << "Failed to commit move";
        m_db.rollback();
        return;
    }

    beginMoveRows(QModelIndex(), from, from, QModelIndex(), to + (to > from ? 1 : 0));
    m_displayCache.move(from, to);
    endMoveRows();
}

void Library::updateProgress(const QString &link, int lastWatchedIndex, double progress) {
    if (link.isEmpty() || lastWatchedIndex < 0 || !std::isfinite(progress)) return;
    progress = progressFraction(progress);
    const QVariantList binds{lastWatchedIndex, progressPercentage(progress), link};
    QSqlQuery episode = prepared(m_db,
        "INSERT INTO episode_progress(show_link,episode_index,progress) VALUES(?,?,?) "
        "ON CONFLICT(show_link,episode_index) DO UPDATE SET progress=excluded.progress",
        {link, lastWatchedIndex, progressPercentage(progress)});
    if (!runQuery(episode, "Failed to save episode progress:")) return;
    QSqlQuery query = prepared(m_db, "UPDATE shows SET last_watched_index = ?, progress = ? "
                                     "WHERE link = ?", binds);
    query.exec();

    QSqlQuery hq = prepared(m_db, "UPDATE history SET last_watched_index = ?, progress = ? "
                                  "WHERE link = ?", binds);
    if (hq.exec() && hq.numRowsAffected() > 0) reloadHistory();

    int idx = indexOf(link);
    if (idx >= 0) {
        auto &e = m_displayCache[idx];
        e.lastWatchedIndex = lastWatchedIndex;
        e.progress = progress;
        emit dataChanged(index(idx), index(idx));
    }
}

QList<QPair<QString, double>> Library::localFolderProgress(const QString &folder) const {
    QList<QPair<QString, double>> rows;
    if (folder.isEmpty()) return rows;
    QSqlQuery query = prepared(m_db, "SELECT path, progress FROM local_progress WHERE folder = ? "
                                     "ORDER BY last_played_at DESC, rowid DESC", {folder});
    if (!runQuery(query, "Failed to read local progress:")) return rows;
    while (query.next())
        rows.append({query.value(0).toString(), progressFromPercentage(query.value(1).toDouble())});
    return rows;
}

QVariantList Library::watchedFilesUnder(const QString &folder) const {
    const QString root = QDir::cleanPath(folder) + QLatin1Char('/');
    QSqlQuery query = prepared(m_db, "SELECT path FROM local_progress WHERE progress >= ?",
                               {progressPercentage(Settings::instance().watchedFraction())});
    QVariantList files;
    if (!query.exec()) return files;
    while (query.next()) {
        const QString path = query.value(0).toString();
        const QFileInfo info(path);
        if (QDir::cleanPath(path).startsWith(root, Qt::CaseInsensitive) && info.isFile())
            files.append(QVariantMap{{"path", path}, {"size", info.size()}});
    }
    return files;
}

QVariantMap Library::watchStats() const {
    const qint64 weekAgo = QDateTime::currentSecsSinceEpoch() - 7 * 24 * 60 * 60;
    const double mark = progressPercentage(Settings::instance().watchedFraction());
    int thisWeek = 0;
    QSet<QDate> days;
    QSqlQuery query(m_db);
    if (query.exec(QStringLiteral("SELECT progress, last_played_at FROM episode_progress WHERE last_played_at > 0 "
                                  "UNION ALL SELECT progress, last_played_at FROM local_progress WHERE last_played_at > 0")))
        while (query.next()) {
            const qint64 at = query.value(1).toLongLong();
            if (at >= weekAgo && query.value(0).toDouble() >= mark) ++thisWeek;
            days.insert(QDateTime::fromSecsSinceEpoch(at).date());
        }
    const QDate today = QDate::currentDate();
    int activeDays = 0;
    for (const QDate &day : std::as_const(days))
        if (day.daysTo(today) < 30) ++activeDays;
    // A streak lasts until a whole day passes without watching; today still has time.
    int streak = 0;
    for (QDate day = days.contains(today) ? today : today.addDays(-1); days.contains(day); day = day.addDays(-1))
        ++streak;
    return {{"episodesThisWeek", thisWeek}, {"activeDays", activeDays}, {"streak", streak},
            {"watching", count(Watching)}, {"completed", count(Completed)}};
}

QVariantMap Library::note(const QString &link) const {
    QSqlQuery query = prepared(m_db, "SELECT rating, note FROM notes WHERE link = ?", {link});
    if (query.exec() && query.next())
        return {{"rating", query.value(0).toInt()}, {"text", query.value(1).toString()}};
    return {{"rating", 0}, {"text", QString()}};
}

void Library::setNote(const QString &link, int rating, const QString &text) {
    if (link.isEmpty()) return;
    rating = qBound(0, rating, 5);
    QSqlQuery query = rating == 0 && text.trimmed().isEmpty()
        ? prepared(m_db, "DELETE FROM notes WHERE link = ?", {link})
        : prepared(m_db, "INSERT INTO notes (link, rating, note) VALUES (?, ?, ?) "
                         "ON CONFLICT(link) DO UPDATE SET rating = excluded.rating, note = excluded.note",
                   {link, rating, text});
    runQuery(query, "Failed to save the note:");
}

int Library::trashFiles(const QStringList &paths) {
    QStringList moved;
    for (const QString &path : paths)
        if (QFile::moveToTrash(path)) moved << path;
    forgetLocalProgress(moved);
    return int(moved.size());
}

void Library::updateLocalProgress(const QString &path, const QString &folder, double progress) {
    if (path.isEmpty() || folder.isEmpty()) return;
    QSqlQuery query = prepared(m_db,
        "INSERT INTO local_progress (path, folder, progress, last_played_at) "
        "VALUES (?, ?, ?, strftime('%s','now')) "
        "ON CONFLICT(path) DO UPDATE SET folder = excluded.folder, progress = excluded.progress, "
        "                                last_played_at = excluded.last_played_at",
        {path, folder, progressPercentage(progress)});
    runQuery(query, "Failed to save local progress:");
}

void Library::forgetLocalProgress(const QStringList &paths) {
    if (paths.isEmpty()) return;
    QSqlQuery query(m_db);
    query.prepare("DELETE FROM local_progress WHERE path = ?");
    query.addBindValue(QVariantList(paths.cbegin(), paths.cend()));
    if (!query.execBatch()) {
        logError() << "Library" << "Failed to forget local progress:" << query.lastError().text();
        return;
    }
    logInfo() << "Library" << "Forgot" << paths.size() << "deleted files";
}

void Library::cacheHistoryMeta(const QString &link, const QString &title,
                                      const QString &cover, const QString &provider, int total) {
    if (link.isEmpty()) return;
    m_historyMeta[link] = { title, cover, provider, total };
}

void Library::recordHistory(const QString &link, int lastWatchedIndex, const QString &episodeTitle) {
    if (!m_historyMeta.contains(link)) return;
    const HistoryMeta meta = m_historyMeta.value(link);
    QSqlQuery q = prepared(m_db,
        "INSERT INTO history "
        "(link, title, cover, provider, last_watched_index, total_episodes, episode_title, last_played_at) "
        "VALUES (?, ?, ?, ?, ?, ?, IFNULL(?, ''), strftime('%s','now')) "
        "ON CONFLICT(link) DO UPDATE SET "
        "  title = CASE WHEN excluded.title != '' THEN excluded.title ELSE title END,"
        "  cover = CASE WHEN excluded.cover != '' THEN excluded.cover ELSE cover END,"
        "  provider = CASE WHEN excluded.provider != '' THEN excluded.provider ELSE provider END,"
        "  total_episodes = CASE WHEN excluded.total_episodes > 0 THEN excluded.total_episodes ELSE total_episodes END,"
        "  last_watched_index = excluded.last_watched_index,"
        "  episode_title = excluded.episode_title,"
        "  last_played_at = excluded.last_played_at",
        {link, meta.title, meta.cover, meta.provider, lastWatchedIndex, meta.total, episodeTitle});
    if (q.exec()) {
        auto stamp = prepared(m_db, "INSERT INTO episode_progress(show_link,episode_index,progress,last_played_at) "
            "VALUES(?,?,0,strftime('%s','now')) ON CONFLICT(show_link,episode_index) "
            "DO UPDATE SET last_played_at=excluded.last_played_at", {link,lastWatchedIndex});
        runQuery(stamp, "Failed to record episode history:");
        reloadHistory();
    }
}

QVariantList Library::clearHistory() {
    const QVariantList rows = rowsOf(QStringLiteral("history"));
    QSqlQuery q(m_db);
    if (!q.exec("DELETE FROM history")) return {};
    m_historyMeta.clear();
    m_history.clear();
    return rows;
}

QVariantList Library::removeFromHistory(const QString &link) {
    if (link.isEmpty()) return {};
    const QVariantList rows = rowsOf(QStringLiteral("history"), link);
    QSqlQuery q = prepared(m_db, "DELETE FROM history WHERE link = ?", {link});
    if (!q.exec()) return {};
    m_historyMeta.remove(link);
    m_history.removeLink(link);
    return rows;
}

LibraryEntry Library::historyEntry(const QString &link) const {
    LibraryEntry e;
    QSqlQuery q = prepared(m_db, "SELECT title, cover, provider, last_watched_index, "
                                 "total_episodes, progress FROM history WHERE link = ?", {link});
    if (q.exec() && q.next()) {
        e.link = link;
        e.title = q.value(0).toString();
        e.cover = q.value(1).toString();
        e.provider = q.value(2).toString();
        e.lastWatchedIndex = q.value(3).toInt();
        e.totalEpisodes = q.value(4).toInt();
        e.progress = progressFromPercentage(q.value(5).toDouble());
        e.valid = true;
    }
    return e;
}

void Library::updateShowCover(const QString &link, const QString &cover) {
    QSqlQuery query = prepared(m_db, "UPDATE shows SET cover = ? WHERE link = ?", {cover, link});
    if (!runQuery(query, "Failed to update show cover:")) return;
    int idx = indexOf(link);
    if (idx >= 0) {
        m_displayCache[idx].cover = cover;
        emit dataChanged(index(idx), index(idx));
    }
}

LibraryEntry Library::entryAt(int index) const {
    return (index >= 0 && index < m_displayCache.size()) ? m_displayCache[index] : LibraryEntry{};
}

void Library::changeLibraryTypeAt(int index, int newLibraryType, int oldLibraryType) {
    if (oldLibraryType == -1) oldLibraryType = m_displayLibraryType;
    QString link = linkAtIndex(index, oldLibraryType);
    changeLibraryType(link, newLibraryType);
}

void Library::changeLibraryType(const QString &link, int libraryType) {
    const int oldLibraryType = libraryTypeOf(link);
    if (oldLibraryType < 0 || oldLibraryType == libraryType) return;

    const int oldIndex = indexOf(link);
    QSqlQuery update = prepared(m_db, "UPDATE shows SET library_type = ?, sort_order = "
                                      "(SELECT IFNULL(MAX(sort_order), -1) + 1 FROM shows WHERE library_type = ?) "
                                      "WHERE link = ?", {libraryType, libraryType, link});
    if (!runQuery(update, "Failed to update libraryType:")) return;

    // Showing every list, the row stays where it is and only its list changes.
    if (m_allLists && oldIndex >= 0) {
        m_displayCache[oldIndex].libraryType = libraryType;
        emit dataChanged(index(oldIndex), index(oldIndex), {Role::Type});
        emit libraryChanged();
        return;
    }
    if (oldIndex >= 0) {
        beginRemoveRows(QModelIndex(), oldIndex, oldIndex);
        m_displayCache.removeAt(oldIndex);
        endRemoveRows();
    }
    if (libraryType == m_displayLibraryType) {
        LibraryEntry e = entryForLink(link);
        if (e.valid) {
            int insertIndex = m_displayCache.size();
            beginInsertRows(QModelIndex(), insertIndex, insertIndex);
            m_displayCache.push_back(e);
            endInsertRows();
            if (e.totalEpisodes <= 0)
                fetchUnwatchedEpisodes(libraryType, true);
        }
    }
    emit libraryChanged();
}

int Library::libraryTypeOf(const QString &link) const {
    const QVariant type = firstValue(m_db, "SELECT library_type FROM shows WHERE link = ?", {link});
    return type.isValid() ? type.toInt() : -1;
}

// Its own pool: a refresh of a large library would otherwise hold the global pool's threads
// that searches and show loads are queued on.
static QThreadPool &countPool() {
    static QThreadPool pool;
    static const bool configured = [] { pool.setMaxThreadCount(6); return true; }();
    Q_UNUSED(configured);
    return pool;
}

void Library::fetchUnwatchedEpisodes(int libraryType, bool force) {
    if (libraryType < -1 || libraryType > LibraryType::Completed) return;

    if (!force) {
        const qint64 last = m_lastFetchMs.value(libraryType, 0);
        if (last != 0 && QDateTime::currentMSecsSinceEpoch() - last < kFetchDebounceMs)
            return;
    }

    if (m_fetchWatcher.isRunning()) {
        m_cancel.cancel();
        m_pendingFetchLibraryType = libraryType;
        m_pendingFetchForced = force;
        return;
    }
    m_pendingFetchLibraryType = kNoPendingFetch;
    m_cancel = CancelToken{};

    QSqlQuery query = prepared(m_db, "SELECT link, provider, library_type FROM shows WHERE (? = -1 OR library_type = ?)",
                               {libraryType, libraryType});
    if (!query.exec()) return;
    QList<QPair<QString, ShowProvider*>> shows;
    QSet<QString> withAiring;   // only the Watching list is worth the extra request
    while (query.next()) {
        QString providerName = query.value(1).toString();
        ShowProvider *provider = ProviderList::byName(providerName);
        if (!provider) continue;
        shows.emplaceBack(query.value(0).toString(), provider);
        if (query.value(2).toInt() == Watching && provider->publishesAiringTimes())
            withAiring.insert(query.value(0).toString());
    }

    m_fetchWatcher.setFuture(QtConcurrent::run([this, shows, withAiring, libraryType, cancel = m_cancel] {
        // Two lanes per provider rather than batches across all of them: a dead site stalls only
        // its own shows, and no site sees a burst it could read as scraping (AllAnime answers
        // one with a captcha).
        constexpr int kLanesPerProvider = 2;
        QHash<ShowProvider *, QStringList> byProvider;
        for (const auto &[link, provider] : shows) byProvider[provider].append(link);

        QMutex resultsMutex;
        QList<QPair<QString, int>> results;
        results.reserve(shows.size());
        QList<QPair<QString, qint64>> airing;
        QList<QFuture<void>> lanes;
        for (auto it = byProvider.cbegin(); it != byProvider.cend(); ++it) {
            for (int lane = 0; lane < kLanesPerProvider; ++lane) {
                lanes.push_back(QtConcurrent::run(&countPool(),
                    [provider = it.key(), links = it.value(), lane, cancel, &withAiring, &resultsMutex, &results, &airing]() {
                    Client client(cancel, false);
                    for (int i = lane; i < links.size() && !cancel.isCancelled(); i += kLanesPerProvider) {
                        auto dummyShow = ShowData("", links[i]);
                        const bool scheduled = withAiring.contains(links[i]);
                        int totalEpisodes = 0;
                        try {
                            totalEpisodes = scheduled ? provider->fetchEpisodeCountAndAiring(&client, dummyShow)
                                                      : provider->fetchEpisodeCount(&client, dummyShow);
                        } catch (AppException &e) {
                            e.log();
                        } catch (const std::exception &e) {
                            logWarn() << "Library" << links[i] << e.what();
                        } catch (...) {
                            logWarn() << "Library" << links[i] << "unknown error";
                        }
                        QMutexLocker lock(&resultsMutex);
                        results.append({links[i], totalEpisodes});
                        // A failed load keeps the old time rather than clearing it.
                        if (scheduled && totalEpisodes > 0)
                            airing.append({links[i], dummyShow.nextEpisodeAt.isValid()
                                                         ? dummyShow.nextEpisodeAt.toSecsSinceEpoch() : 0});
                    }
                }));
            }
        }
        for (auto &lane : lanes) lane.waitForFinished();

        QMetaObject::invokeMethod(this, [this, results, airing, libraryType, partial = cancel.isCancelled()]() {
            // Stamping a cancelled run debounces away the refetch that replaced it.
            if (!partial) m_lastFetchMs[libraryType] = QDateTime::currentMSecsSinceEpoch();
            applyAiringTimes(airing);
            applyEpisodeCounts(results);
            emit fetchedAllEpCounts();
        }, Qt::QueuedConnection);
    }));
}

void Library::applyEpisodeCounts(const QList<QPair<QString, int>> &counts) {
    QList<QPair<QString, int>> valid;
    valid.reserve(counts.size());
    for (const auto &p : counts)
        if (p.second > 0) valid.append(p);
    if (valid.isEmpty()) return;

    // Read before the new totals land. A show's first count is not news.
    struct Watched { QString title, provider; int total; };
    QHash<QString, Watched> watching;
    QSqlQuery query = prepared(m_db, "SELECT link, title, provider, total_episodes FROM shows WHERE library_type = ?",
                               {int(LibraryType::Watching)});
    if (query.exec())
        while (query.next())
            watching.insert(query.value(0).toString(),
                            {query.value(1).toString(), query.value(2).toString(), query.value(3).toInt()});
    QVariantList grown;
    for (const auto &[link, total] : std::as_const(valid)) {
        const auto it = watching.constFind(link);
        if (it != watching.constEnd() && it->total > 0 && total > it->total)
            grown.append(QVariantMap{{"title", it->title}, {"link", link}, {"provider", it->provider},
                                     {"total", total}});
    }

    persistEpisodeCounts(valid);
    for (const auto &[link, total] : std::as_const(valid)) {
        const int idx = indexOf(link);
        if (idx >= 0 && m_displayCache[idx].totalEpisodes != total) {
            m_displayCache[idx].totalEpisodes = total;
            emit dataChanged(index(idx), index(idx));
        }
    }
    if (!grown.isEmpty()) emit newEpisodesFound(grown);
}

void Library::applyAiringTimes(const QList<QPair<QString, qint64>> &times) {
    for (const auto &[link, at] : times) {
        QSqlQuery query = at > 0 ? prepared(m_db, "INSERT OR REPLACE INTO airing (link, next_at) VALUES (?, ?)", {link, at})
                                 : prepared(m_db, "DELETE FROM airing WHERE link = ?", {link});
        if (!query.exec()) logError() << "Library" << "Could not store the airing time of" << link << query.lastError().text();
    }
}

QVariantList Library::airingSchedule() const {
    // An hour's grace keeps an episode that just aired listed until the next check moves it on.
    QSqlQuery query = prepared(m_db,
        "SELECT s.link, s.title, s.provider, s.total_episodes, a.next_at FROM airing a JOIN shows s ON s.link = a.link "
        "WHERE s.library_type = ? AND a.next_at > ? ORDER BY a.next_at",
        {int(Watching), QDateTime::currentSecsSinceEpoch() - 3600});
    QVariantList shows;
    if (!query.exec()) return shows;
    while (query.next())
        shows.append(QVariantMap{{"link", query.value(0)}, {"title", query.value(1)}, {"provider", query.value(2)},
                                 {"episode", query.value(3).toInt() + 1},
                                 {"at", QDateTime::fromSecsSinceEpoch(query.value(4).toLongLong())}});
    return shows;
}

void Library::persistEpisodeCounts(const QList<QPair<QString, int>> &counts) {
    if (!m_db.transaction()) {
        logError() << "Library" << "Could not open a transaction for episode counts:" << m_db.lastError().text();
        return;
    }
    QSqlQuery update(m_db);
    update.prepare(QStringLiteral("UPDATE shows SET total_episodes = ? WHERE link = ?"));
    for (const auto &[link, total] : counts) {
        update.bindValue(0, total);
        update.bindValue(1, link);
        if (!update.exec())
            logError() << "Library" << "Could not set total_episodes for" << link << ":" << update.lastError().text();
    }

    if (!m_db.commit()) {
        logError() << "Library" << "Could not commit episode counts:" << m_db.lastError().text();
        m_db.rollback();
    }
}

void Library::setDisplayLibraryType(int newLibraryType) {
    newLibraryType = qBound<int>(Watching, newLibraryType, Completed);
    if (m_displayLibraryType != newLibraryType) {
        beginResetModel();
        m_displayLibraryType = newLibraryType;
        refreshDisplayCache();
        endResetModel();
        emit libraryTypeChanged();
        fetchUnwatchedEpisodes(newLibraryType);
    }
}

void Library::restoreEpisodeProgress(const QSharedPointer<PlaylistItem> &playlist) const {
    if (!playlist) return;
    auto query = prepared(m_db, "SELECT episode_index,progress FROM episode_progress WHERE show_link=?", {playlist->link});
    if (!query.exec()) return;
    while (query.next())
        if (auto item = playlist->at(query.value(0).toInt()))
            item->setProgress(progressFromPercentage(query.value(1).toDouble()));
}
