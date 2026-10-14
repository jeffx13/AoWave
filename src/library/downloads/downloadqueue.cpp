#include "library/downloads/downloadqueue.h"
#include <QSqlError>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QDateTime>
#include <QSqlDatabase>
#include <QSqlQuery>
#include "library/store.h"
#include <QRegularExpression>
#include "providers/showdata.h"
#include "providers/provider.h"
#include "providers/providerlist.h"
#include "app/mainwindow.h"
#include "app/logger.h"
#include "app/settings.h"
#include "system/platform.h"
#include "net/client.h"
#include "app/async.h"
#include <QtConcurrent/QtConcurrentRun>

static bool alreadyDownloaded(const QString &basePath) {
    return QFile::exists(basePath + QStringLiteral(".mp4"))
        || QFile::exists(basePath + QStringLiteral(".mkv"));
}

// A missing file is gone for good only if a folder above it, not a drive or share root, still answers; a dead share is slow, so not on the GUI thread.
static bool folderAnswers(const QString &path) {
    QString folder = QDir::cleanPath(QFileInfo(path).path());
    for (;;) {
        const QDir dir(folder);
        if (dir.exists()) return !dir.isRoot();
        const QString parent = QDir::cleanPath(folder + QStringLiteral("/.."));
        if (dir.isRoot() || parent == folder) return false;
        folder = parent;
    }
}

static QString otherContainer(const QString &path) {
    if (path.endsWith(QLatin1String(".mp4"), Qt::CaseInsensitive)) return path.chopped(4) + QStringLiteral(".mkv");
    if (path.endsWith(QLatin1String(".mkv"), Qt::CaseInsensitive)) return path.chopped(4) + QStringLiteral(".mp4");
    return {};
}

QString DownloadQueue::cleanFolderName(const QString &name) {
    static const QList<QPair<QChar, QChar>> replacements = {
        {':', u'꞉'}, {'"', '\''}, {'?', u'？'}, {'*', u'∗'},
        {'|', u'｜'}, {'<', u'≺'}, {'>', u'≻'}, {'/', u'∕'}, {'\\', u'⧵'}
    };
    static const QRegularExpression control(QStringLiteral("[\\x00-\\x1F]"));
    static const QRegularExpression device(QStringLiteral("^(con|prn|aux|nul|com[1-9]|lpt[1-9])(\\..*)?$"),
                                           QRegularExpression::CaseInsensitiveOption);
    QString result = name;
    for (const auto &[from, to] : replacements)
        result.replace(from, to);
    result.remove(control);
    while (!result.isEmpty() && (result.endsWith(' ') || result.endsWith('.')))
        result.chop(1);
    result = result.trimmed();
    if (const auto match = device.match(result); match.hasMatch()) result.insert(match.capturedLength(1), QLatin1Char('_'));
    return result.isEmpty() ? QStringLiteral("download") : result;
}

QString DownloadQueue::showFolder(const QString &showTitle) {
    return Settings::instance().downloadDir() + "/" + cleanFolderName(showTitle);
}

QString DownloadQueue::episodeFileName(const PlaylistItem &episode) {
    return cleanFolderName(episode.displayName.trimmed().replace("\n", ". "));
}

DownloadQueue::EpisodeState DownloadQueue::episodeState(const QString &showTitle, const QString &showLink,
                                                      const PlaylistItem &episode) const {
    const QString base = QDir::cleanPath(showFolder(showTitle) + "/" + episodeFileName(episode));
    if (m_ongoingPaths.contains(base)) return Queued;
    if (!downloadedFile(showLink, episode.link).isEmpty()) return Downloaded;
    return alreadyDownloaded(base) ? Downloaded : NotDownloaded;
}

QString DownloadQueue::downloadedFile(const QString &showLink, const QString &episodeLink) const {
    if (showLink.isEmpty() || episodeLink.isEmpty()) return {};
    auto it = m_keptByShow.find(showLink);
    if (it == m_keptByShow.end()) {
        QHash<QString, QString> files;
        QSqlQuery query(database());
        query.prepare(QStringLiteral("SELECT episode_link, path FROM finished_downloads WHERE show_link = ? "
                                     "ORDER BY finished_at"));
        query.addBindValue(showLink);
        if (query.exec())
            while (query.next()) files.insert(query.value(0).toString(), query.value(1).toString());
        it = m_keptByShow.insert(showLink, files);
    }
    const QString path = it->value(episodeLink);
    if (path.isEmpty()) return {};
    if (QFileInfo::exists(path)) return path;
    if (QDateTime::currentMSecsSinceEpoch() - m_prunedAtMs > 10000) m_pruneSoon.start();
    return {};
}

