#include "library/store.h"
#include "app/settings.h"

#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>

#include "app/logger.h"

namespace {

constexpr auto kDbFileName = "store.db";

QString storePath()  { return Settings::dataDir() + QLatin1Char('/') + QLatin1String(kDbFileName); }

bool run(QSqlQuery &query, const char *sql) {
    if (query.exec(QLatin1String(sql))) return true;
    logError() << "Library" << query.lastError().text() << "in" << QLatin1String(sql);
    return false;
}

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
            last_played_at INTEGER DEFAULT 0, progress REAL DEFAULT 0,
            episode_title TEXT NOT NULL DEFAULT ''))")
        && run(query, R"(
        CREATE TABLE IF NOT EXISTS local_progress (
            path TEXT PRIMARY KEY, folder TEXT NOT NULL,
            progress REAL DEFAULT 0, last_played_at INTEGER DEFAULT 0))")
        && run(query, R"(
        CREATE TABLE IF NOT EXISTS episode_progress (
            show_link TEXT NOT NULL, episode_index INTEGER NOT NULL,
            progress REAL NOT NULL DEFAULT 0, last_played_at INTEGER NOT NULL DEFAULT 0,
            PRIMARY KEY (show_link, episode_index)))")
        && run(query, R"(
        CREATE TABLE IF NOT EXISTS sync_state (
            episode_link TEXT NOT NULL, service TEXT NOT NULL, base_pos REAL DEFAULT 0,
            base_server_at INTEGER DEFAULT 0, pushed_pos REAL DEFAULT 0,
            PRIMARY KEY (episode_link, service)))")
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
            queued_at INTEGER DEFAULT 0, max_height INTEGER NOT NULL DEFAULT 0,
            show_link TEXT, cover TEXT, episode_index INTEGER NOT NULL DEFAULT -1))")
        && run(query, "CREATE TABLE IF NOT EXISTS airing (link TEXT PRIMARY KEY, next_at INTEGER NOT NULL)")
        && run(query, "CREATE TABLE IF NOT EXISTS notifications (id INTEGER PRIMARY KEY AUTOINCREMENT, "
                      "kind TEXT NOT NULL, title TEXT, message TEXT, link TEXT, provider TEXT, "
                      "at INTEGER NOT NULL, seen INTEGER NOT NULL DEFAULT 0)")
        && run(query, "CREATE TABLE IF NOT EXISTS finished_downloads (path TEXT PRIMARY KEY, title TEXT, "
                      "finished_at INTEGER NOT NULL, show_link TEXT, provider TEXT, show_title TEXT, cover TEXT, "
                      "episode_link TEXT, episode_index INTEGER NOT NULL DEFAULT -1, "
                      "episode_number REAL NOT NULL DEFAULT -1, season INTEGER NOT NULL DEFAULT 0)")
        && run(query, "CREATE TABLE IF NOT EXISTS notes (link TEXT PRIMARY KEY, rating INTEGER NOT NULL DEFAULT 0, "
                      "note TEXT NOT NULL DEFAULT '')")
        && run(query, "CREATE INDEX IF NOT EXISTS idx_shows_library ON shows(library_type, sort_order)")
        && run(query, "CREATE INDEX IF NOT EXISTS idx_local_progress_folder "
                      "ON local_progress(folder, last_played_at)")
        && run(query, "CREATE INDEX IF NOT EXISTS idx_episode_progress_show ON episode_progress(show_link)")
        && run(query, "CREATE INDEX IF NOT EXISTS idx_history_played ON history(last_played_at)")
        && run(query, "CREATE INDEX IF NOT EXISTS idx_sync_outbox_queued ON sync_outbox(queued_at)")
        && run(query, "CREATE INDEX IF NOT EXISTS idx_finished_show ON finished_downloads(show_link)");
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
        logError() << "Library" << "the schema could not be created";
        db.close();
        return db;
    }
    return db;
}

void Store::prune(QSqlDatabase &db) {
    if (!db.isOpen()) return;
    QSqlQuery query(db);

    // rowid, not the key: one NULL key in the subquery makes NOT IN match nothing.
    struct Cap { const char *table; int rows; };
    for (const Cap cap : {Cap{"history", kHistoryRows}, Cap{"local_progress", 2000}}) {
        if (!query.exec(QStringLiteral("DELETE FROM %1 WHERE rowid NOT IN (SELECT rowid FROM %1 "
                                       "ORDER BY last_played_at DESC LIMIT %2)")
                            .arg(QLatin1String(cap.table)).arg(cap.rows)))
            continue;
        if (const int removed = query.numRowsAffected(); removed > 0)
            logInfo() << "Library" << "dropped" << removed << "stale" << cap.table << "row(s)";
    }

    const qint64 monthAgo = QDateTime::currentSecsSinceEpoch() - 30 * 24 * 60 * 60;

    query.prepare(QStringLiteral(
        "DELETE FROM episode_progress WHERE last_played_at < ? AND show_link NOT IN "
        "(SELECT link FROM shows UNION SELECT link FROM history)"));
    query.addBindValue(monthAgo);
    if (query.exec() && query.numRowsAffected() > 0)
        logInfo() << "Library" << "dropped" << query.numRowsAffected() << "orphaned episode row(s)";

    query.prepare(QStringLiteral("DELETE FROM sync_outbox WHERE queued_at < ?"));
    query.addBindValue(monthAgo);
    if (query.exec() && query.numRowsAffected() > 0)
        logInfo() << "Library" << "dropped" << query.numRowsAffected() << "expired outbox row(s)";

    query.exec(QStringLiteral("DELETE FROM airing WHERE link NOT IN (SELECT link FROM shows)"));
    query.exec(QStringLiteral(
        "DELETE FROM tracker_links WHERE show_link NOT IN "
        "(SELECT link FROM shows UNION SELECT link FROM history)"));

    const auto pages = [&](const char *name) {
        QSqlQuery count(db);
        return count.exec(QStringLiteral("PRAGMA %1").arg(QLatin1String(name))) && count.next()
                   ? count.value(0).toInt() : 0;
    };
    const int freePages = pages("freelist_count");
    if (freePages > 256 && freePages * 2 > pages("page_count")) {
        QSqlQuery vacuum(db);
        if (vacuum.exec(QStringLiteral("VACUUM"))) logInfo() << "Library" << "compacted the database";
    }
}
