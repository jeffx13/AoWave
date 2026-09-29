#include "library/store.h"
#include "core/settings.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QSqlError>
#include <QSqlQuery>

#include "core/logger.h"

namespace {

constexpr auto kDbFileName = "store.db";

QString storePath()  { return Settings::dataDir() + QLatin1Char('/') + QLatin1String(kDbFileName); }

bool run(QSqlQuery &query, const char *sql) {
    if (query.exec(QLatin1String(sql))) return true;
    logError() << "Store" << query.lastError().text() << "in" << QLatin1String(sql);
    return false;
}

// Kept apart: history reaches rows never in the library, and the library keeps unplayed shows.
bool createSchema(QSqlDatabase &db) {
    QSqlQuery query(db);
    return run(query, R"(
        CREATE TABLE IF NOT EXISTS shows (
            link TEXT PRIMARY KEY, title TEXT, cover TEXT, provider TEXT,
            library_type INTEGER, last_watched_index INTEGER, total_episodes INTEGER,
            sort_order INTEGER, show_type INTEGER DEFAULT 0, progress REAL DEFAULT 0))")
        && run(query, R"(
        CREATE TABLE IF NOT EXISTS history (
            link TEXT PRIMARY KEY, title TEXT, cover TEXT, provider TEXT,
            last_watched_index INTEGER, total_episodes INTEGER,
            last_played_at INTEGER DEFAULT 0, progress REAL DEFAULT 0))")
        // Reopening a local file through shows/history would route it through a missing provider.
        && run(query, R"(
        CREATE TABLE IF NOT EXISTS local_progress (
            path TEXT PRIMARY KEY, folder TEXT NOT NULL,
            progress REAL DEFAULT 0, last_played_at INTEGER DEFAULT 0))")
        && run(query, R"(
        CREATE TABLE IF NOT EXISTS episode_progress (
            show_link TEXT NOT NULL, episode_index INTEGER NOT NULL,
            progress REAL NOT NULL DEFAULT 0, last_played_at INTEGER NOT NULL DEFAULT 0,
            PRIMARY KEY (show_link, episode_index)))")
        // base_* is the position both sides last agreed on.
        && run(query, R"(
        CREATE TABLE IF NOT EXISTS sync_state (
            episode_link TEXT NOT NULL, service TEXT NOT NULL, base_pos REAL DEFAULT 0,
            base_server_at INTEGER DEFAULT 0, pushed_pos REAL DEFAULT 0,
            PRIMARY KEY (episode_link, service)))")
        // A failed push waits here and is replayed.
        && run(query, R"(
        CREATE TABLE IF NOT EXISTS sync_outbox (
            episode_link TEXT NOT NULL, service TEXT NOT NULL, seconds REAL, duration REAL,
            queued_at INTEGER, PRIMARY KEY (episode_link, service)))")
        && run(query, R"(
        CREATE TABLE IF NOT EXISTS tracker_links (
            show_link TEXT NOT NULL, service TEXT NOT NULL, remote_id TEXT NOT NULL,
            status TEXT, score REAL DEFAULT 0, progress INTEGER DEFAULT 0,
            updated_at INTEGER DEFAULT 0, PRIMARY KEY (show_link, service)))")
        && run(query, R"(
        CREATE TABLE IF NOT EXISTS downloads (
            base_path TEXT PRIMARY KEY,
            video_name TEXT NOT NULL, folder TEXT NOT NULL,
            link TEXT, display_name TEXT,
            provider TEXT, episode_link TEXT, show_name TEXT,
            episode_number REAL DEFAULT -1, episode_season INTEGER DEFAULT 0,
            queued_at INTEGER DEFAULT 0))")
        // When the next episode airs, for shows whose provider publishes it.
        && run(query, "CREATE TABLE IF NOT EXISTS airing (link TEXT PRIMARY KEY, next_at INTEGER NOT NULL)")
        // The notification center's past alerts; `seen` once the center has been opened since.
        && run(query, "CREATE TABLE IF NOT EXISTS notifications (id INTEGER PRIMARY KEY AUTOINCREMENT, "
                      "kind TEXT NOT NULL, title TEXT, message TEXT, link TEXT, provider TEXT, "
                      "at INTEGER NOT NULL, seen INTEGER NOT NULL DEFAULT 0)")
        && run(query, "CREATE TABLE IF NOT EXISTS finished_downloads (path TEXT PRIMARY KEY, title TEXT, "
                      "finished_at INTEGER NOT NULL)")
        // The user's own words: never pruned.
        && run(query, "CREATE TABLE IF NOT EXISTS notes (link TEXT PRIMARY KEY, rating INTEGER NOT NULL DEFAULT 0, "
                      "note TEXT NOT NULL DEFAULT '')")
        && run(query, "CREATE INDEX IF NOT EXISTS idx_shows_library ON shows(library_type, sort_order)")
        && run(query, "CREATE INDEX IF NOT EXISTS idx_local_progress_folder "
                      "ON local_progress(folder, last_played_at)")
        // Without these the prune is three table scans.
        && run(query, "CREATE INDEX IF NOT EXISTS idx_episode_progress_show ON episode_progress(show_link)")
        && run(query, "CREATE INDEX IF NOT EXISTS idx_history_played ON history(last_played_at)")
        && run(query, "CREATE INDEX IF NOT EXISTS idx_sync_outbox_queued ON sync_outbox(queued_at)");
}

}