DownloadQueue::Kept DownloadQueue::keptFor(const QString &path) const {
    Kept kept;
    QSqlQuery query(database());
    query.prepare(QStringLiteral("SELECT show_link, provider, show_title, cover, episode_link, episode_index, "
                                 "episode_number, season FROM finished_downloads WHERE path = ? AND show_link != ''"));
    query.addBindValue(QDir::cleanPath(path));
    if (query.exec() && query.next()) {
        kept = {true, query.value(0).toString(), query.value(1).toString(), query.value(2).toString(),
                query.value(3).toString(), query.value(4).toString(), query.value(5).toInt(),
                float(query.value(6).toDouble()), query.value(7).toInt()};
    }
    return kept;
}

DownloadQueue::DownloadQueue(QObject *parent)
    : QAbstractListModel(parent)
    , m_maxDownloads(qBound(1, Settings::instance().get(Config::MaxDownloads), kMaxDownloadsLimit))
{
    m_threadPool.setMaxThreadCount(m_maxDownloads);
    for (auto signal : {&QAbstractItemModel::rowsInserted, &QAbstractItemModel::rowsRemoved})
        connect(this, signal, this, &DownloadQueue::overviewChanged);
    connect(this, &QAbstractItemModel::dataChanged, this, &DownloadQueue::overviewChanged);
    m_pruneSoon.setSingleShot(true);
    m_pruneSoon.setInterval(1500);
    connect(&m_pruneSoon, &QTimer::timeout, this, &DownloadQueue::pruneMissing);
    connect(&m_pruneWatcher, &QFutureWatcher<QList<Gone>>::finished, this, [this] {
        applyPruned(m_pruneWatcher.result());
        if (std::exchange(m_pruneAgain, false)) pruneMissing();
    });
}

DownloadQueue::~DownloadQueue() {
    saveQueue();
    waitFor(m_pruneWatcher, "Downloads prune");
    m_resolveCancel.cancel();
    {
        QMutexLocker locker(&m_mutex);
        m_taskQueue.clear();
        for (const auto &task : std::as_const(m_tasks)) {
            task->setPaused(true);
            task->cancel();
        }
    }
    m_threadPool.waitForDone();
    database() = QSqlDatabase();
    QSqlDatabase::removeDatabase(QStringLiteral("downloads"));
}

QSqlDatabase &DownloadQueue::database() {
    static QSqlDatabase db;
    if (!db.isValid()) {
        db = Store::open(QStringLiteral("downloads"));
        if (!db.isOpen()) logError() << "Downloads" << "the queue cannot be kept:" << db.lastError().text();
    }
    return db;
}

void DownloadQueue::saveQueue() const {
    QSqlDatabase &db = database();
    if (!db.isOpen()) return;

    QList<QSharedPointer<DownloadTask>> tasks;
    {
        QMutexLocker locker(&m_mutex);
        tasks = m_tasks;
    }

    if (!db.transaction()) {
        logWarn() << "Downloads" << "could not save the queue:" << db.lastError().text();
        return;
    }
    QSqlQuery query(db);
    if (!query.exec(QStringLiteral("DELETE FROM downloads"))) {
        db.rollback();
        logWarn() << "Downloads" << "could not save the queue:" << query.lastError().text();
        return;
    }
    query.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO downloads (base_path, video_name, folder, link, display_name, "
        "provider, episode_link, show_name, episode_number, episode_season, queued_at, max_height, "
        "show_link, cover, episode_index) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    for (const auto &task : std::as_const(tasks)) {
        const DownloadTask::Origin &origin = task->origin;
        query.addBindValue(task->basePath());
        query.addBindValue(task->videoName);
        query.addBindValue(task->folder);
        query.addBindValue(origin.provider.isEmpty() ? task->link : QString());
        query.addBindValue(task->displayName);
        query.addBindValue(origin.provider);
        query.addBindValue(origin.episodeLink);
        query.addBindValue(origin.showName);
        query.addBindValue(double(origin.episodeNumber));
        query.addBindValue(origin.episodeSeason);
        query.addBindValue(now);
        query.addBindValue(task->maxHeight);
        query.addBindValue(origin.showLink);
        query.addBindValue(origin.cover);
        query.addBindValue(origin.episodeIndex);
        if (!query.exec()) {
            db.rollback();
            logWarn() << "Downloads" << "could not save the queue, the earlier one stays:" << query.lastError().text();
            return;
        }
    }
    if (!db.commit()) {
        db.rollback();
        logWarn() << "Downloads" << "could not save the queue";
    }
}

void DownloadQueue::restoreQueue() {
    QSqlDatabase &db = database();
    if (!db.isOpen()) return;

    QSqlQuery query(db);
    if (!query.exec(QStringLiteral(
            "SELECT video_name, folder, link, display_name, provider, episode_link, show_name, "
            "episode_number, episode_season, max_height, show_link, cover, episode_index "
            "FROM downloads ORDER BY queued_at, rowid"))) {
        logWarn() << "Downloads" << "could not read the queue:" << query.lastError().text();
        return;
    }

    QList<QSharedPointer<DownloadTask>> restored;
    while (query.next()) {
        const DownloadTask::Origin origin{query.value(4).toString(), query.value(5).toString(),
                                          query.value(6).toString(), float(query.value(7).toDouble()),
                                          query.value(8).toInt(), query.value(10).toString(),
                                          query.value(11).toString(), query.value(12).toInt()};
        auto task = DownloadTask::restore(query.value(0).toString(), query.value(1).toString(),
                                          query.value(2).toString(), query.value(3).toString(), origin);
        if (!task || alreadyDownloaded(task->basePath())) continue;
        task->maxHeight = query.value(9).toInt();
        restored.append(task);
    }
    if (restored.isEmpty()) return;

    beginInsertRows(QModelIndex(), m_tasks.size(), m_tasks.size() + restored.size() - 1);
    {
        QMutexLocker locker(&m_mutex);
        for (const auto &task : restored) {
            m_ongoingPaths.insert(task->basePath());
            m_tasks.push_back(task);
        }
    }
    endInsertRows();
    logInfo() << "Downloads" << "restored" << QString::number(restored.size()) << "paused task(s)";
}

int DownloadQueue::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : m_tasks.count();
}

QVariant DownloadQueue::data(const QModelIndex &index, int role) const {
    auto *task = taskAt(index.row());
    if (!task) return {};
    switch (role) {
    case NameRole:          return task->displayName;
    case PathRole:          return task->path();
    case ProgressValueRole: return task->progressValue();
    case ProgressTextRole:  return task->progressText();
    case StatusRole:        return task->status();
    case StatsRole:         return task->stats();
    default:                return {};
    }
}

QHash<int, QByteArray> DownloadQueue::roleNames() const {
    return {
        {NameRole, "downloadName"}, {PathRole, "downloadPath"},
        {ProgressValueRole, "progressValue"}, {ProgressTextRole, "progressText"},
        {StatusRole, "status"}, {StatsRole, "stats"},
    };
}

int DownloadQueue::downloading() const {
    return int(std::count_if(m_tasks.cbegin(), m_tasks.cend(), [](const auto &task) {
        return task->status() == DownloadTask::Running;
    }));
}

QString DownloadQueue::overview() const {
    int active = 0, progress = 0;
    double remaining = 0, rate = 0;
    for (const auto &task : m_tasks) {
        const int status = task->status();
        if (status != DownloadTask::Queued && status != DownloadTask::Running) continue;
        ++active;
        progress += task->progressValue();
        remaining += 100 - task->progressValue();
        if (status == DownloadTask::Running) rate += task->rate();
    }
    if (active == 0) return {};
    QString text = tr("%1% overall").arg(progress / active);
    if (rate > 0) text += QStringLiteral(" \u00b7 ") + tr("%1 left").arg(DownloadTask::formatEta(int(remaining / rate)));
    return text;
}

void DownloadQueue::emitRowChanged(int row) {
    if (row < 0) return;
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this, row]() {
            if (row < m_tasks.size()) { auto i = index(row); emit dataChanged(i, i); }
        }, Qt::QueuedConnection);
    } else if (row < m_tasks.size()) {
        auto i = index(row);
        emit dataChanged(i, i);
    }
}