QSqlDatabase Store::open(const QString &connectionName) {
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    db.setDatabaseName(storePath());
    if (!db.open()) return db;

    QSqlQuery pragma(db);
    pragma.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    pragma.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));

    if (!createSchema(db)) {
        logError() << "Store" << "the schema could not be created";
        db.close();
        return db;
    }
    // Columns added since their table first shipped. SQLite has no ADD COLUMN IF NOT EXISTS, and
    // the statement fails harmlessly once the column is there.
    QSqlQuery(db).exec(QStringLiteral("ALTER TABLE downloads ADD COLUMN max_height INTEGER NOT NULL DEFAULT 0"));
    QSqlQuery(db).exec(QStringLiteral("ALTER TABLE history ADD COLUMN episode_title TEXT NOT NULL DEFAULT ''"));
    return db;
}

void Store::prune(QSqlDatabase &db) {
    if (!db.isOpen()) return;
    QSqlQuery query(db);

    // rowid, not the key: one NULL key in the subquery makes NOT IN match nothing.
    struct Cap { const char *table; int rows; };
    for (const Cap cap : {Cap{"history", 100}, Cap{"local_progress", 2000}}) {
        if (!query.exec(QStringLiteral("DELETE FROM %1 WHERE rowid NOT IN (SELECT rowid FROM %1 "
                                       "ORDER BY last_played_at DESC LIMIT %2)")
                            .arg(QLatin1String(cap.table)).arg(cap.rows)))
            continue;
        if (const int removed = query.numRowsAffected(); removed > 0)
            logInfo() << "Store" << "dropped" << removed << "stale" << cap.table << "row(s)";
    }

    const qint64 monthAgo = QDateTime::currentSecsSinceEpoch() - 30 * 24 * 60 * 60;

    query.prepare(QStringLiteral(
        "DELETE FROM episode_progress WHERE last_played_at < ? AND show_link NOT IN "
        "(SELECT link FROM shows UNION SELECT link FROM history)"));
    query.addBindValue(monthAgo);
    if (query.exec() && query.numRowsAffected() > 0)
        logInfo() << "Store" << "dropped" << query.numRowsAffected() << "orphaned episode row(s)";

    // Replaying a month-old push would move a watch list backwards.
    query.prepare(QStringLiteral("DELETE FROM sync_outbox WHERE queued_at < ?"));
    query.addBindValue(monthAgo);
    if (query.exec() && query.numRowsAffected() > 0)
        logInfo() << "Store" << "dropped" << query.numRowsAffected() << "expired outbox row(s)";

    query.exec(QStringLiteral("DELETE FROM airing WHERE link NOT IN (SELECT link FROM shows)"));
    query.exec(QStringLiteral(
        "DELETE FROM tracker_links WHERE show_link NOT IN "
        "(SELECT link FROM shows UNION SELECT link FROM history)"));

    // sync_state is keyed on the episode link, so the obvious join deletes everything.

    const auto pages = [&](const char *name) {
        QSqlQuery count(db);
        return count.exec(QStringLiteral("PRAGMA %1").arg(QLatin1String(name))) && count.next()
                   ? count.value(0).toInt() : 0;
    };
    const int freePages = pages("freelist_count");
    if (freePages > 256 && freePages * 2 > pages("page_count")) {
        QSqlQuery vacuum(db);
        if (vacuum.exec(QStringLiteral("VACUUM"))) logInfo() << "Store" << "compacted the database";
    }
}