int DownloadQueue::rowOf(const QSharedPointer<DownloadTask> &task) const {
    QMutexLocker locker(&m_mutex);
    return m_tasks.indexOf(task);
}

void DownloadQueue::downloadLink(const QString &name, const QString &link) {
    if (!DownloadTask::checkDependencies()) {
        MainWindow::instance().reportError(
            tr("N_m3u8DL-RE.exe and ffmpeg.exe must sit next to %1.exe.").arg(QStringLiteral(APP_NAME)), tr("Download"));
        return;
    }

    QString showLink;
    int episodeIndex = -1;
    if (Provider *provider = ProviderList::forUrl(QUrl::fromUserInput(link.trimmed()), showLink, episodeIndex)) {
        const QString displayName = name.trimmed();
        const CancelToken cancel = m_resolveCancel;
        m_threadPool.start([this, provider, showLink, episodeIndex, displayName, cancel]() {
            ShowData show(displayName, showLink, {}, provider);
            try {
                Client client(cancel);
                provider->loadShow(&client, show);
            } catch (const std::exception &error) {
                if (!cancel.isCancelled())
                    MainWindow::instance().reportError(QString::fromUtf8(error.what()), tr("Download"));
                return;
            }
            if (cancel.isCancelled()) return;

            const auto playlist = show.playlist();
            if (!playlist || playlist->isEmpty()) {
                MainWindow::instance().reportError(tr("Nothing to download at that address."), tr("Download"));
                return;
            }
            const int named = episodeIndex >= 0 ? episodeIndex : playlist->currentIndex();
            const int first = named >= 0 ? named : 0;
            const int last  = named >= 0 ? named : playlist->count() - 1;
            if (!playlist->isValidIndex(first)) {
                MainWindow::instance().reportError(tr("That episode does not exist."), tr("Download"));
                return;
            }
            QMetaObject::invokeMethod(this, [this, show = std::move(show), first, last]() {
                downloadShow(show, first, last);
            }, Qt::QueuedConnection);
        });
        return;
    }

    QString cleanedName = cleanFolderName(name);
    auto task = QSharedPointer<DownloadTask>::create(cleanedName, Settings::instance().downloadDir(),
                                                     link, cleanedName);
    task->maxHeight = Settings::instance().downloadMaxHeight();
    if (alreadyDownloaded(task->basePath()) || m_ongoingPaths.contains(task->basePath())) {
        logWarn() << "Downloads" << "Already exists or downloading" << task->path();
        return;
    }
    beginInsertRows(QModelIndex(), m_tasks.size(), m_tasks.size());
    {
        QMutexLocker locker(&m_mutex);
        m_ongoingPaths.insert(task->basePath());
        m_tasks.push_back(task);
        m_taskQueue.append(task);
    }
    endInsertRows();
    saveQueue();
    startTasks();
}

void DownloadQueue::downloadShow(const ShowData &show, int startIndex, int endIndex) {
    if (!DownloadTask::checkDependencies()) {
        MainWindow::instance().reportError(
            tr("N_m3u8DL-RE.exe and ffmpeg.exe must sit next to %1.exe.").arg(QStringLiteral(APP_NAME)), tr("Download"));
        return;
    }

    auto playlist = show.playlist();
    if (!playlist || !playlist->isValidIndex(startIndex)) return;

    if (endIndex < startIndex) std::swap(startIndex, endIndex);
    endIndex = qMin(endIndex, playlist->count() - 1);

    const QString workDir = showFolder(show.title);
    logInfo() << "Downloads" << show.title << "from" << startIndex << "to" << endIndex;

    for (int i = startIndex; i <= endIndex; ++i) {
        auto task = QSharedPointer<DownloadTask>::create(playlist->at(i), show.provider, workDir);
        task->maxHeight = Settings::instance().downloadMaxHeight();
        task->origin.showLink = show.link;
        task->origin.showName = show.title;
        task->origin.cover = show.coverUrl;
        if (alreadyDownloaded(task->basePath()) || m_ongoingPaths.contains(task->basePath())) {
            logInfo() << "Downloads" << "Already exists or downloading" << task->path();
            continue;
        }
        beginInsertRows(QModelIndex(), m_tasks.size(), m_tasks.size());
        {
            QMutexLocker locker(&m_mutex);
            m_ongoingPaths.insert(task->basePath());
            m_tasks.push_back(task);
            m_taskQueue.append(task);
        }
        endInsertRows();
    }
    saveQueue();
    startTasks();
}

void DownloadQueue::runTask(QSharedPointer<DownloadTask> task) {
    if (!DownloadTask::checkDependencies()) {
        { QMutexLocker locker(&m_mutex); m_currentConcurrentDownloads--; }
        QMetaObject::invokeMethod(this, [this]() { startTasks(); }, Qt::QueuedConnection);
        return;
    }

    if (task->link.isEmpty()) {
        task->link = task->extractLink();
        if (task->link.isEmpty()) {
            { QMutexLocker locker(&m_mutex); m_currentConcurrentDownloads--; }
            QMetaObject::invokeMethod(this, [this, task]() {
                if (task->isCancelled()) {
                    removeTask(task, true);
                } else {
                    task->setStatus(DownloadTask::Failed);
                    task->setProgressText(tr("Could not find a source. Press Retry."));
                    emitRowChanged(rowOf(task));
                }
                startTasks();
            }, Qt::QueuedConnection);
            return;
        }
    }
    task->refreshLink();

    task->prepareSubtitles();
    emitRowChanged(rowOf(task));

    const bool ffmpeg = task->usesFfmpeg();
    if (ffmpeg) QDir().mkpath(task->folder);
    else        QDir().mkpath(task->tmpDir());

    auto *process = new QProcess(nullptr);
    process->setProgram(task->program());
    process->setArguments(ffmpeg ? task->ffmpegArguments() : task->toolArguments());
    process->setProcessChannelMode(QProcess::MergedChannels);
    task->setProcess(process);
    process->start();

    static QRegularExpression percentRegex(R"((\d+\.\d+)%)");
    static QRegularExpression speedRegex(R"(([\d.]+\s*[KMGTP]?i?B(?:ps|/s)))");
    static QRegularExpression sizeRegex(R"(([\d.]+\s*[KMGTP]?i?B)\s*/\s*([\d.]+\s*[KMGTP]?i?B)(?!ps|/s))");
    // N_m3u8DL-RE runs its fields together once its bars are gone: "698.81MB/701.04MB192.00KBps".
    static QRegularExpression unitRegex(R"(([\d.])\s*([KMGTP]?i?B))");
    const auto readable = [](QString amount) {
        amount.replace(unitRegex, QStringLiteral("\\1 \\2"));
        if (amount.endsWith(QLatin1String("ps"))) { amount.chop(2); amount += QLatin1String("/s"); }
        return amount;
    };
    static QRegularExpression ffDurRegex(R"(Duration:\s*(\d+):(\d+):(\d+)\.(\d+))");
    static QRegularExpression ffTimeRegex(R"(time=\s*(\d+):(\d+):(\d+)\.(\d+))");
    auto ffSeconds = [](const QRegularExpressionMatch &m) {
        return m.captured(1).toInt() * 3600 + m.captured(2).toInt() * 60
             + m.captured(3).toInt() + m.captured(4).toInt() / 100.0;
    };
    double ffTotal = -1;
    bool reportedError = false;

    while (!task->isCancelled() && !task->isPaused()) {
        bool ready = process->waitForReadyRead(1000);
        if (process->bytesAvailable() > 0) {
            auto line = process->readAll().trimmed();
            line.replace("━", "");
            if (ffmpeg) {
                if (ffTotal < 0) {
                    auto dm = ffDurRegex.match(line);
                    if (dm.hasMatch()) ffTotal = ffSeconds(dm);
                }
                double cur = -1;
                for (auto it = ffTimeRegex.globalMatch(line); it.hasNext(); )
                    cur = ffSeconds(it.next());
                if (cur >= 0 && ffTotal > 0) {
                    task->setProgressValue(qBound(0, int(cur / ffTotal * 100), 100));
                    task->setProgressText(tr("Downloading…"));
                }
            } else {
                auto match = percentRegex.match(line);
                if (match.hasMatch())
                    task->setProgressValue(static_cast<int>(match.captured(1).toFloat()));
                else if (line.contains("ERROR:") && !reportedError) {
                    reportedError = true;
                    MainWindow::instance().reportError(QString("%1\n%2").arg(task->displayName, line),
                                                     tr("Download error"));
                }
                auto sm = speedRegex.match(line);
                if (sm.hasMatch()) task->setSpeed(readable(sm.captured(1)));
                const auto size = sizeRegex.match(line);
                task->setProgressText(size.hasMatch() ? tr("%1 of %2").arg(readable(size.captured(1)), readable(size.captured(2)))
                                      : match.hasMatch() ? tr("Downloading…") : QString::fromUtf8(line));
            }
            const int i = rowOf(task);
            emitRowChanged(i);
        }
        if (!ready && process->state() != QProcess::Running)
            break;
    }

    const bool cancelled = task->isCancelled();
    const bool paused    = task->isPaused();
    if (cancelled || paused) process->kill();
    process->waitForFinished(-1);

    const bool startFailed = process->error() == QProcess::FailedToStart;
    bool succeeded = !cancelled && !paused && !startFailed && process->exitCode() == 0;

    if (ffmpeg) {
        if (succeeded) {
            QString why;
            if (!Platform::replaceFile(task->partPath(), task->path(), &why)) {
                logWarn() << "Downloads" << "Could not move" << task->partPath() << "to" << task->path() << why;
                task->keepPart = true;
                succeeded = false;
            }
        }
        if (!succeeded && !paused && !task->keepPart) QFile::remove(task->partPath());
    } else if (succeeded) {
        QDir(task->tmpDir()).removeRecursively();   // --del-after-done leaves the wrapper behind
        task->addDanmaku();
    }
    if (succeeded) task->discardSubtitles();

    {
        QMutexLocker locker(&m_mutex);
        m_currentConcurrentDownloads--;
        task->setProcess(nullptr);
        delete process;
    }

    QMetaObject::invokeMethod(this, [this, task, succeeded, cancelled, paused]() {
        if (cancelled) {
            removeTask(task, true);
        } else if (paused) {
            task->setPaused(false);
            task->setStatus(DownloadTask::Paused);
            task->setProgressText(tr("Paused"));
            emitRowChanged(rowOf(task));
        } else if (succeeded) {
            recordFinished(*task);
            removeTask(task, true);
        } else {
            task->setStatus(DownloadTask::Failed);
            task->setProgressText(tr("Failed. Press Retry to resume."));
            emitRowChanged(rowOf(task));
        }
        startTasks();
    }, Qt::QueuedConnection);
}

void DownloadQueue::recordFinished(const DownloadTask &task) {
    const QString path = QDir::cleanPath(task.path());
    const DownloadTask::Origin &origin = task.origin;
    QSqlQuery query(database());
    query.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO finished_downloads (path, title, finished_at, show_link, provider, show_title, "
        "cover, episode_link, episode_index, episode_number, season) VALUES (?,?,?,?,?,?,?,?,?,?,?)"));
    query.addBindValue(path);
    query.addBindValue(task.displayName);
    query.addBindValue(QDateTime::currentSecsSinceEpoch());
    query.addBindValue(origin.showLink);
    query.addBindValue(origin.provider);
    query.addBindValue(origin.showName);
    query.addBindValue(origin.cover);
    query.addBindValue(origin.episodeLink);
    query.addBindValue(origin.episodeIndex);
    query.addBindValue(double(origin.episodeNumber));
    query.addBindValue(origin.episodeSeason);
    if (!query.exec()) logWarn() << "Downloads" << "Could not record a finished download:" << query.lastError().text();
    m_keptByShow.remove(origin.showLink);
    emit finishedChanged();
    emit downloadFinished(path, task.displayName);
}

QVariantList DownloadQueue::finished() const {
    QSqlQuery query(database());
    QVariantList list;
    if (!query.exec(QStringLiteral("SELECT path, title, finished_at, show_link FROM finished_downloads "
                                   "ORDER BY finished_at DESC")))
        return list;
    while (query.next()) {
        const QString path = query.value(0).toString();
        if (QFileInfo::exists(path))
            list.append(QVariantMap{{"path", path}, {"title", query.value(1)}, {"at", query.value(2)},
                                    {"folder", QFileInfo(path).absolutePath()}, {"showLink", query.value(3)}});
    }
    return list;
}

void DownloadQueue::pruneMissing() {
    if (m_pruneWatcher.isRunning()) {
        m_pruneAgain = true;
        return;
    }
    QStringList paths;
    QSqlQuery query(database());
    if (query.exec(QStringLiteral("SELECT path FROM finished_downloads")))
        while (query.next()) paths.append(query.value(0).toString());
    if (paths.isEmpty()) {
        emit pruned(0);
        return;
    }
    m_pruneWatcher.setFuture(QtConcurrent::run([paths] {
        QList<Gone> gone;
        for (const QString &path : paths) {
            if (QFileInfo::exists(path)) continue;
            if (const QString other = otherContainer(path); !other.isEmpty() && QFileInfo::exists(other))
                gone.append({path, other});
            else if (folderAnswers(path))
                gone.append({path, {}});
        }
        return gone;
    }));
}

void DownloadQueue::applyPruned(const QList<Gone> &gone) {
    m_prunedAtMs = QDateTime::currentMSecsSinceEpoch();
    int removed = 0;
    if (!gone.isEmpty()) {
        QSqlDatabase &db = database();
        db.transaction();
        QSqlQuery drop(db), repoint(db);
        drop.prepare(QStringLiteral("DELETE FROM finished_downloads WHERE path = ?"));
        repoint.prepare(QStringLiteral("UPDATE OR REPLACE finished_downloads SET path = ? WHERE path = ?"));
        for (const Gone &entry : gone) {
            QSqlQuery &statement = entry.variant.isEmpty() ? drop : repoint;
            if (!entry.variant.isEmpty()) statement.addBindValue(entry.variant);
            statement.addBindValue(entry.path);
            if (statement.exec() && entry.variant.isEmpty()) ++removed;
        }
        db.commit();
        logInfo() << "Downloads" << QStringLiteral("forgot %1 finished downloads whose files are gone, kept %2 moved to the other container")
                                        .arg(removed).arg(gone.size() - removed);
        m_keptByShow.clear();
        emit finishedChanged();
    }
    emit pruned(removed);
}

void DownloadQueue::forgetFinished(const QString &path) {
    QSqlQuery query(database());
    query.prepare(QStringLiteral("DELETE FROM finished_downloads WHERE path = ?"));
    query.addBindValue(path);
    m_keptByShow.clear();
    if (query.exec()) emit finishedChanged();
}

void DownloadQueue::clearFinished() {
    QSqlQuery query(database());
    m_keptByShow.clear();
    if (query.exec(QStringLiteral("DELETE FROM finished_downloads"))) emit finishedChanged();
}

void DownloadQueue::showInFolder(const QString &path) {
    // Explorer takes the file after /select as one quoted argument, which QProcess would quote whole.
    QProcess explorer;
    explorer.setProgram(QStringLiteral("explorer.exe"));
    explorer.setNativeArguments(QStringLiteral("/select,\"%1\"").arg(QDir::toNativeSeparators(path)));
    explorer.startDetached();
}

void DownloadQueue::removeTask(const QSharedPointer<DownloadTask> &task, bool force) {
    int idx;
    bool workerOwnsIt;
    {
        QMutexLocker locker(&m_mutex);
        idx = m_tasks.indexOf(task);
        if (idx == -1) return;
        m_taskQueue.removeOne(task);
        workerOwnsIt = !force && task->status() == DownloadTask::Running;
        if (workerOwnsIt) {
            logInfo() << "Downloads" << "Cancelling" << task->displayName;
            task->cancel();
            task->setProgressText(tr("Cancelling"));
        }
    }
    if (workerOwnsIt) { emitRowChanged(idx); return; }

    if (task->usesFfmpeg()) {
        if (!task->keepPart) QFile::remove(task->partPath());
    } else {
        QDir(task->tmpDir()).removeRecursively();
    }
    task->discardSubtitles();

    beginRemoveRows(QModelIndex(), idx, idx);
    {
        QMutexLocker locker(&m_mutex);
        m_ongoingPaths.remove(task->basePath());
        m_tasks.removeAt(idx);
    }
    endRemoveRows();
    saveQueue();
}

void DownloadQueue::cancelTask(int index) {
    QSharedPointer<DownloadTask> task;
    { QMutexLocker locker(&m_mutex); if (index >= 0 && index < m_tasks.size()) task = m_tasks[index]; }
    if (task) removeTask(task);
}

int DownloadQueue::rowOfPath(const QString &path) const {
    QMutexLocker locker(&m_mutex);
    for (int i = 0; i < m_tasks.size(); ++i)
        if (m_tasks[i]->path() == path) return i;
    return -1;
}

QVariantList DownloadQueue::holdCancel(int index) {
    QList<QSharedPointer<DownloadTask>> tasks;
    {
        QMutexLocker locker(&m_mutex);
        if (index < 0) tasks = m_tasks;
        else if (index < m_tasks.size()) tasks = {m_tasks[index]};
    }
    QVariantList held;
    for (const auto &task : std::as_const(tasks)) {
        const bool active = task->status() == DownloadTask::Queued || task->status() == DownloadTask::Running;
        held.append(QVariantMap{{"path", task->path()}, {"active", active}});
        if (active) pauseTask(rowOf(task));
    }
    return held;
}

void DownloadQueue::settleCancel(const QVariantList &held) {
    for (const QVariant &entry : held) {
        const int row = rowOfPath(entry.toMap().value("path").toString());
        const auto *task = taskAt(row);
        if (task && (task->status() == DownloadTask::Paused || task->status() == DownloadTask::Failed))
            cancelTask(row);
    }
}

void DownloadQueue::undoCancel(const QVariantList &held) {
    for (const QVariant &entry : held)
        if (entry.toMap().value("active").toBool())
            resumeTask(rowOfPath(entry.toMap().value("path").toString()));
}

void DownloadQueue::pauseTask(int index) {
    QSharedPointer<DownloadTask> task;
    bool wasQueued = false;
    {
        QMutexLocker locker(&m_mutex);
        if (index < 0 || index >= m_tasks.size()) return;
        task = m_tasks[index];
        if (task->status() == DownloadTask::Queued) {
            m_taskQueue.removeOne(task);
            wasQueued = true;
        }
    }
    if (wasQueued) {
        task->setStatus(DownloadTask::Paused);
        task->setProgressText(tr("Paused"));
        emitRowChanged(index);
    } else if (task->status() == DownloadTask::Running) {
        task->setPaused(true);
        task->setProgressText(tr("Pausing…"));
        emitRowChanged(index);
    }
}

void DownloadQueue::resumeTask(int index) {
    QSharedPointer<DownloadTask> task;
    {
        QMutexLocker locker(&m_mutex);
        if (index < 0 || index >= m_tasks.size()) return;
        task = m_tasks[index];
        if (task->status() != DownloadTask::Paused && task->status() != DownloadTask::Failed) return;
        task->setPaused(false);
        task->keepPart = false;
        task->setStatus(DownloadTask::Queued);
        if (!m_taskQueue.contains(task)) m_taskQueue.append(task);
    }
    task->setProgressText(tr("Queued"));
    emitRowChanged(index);
    startTasks();
}

void DownloadQueue::pauseAll() {
    int n;
    { QMutexLocker locker(&m_mutex); n = m_tasks.size(); }
    for (int i = 0; i < n; ++i) pauseTask(i);
}

void DownloadQueue::resumeAll() {
    int n;
    { QMutexLocker locker(&m_mutex); n = m_tasks.size(); }
    for (int i = 0; i < n; ++i) resumeTask(i);
}

void DownloadQueue::startTasks() {
    QList<int> startedRows;
    {
        QMutexLocker locker(&m_mutex);
        while (!m_taskQueue.isEmpty() && m_currentConcurrentDownloads < m_maxDownloads) {
            auto task = m_taskQueue.takeFirst();
            m_currentConcurrentDownloads++;
            task->maxSpeed = Settings::instance().get(Config::MaxSpeed);
            if (Settings::instance().get(Config::DownloadDanmaku)) task->danmaku = DanmakuOptions::current();
            else task->danmaku.reset();
            task->setStatus(DownloadTask::Running);
            task->resetStats();
            task->setProgressText(tr("Starting…"));
            int row = m_tasks.indexOf(task);
            if (row >= 0) startedRows.append(row);
            m_threadPool.start([this, task]() { runTask(task); });
        }
    }
    for (int row : startedRows) emitRowChanged(row);
}

int DownloadQueue::maxDownloads() const { return m_maxDownloads; }

void DownloadQueue::setMaxDownloads(int n) {
    n = qBound(1, n, kMaxDownloadsLimit);
    if (m_maxDownloads == n) return;
    m_maxDownloads = n;
    Settings::instance().set(Config::MaxDownloads, n);
    m_threadPool.setMaxThreadCount(n);
    emit maxDownloadsChanged();
    startTasks();
}
