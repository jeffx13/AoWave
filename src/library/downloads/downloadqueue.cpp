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
#include <QCollator>
#include <QDesktopServices>
#include <QUrl>
#include <climits>

static bool alreadyDownloaded(const QString &basePath) {
    return QFile::exists(basePath + QStringLiteral(".mp4"))
        || QFile::exists(basePath + QStringLiteral(".mkv"));
}

// slow on dead shares: keep off the GUI thread
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
    connect(&m_pruneWatcher, &QFutureWatcher<Pruned>::finished, this, [this] {
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
    m_resolvePool.waitForDone();
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

int DownloadQueue::active() const {
    return int(std::count_if(m_tasks.cbegin(), m_tasks.cend(), [](const auto &task) {
        return task->status() == DownloadTask::Queued || task->status() == DownloadTask::Running;
    }));
}

int DownloadQueue::paused() const {
    return int(std::count_if(m_tasks.cbegin(), m_tasks.cend(), [](const auto &task) {
        return task->status() == DownloadTask::Paused;
    }));
}

int DownloadQueue::failed() const {
    return int(std::count_if(m_tasks.cbegin(), m_tasks.cend(), [](const auto &task) {
        return task->status() == DownloadTask::Failed;
    }));
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

bool DownloadQueue::downloadLink(const QString &name, const QString &link) {
    if (!DownloadTask::checkDependencies()) {
        MainWindow::instance().reportError(
            tr("N_m3u8DL-RE.exe and ffmpeg.exe must sit next to %1.exe.").arg(QStringLiteral(APP_NAME)), tr("Download"));
        return false;
    }

    QString showLink;
    int episodeIndex = -1;
    if (Provider *provider = ProviderList::forUrl(QUrl::fromUserInput(link.trimmed()), showLink, episodeIndex)) {
        const QString displayName = name.trimmed();
        const CancelToken cancel = m_resolveCancel;
        m_resolvePool.start([this, provider, showLink, episodeIndex, displayName, cancel]() {
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
        return true;
    }

    QString cleanedName = cleanFolderName(name);
    auto task = QSharedPointer<DownloadTask>::create(cleanedName, Settings::instance().downloadDir(),
                                                     link, cleanedName);
    task->maxHeight = Settings::instance().downloadMaxHeight();
    if (alreadyDownloaded(task->basePath()) || m_ongoingPaths.contains(task->basePath())) {
        logWarn() << "Downloads" << "Already exists or downloading" << task->path();
        emit queued(0, 1);
        return false;
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
    emit queued(1, 0);
    return true;
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

    int added = 0, skipped = 0;
    for (int i = startIndex; i <= endIndex; ++i) {
        auto task = QSharedPointer<DownloadTask>::create(playlist->at(i), show.provider, workDir);
        task->maxHeight = Settings::instance().downloadMaxHeight();
        task->origin.showLink = show.link;
        task->origin.showName = show.title;
        task->origin.cover = show.coverUrl;
        if (alreadyDownloaded(task->basePath()) || m_ongoingPaths.contains(task->basePath())
            || !downloadedFile(show.link, task->origin.episodeLink).isEmpty()) {
            logInfo() << "Downloads" << "Already exists or downloading" << task->path();
            ++skipped;
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
        ++added;
    }
    saveQueue();
    startTasks();
    emit queued(added, skipped);
}

void DownloadQueue::runTask(QSharedPointer<DownloadTask> task) {
    if (!DownloadTask::checkDependencies()) {
        { QMutexLocker locker(&m_mutex); m_currentConcurrentDownloads--; }
        QMetaObject::invokeMethod(this, [this, task]() {
            if (task->isCancelled()) {
                removeTask(task, true);
            } else {
                task->setStatus(DownloadTask::Failed);
                task->setProgressText(tr("N_m3u8DL-RE.exe and ffmpeg.exe must sit next to %1.exe.")
                                          .arg(QStringLiteral(APP_NAME)));
                emitRowChanged(rowOf(task));
            }
            startTasks();
        }, Qt::QueuedConnection);
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
                    QString why = task->extractError.trimmed();
                    while (why.endsWith(QLatin1Char('.'))) why.chop(1);
                    task->setProgressText(why.isEmpty() ? tr("Could not find a source. Press Retry.")
                                                        : tr("Could not find a source: %1. Press Retry.").arg(why));
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
    else {
        QDir(task->outDir()).removeRecursively();
        QDir().mkpath(task->tmpDir());
    }

    auto *process = new QProcess(nullptr);
    process->setProgram(task->program());
    process->setArguments(ffmpeg ? task->ffmpegArguments() : task->toolArguments());
    process->setProcessChannelMode(QProcess::MergedChannels);
    task->setProcess(process);
    process->start();

    static QRegularExpression percentRegex(R"((\d+\.\d+)%)");
    static QRegularExpression speedRegex(R"(([\d.]+\s*[KMGTP]?i?B(?:ps|/s)))");
    static QRegularExpression sizeRegex(R"(([\d.]+\s*[KMGTP]?i?B)\s*/\s*([\d.]+\s*[KMGTP]?i?B)(?!ps|/s))");
    // N_m3u8DL-RE runs fields together: "698.81MB/701.04MB192.00KBps"
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
    QByteArray output;

    while (!task->isCancelled() && !task->isPaused()) {
        bool ready = process->waitForReadyRead(1000);
        if (process->bytesAvailable() > 0) {
            const QByteArray chunk = process->readAll();
            output += chunk;
            if (output.size() > (4 << 20)) output.remove(0, output.size() - (2 << 20));
            auto line = chunk.trimmed();
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
    output += process->readAll();

    const bool startFailed = process->error() == QProcess::FailedToStart;
    const int exitCode = process->exitCode();
    const bool ran = !cancelled && !paused && !startFailed;
    // N_m3u8DL-RE exits 0 when parts failed, and leaves no video
    const DownloadTask::ToolReport report = ran && !ffmpeg ? DownloadTask::readToolOutput(output, QUrl(task->link).host())
                                                           : DownloadTask::ToolReport{};
    bool succeeded = ran && exitCode == 0 && !report.failed;
    QString reason = report.reason;

    if (ffmpeg) {
        if (succeeded) {
            QString why;
            if (!Platform::replaceFile(task->partPath(), task->path(), &why)) {
                logWarn() << "Downloads" << "Could not move" << task->partPath() << "to" << task->path() << why;
                task->keepPart = true;
                succeeded = false;
                reason = tr("Could not save %1. Close what is using it and press Retry.").arg(QFileInfo(task->path()).fileName());
            }
        }
        if (!succeeded && !paused && !task->keepPart) QFile::remove(task->partPath());
    } else if (succeeded) {
        if (task->place()) {
            QDir(task->tmpDir()).removeRecursively();
        } else {
            succeeded = false;
            if (reason.isEmpty()) reason = tr("The download tool finished without a video.");
        }
    }
    if (succeeded) task->discardSubtitles();

    QString detail = report.detail;
    if (ran && !succeeded) {
        if (reason.isEmpty() && exitCode != 0) reason = tr("The download tool stopped (exit code %1).").arg(exitCode);
        if (detail.isEmpty()) {
            QStringList lines = QString::fromUtf8(output).split(QRegularExpression(QStringLiteral("[\r\n]+")), Qt::SkipEmptyParts);
            detail = lines.mid(qMax<qsizetype>(0, lines.size() - 12)).join(QLatin1Char('\n'));
        }
        logWarn() << "Downloads" << task->displayName << "failed:" << reason;
        if (!detail.isEmpty()) logInfo() << "Downloads" << "the tool said:" << QString(detail).replace(QLatin1Char('\n'), QStringLiteral(" | "));
    } else if (startFailed) {
        reason = tr("Could not start the download tool.");
        logWarn() << "Downloads" << task->displayName << "could not start" << task->program() << process->errorString();
    }

    {
        QMutexLocker locker(&m_mutex);
        m_currentConcurrentDownloads--;
        task->setProcess(nullptr);
        delete process;
    }

    QMetaObject::invokeMethod(this, [this, task, succeeded, cancelled, paused, ran, reason, detail]() {
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
            task->setDetail(detail);
            const QString why = reason.isEmpty() ? tr("The download stopped.") : reason;
            if (ran && task->autoRetries < kAutoRetries) {
                const int attempt = ++task->autoRetries;
                const int seconds = 10 * attempt;
                task->retryPending = true;
                task->setProgressText(tr("%1 Trying again in %2 s.").arg(why).arg(seconds));
                QTimer::singleShot(seconds * 1000, this, [this, task, attempt]() {
                    if (task->retryPending && task->status() == DownloadTask::Failed && task->autoRetries == attempt)
                        resume(task);
                });
            } else {
                task->setProgressText(tr("%1 Press Retry to resume.").arg(why));
            }
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
    m_sizes.insert(path, QFileInfo(path).size());
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
        Pruned pruned;
        for (const QString &path : paths) {
            if (const QFileInfo file(path); file.exists()) {
                pruned.sizes.insert(path, file.size());
                continue;
            }
            pruned.sizes.insert(path, -1);
            if (const QString other = otherContainer(path); !other.isEmpty() && QFileInfo::exists(other)) {
                pruned.gone.append({path, other});
                pruned.sizes.insert(other, QFileInfo(other).size());
            } else if (folderAnswers(path)) {
                pruned.gone.append({path, {}});
            }
        }
        return pruned;
    }));
}

void DownloadQueue::applyPruned(const Pruned &result) {
    m_prunedAtMs = QDateTime::currentMSecsSinceEpoch();
    m_sizes = result.sizes;
    const QList<Gone> &gone = result.gone;
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

int DownloadQueue::deleteDownloads(const QString &folder, const QString &path) {
    const QString dir = QDir::cleanPath(path.isEmpty() ? folder : QFileInfo(path).absolutePath());
    const bool root = dir == QDir::cleanPath(Settings::instance().downloadDir());
    QStringList paths;
    if (!path.isEmpty()) {
        paths << path;
    } else {
        QList<QSharedPointer<DownloadTask>> going;
        {
            QMutexLocker locker(&m_mutex);
            for (const auto &task : std::as_const(m_tasks))
                if (QDir::cleanPath(task->folder) == dir) going << task;
        }
        for (const auto &task : std::as_const(going)) {
            if (task->status() == DownloadTask::Running && !root) m_dropFolders.insert(dir);
            removeTask(task);
        }
        QSqlQuery query(database());
        if (query.exec(QStringLiteral("SELECT path FROM finished_downloads")))
            while (query.next())
                if (const QString file = query.value(0).toString(); QDir::cleanPath(QFileInfo(file).absolutePath()) == dir)
                    paths << file;
    }

    QStringList moved, gone;
    for (const QString &file : std::as_const(paths)) {
        if (QFile::moveToTrash(file)) moved << file;
        if (!QFileInfo::exists(file)) gone << file;
    }
    QSqlQuery drop(database());
    drop.prepare(QStringLiteral("DELETE FROM finished_downloads WHERE path = ?"));
    for (const QString &file : std::as_const(gone)) {
        drop.addBindValue(file);
        drop.exec();
        m_sizes.remove(file);
    }
    if (gone.size() < paths.size())
        MainWindow::instance().reportError(tr("Could not move %1 of %2 files to the Recycle Bin. Close what is using "
                                              "them and try again.").arg(paths.size() - gone.size()).arg(paths.size()),
                                           tr("Downloads"));
    if (!root) QDir().rmdir(dir);
    logInfo() << "Downloads" << "moved" << moved.size() << "files to the Recycle Bin from" << dir;
    m_keptByShow.clear();
    emit trashed(moved);
    emit finishedChanged();
    return int(moved.size());
}

void DownloadQueue::showInFolder(const QString &path) {
    // Explorer wants /select,path as one quoted argument
    QProcess explorer;
    explorer.setProgram(QStringLiteral("explorer.exe"));
    explorer.setNativeArguments(QStringLiteral("/select,\"%1\"").arg(QDir::toNativeSeparators(path)));
    explorer.startDetached();
}

void DownloadQueue::openFolder(const QString &folder) {
    // # in a name is a fragment in a typed URL
    QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
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
        QDir(task->outDir()).removeRecursively();
    }
    task->discardSubtitles();
    if (const QString folder = QDir::cleanPath(task->folder); m_dropFolders.contains(folder)) {
        QMutexLocker locker(&m_mutex);
        if (std::none_of(m_tasks.cbegin(), m_tasks.cend(), [&](const QSharedPointer<DownloadTask> &other) {
                return other != task && QDir::cleanPath(other->folder) == folder;
            })) {
            QDir().rmdir(folder);
            m_dropFolders.remove(folder);
        }
    }

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
        const bool active = task->status() == DownloadTask::Queued || task->status() == DownloadTask::Running
                            || task->retryPending;
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
    if (wasQueued || task->retryPending) {
        task->retryPending = false;
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
    }
    task->autoRetries = 0;
    resume(task);
}

void DownloadQueue::resume(const QSharedPointer<DownloadTask> &task) {
    {
        QMutexLocker locker(&m_mutex);
        if (!m_tasks.contains(task)) return;
        if (task->status() != DownloadTask::Paused && task->status() != DownloadTask::Failed) return;
        task->retryPending = false;
        task->setPaused(false);
        task->keepPart = false;
        task->setStatus(DownloadTask::Queued);
        if (!m_taskQueue.contains(task)) m_taskQueue.append(task);
    }
    task->setProgressText(tr("Queued"));
    emitRowChanged(rowOf(task));
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

void DownloadQueue::retryFailed() {
    int n;
    { QMutexLocker locker(&m_mutex); n = m_tasks.size(); }
    for (int i = 0; i < n; ++i)
        if (const auto *task = taskAt(i); task && task->status() == DownloadTask::Failed) resumeTask(i);
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

DownloadList::DownloadList(DownloadQueue *queue) : m_queue(queue) {
    m_rebuildSoon.setSingleShot(true);
    connect(&m_rebuildSoon, &QTimer::timeout, this, &DownloadList::rebuild);
    connect(queue, &QAbstractItemModel::rowsInserted, &m_rebuildSoon, qOverload<>(&QTimer::start));
    connect(queue, &QAbstractItemModel::rowsRemoved, this,
            [this](const QModelIndex &, int first, int last) { dropTasks(first, last); });
    connect(queue, &DownloadQueue::finishedChanged, this, &DownloadList::rebuild);
    connect(queue, &DownloadQueue::pruned, this, &DownloadList::rebuild);
    connect(queue, &QAbstractItemModel::dataChanged, this, [this](const QModelIndex &from, const QModelIndex &to) {
        for (int row = from.row(); row <= to.row(); ++row) refreshTask(row);
    });
    m_rebuildSoon.start();
}

int DownloadList::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : int(m_rows.size());
}

QVariant DownloadList::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= m_rows.size()) return {};
    const Row &row = m_rows.at(index.row());
    switch (role) {
    case KeyRole:          return row.key;
    case GroupRole:        return row.group;
    case FolderRole:       return row.folder;
    case TitleRole:        return row.title;
    case CoverRole:        return row.cover;
    case PathRole:         return row.path;
    case StatusRole:       return row.status;
    case ProgressRole:     return row.progress;
    case ProgressTextRole: return row.progressText;
    case StatsRole:        return row.stats;
    case DetailRole:       return row.detail;
    case TaskRowRole:      return row.taskRow;
    case CountRole:        return row.count;
    case ActiveRole:       return row.active;
    case FailedRole:       return row.failed;
    case SizeRole:         return row.size;
    case AtRole:           return row.at;
    case OpenRole:         return row.open;
    case HiddenRole:       return row.hidden;
    case ShowLinkRole:     return row.showLink;
    case TotalRole:        return row.total;
    case TotalSizeRole:    return row.totalSize;
    case TasksRole:        return row.tasks;
    case PausedRole:       return row.paused;
    default:               return {};
    }
}

QHash<int, QByteArray> DownloadList::roleNames() const {
    return {
        {KeyRole, "key"}, {GroupRole, "isGroup"}, {FolderRole, "folder"}, {TitleRole, "title"}, {CoverRole, "cover"},
        {PathRole, "path"}, {StatusRole, "status"}, {ProgressRole, "progress"}, {ProgressTextRole, "progressText"},
        {StatsRole, "stats"}, {DetailRole, "detail"}, {TaskRowRole, "taskRow"}, {CountRole, "count"},
        {ActiveRole, "active"}, {FailedRole, "failed"}, {SizeRole, "size"}, {AtRole, "at"}, {OpenRole, "open"},
        {HiddenRole, "hidden"}, {ShowLinkRole, "showLink"}, {TotalRole, "total"}, {TotalSizeRole, "totalSize"},
        {TasksRole, "tasks"}, {PausedRole, "paused"},
    };
}

DownloadList::Row DownloadList::rowFor(int taskRow, const DownloadTask &task) {
    Row row;
    row.key = QStringLiteral("t:") + task.basePath();
    row.folder = QDir::cleanPath(task.folder);
    row.title = task.videoName;
    row.path = task.basePath();
    row.status = task.status();
    row.progress = task.progressValue();
    row.progressText = task.progressText();
    row.stats = task.stats();
    row.detail = task.detail();
    row.taskRow = taskRow;
    row.number = task.origin.episodeNumber;
    row.season = task.origin.episodeSeason;
    row.showLink = task.origin.showLink;
    return row;
}

void DownloadList::aggregate(Row &group) const {
    int progress = 0;
    double remaining = 0, rate = 0;
    group.tasks = group.active = group.paused = group.failed = 0;
    for (int i = 0; i < m_queue->count(); ++i) {
        const DownloadTask *task = m_queue->taskAt(i);
        if (QDir::cleanPath(task->folder) != group.folder) continue;
        ++group.tasks;
        const int status = task->status();
        if (status == DownloadTask::Failed) ++group.failed;
        if (status == DownloadTask::Paused) ++group.paused;
        if (status != DownloadTask::Queued && status != DownloadTask::Running) continue;
        ++group.active;
        progress += task->progressValue();
        remaining += 100 - task->progressValue();
        if (status == DownloadTask::Running) rate += task->rate();
    }
    group.progress = group.active > 0 ? progress / group.active : 0;
    group.stats = rate > 0 ? tr("%1 left").arg(DownloadTask::formatEta(int(remaining / rate))) : QString();
}

void DownloadList::refreshTask(int row) {
    const DownloadTask *task = m_queue->taskAt(row);
    if (!task) return;
    const QString key = QStringLiteral("t:") + task->basePath(), folder = QDir::cleanPath(task->folder);
    for (int i = 0; i < m_rows.size(); ++i) {
        Row next = m_rows[i];
        if (next.group && next.folder == folder) aggregate(next);
        else if (next.key == key) next = rowFor(row, *task);
        else continue;
        if (next == m_rows[i]) continue;
        m_rows[i] = next;
        emit dataChanged(index(i), index(i));
    }
}

void DownloadList::rebuild() {
    m_rebuildSoon.stop();
    struct Group {
        Row head;
        QList<Row> rows;
        int firstTask = INT_MAX;
        qint64 hiddenAt = 0;
        std::optional<bool> open;
    };
    QHash<QString, Group> groups;
    QSqlQuery query(DownloadQueue::database());
    if (query.exec(QStringLiteral("SELECT folder, open, hidden_at FROM download_groups")))
        while (query.next()) {
            Group &group = groups[query.value(0).toString()];
            if (!query.value(1).isNull()) group.open = query.value(1).toBool();
            group.hiddenAt = query.value(2).toLongLong();
        }
    for (int i = 0; i < m_queue->count(); ++i) {
        const DownloadTask &task = *m_queue->taskAt(i);
        Group &group = groups[QDir::cleanPath(task.folder)];
        group.firstTask = qMin(group.firstTask, i);
        if (group.head.title.isEmpty()) group.head.title = task.origin.showName;
        if (group.head.cover.isEmpty()) group.head.cover = task.origin.cover;
        group.rows.append(rowFor(i, task));
    }
    int hidden = 0;
    bool unsized = false;
    if (query.exec(QStringLiteral("SELECT path, title, finished_at, show_title, cover, episode_number, season, show_link "
                                  "FROM finished_downloads ORDER BY finished_at DESC")))
        while (query.next()) {
            const QString path = query.value(0).toString();
            const qint64 size = m_queue->knownSize(path);
            if (size == -1) continue;
            unsized |= size < 0;
            const QString folder = QDir::cleanPath(path.section(QLatin1Char('/'), 0, -2));
            Group &group = groups[folder];
            Row row;
            row.at = query.value(2).toLongLong();
            row.hidden = row.at <= group.hiddenAt;
            row.size = qMax<qint64>(0, size);
            ++group.head.total;
            group.head.totalSize += row.size;
            if (row.hidden) {
                ++hidden;
                group.head.hidden = true;
                if (!m_showHidden) continue;
            }
            const QString show = query.value(3).toString();
            row.title = query.value(1).toString();
            if (const QString prefix = show + QStringLiteral(" : "); !show.isEmpty() && row.title.startsWith(prefix))
                row.title = row.title.mid(prefix.size());
            row.key = QStringLiteral("f:") + path;
            row.folder = folder;
            row.path = path;
            row.number = query.value(5).toDouble();
            row.season = query.value(6).toInt();
            row.showLink = query.value(7).toString();
            if (group.head.title.isEmpty()) group.head.title = show;
            if (group.head.cover.isEmpty()) group.head.cover = query.value(4).toString();
            group.head.at = qMax(group.head.at, row.at);
            group.head.size += row.size;
            group.rows.append(row);
        }

    QCollator collator;
    collator.setNumericMode(true);
    const auto episodeBefore = [&collator](const Row &a, const Row &b) {
        if (a.season != b.season) return a.season < b.season;
        if ((a.number >= 0) != (b.number >= 0)) return a.number >= 0;
        if (a.number != b.number) return a.number < b.number;
        return collator.compare(a.title, b.title) < 0;
    };
    QList<Group *> order;
    for (auto it = groups.begin(); it != groups.end(); ++it) {
        Group &group = it.value();
        if (group.rows.isEmpty()) continue;
        Row &head = group.head;
        head.key = QStringLiteral("g:") + it.key();
        head.group = true;
        head.folder = it.key();
        if (head.title.isEmpty()) head.title = QFileInfo(it.key()).fileName();
        if (head.title.isEmpty()) head.title = QDir::toNativeSeparators(it.key());
        head.count = int(group.rows.size());
        head.open = group.open.value_or(group.firstTask != INT_MAX
                                        || head.at > QDateTime::currentSecsSinceEpoch() - 60 * 60);
        aggregate(head);
        head.total += head.tasks;
        std::stable_sort(group.rows.begin(), group.rows.end(), episodeBefore);
        order << &group;
    }
    std::sort(order.begin(), order.end(), [&collator](const Group *a, const Group *b) {
        if (a->firstTask != b->firstTask) return a->firstTask < b->firstTask;
        if (a->head.at != b->head.at) return a->head.at > b->head.at;
        if (const int order = collator.compare(a->head.title, b->head.title)) return order < 0;
        return a->head.folder < b->head.folder;
    });
    QList<Row> next;
    for (const Group *group : std::as_const(order)) {
        next << group->head;
        if (group->head.open) next << group->rows;
    }
    apply(next);
    const bool unstuck = hidden == 0 && m_showHidden;
    if (unstuck) m_showHidden = false;
    if (hidden != m_hiddenCount || unstuck) {
        m_hiddenCount = hidden;
        emit hiddenChanged();
    }
    if (unsized) m_queue->pruneMissing();
}

void DownloadList::dropTasks(int first, int last) {
    for (qsizetype i = m_rows.size() - 1; i >= 0; --i) {
        Row &row = m_rows[i];
        if (row.taskRow < first) continue;
        if (row.taskRow <= last) {
            beginRemoveRows(QModelIndex(), int(i), int(i));
            m_rows.removeAt(i);
            endRemoveRows();
        } else {
            row.taskRow -= last - first + 1;
            emit dataChanged(index(int(i)), index(int(i)), {TaskRowRole});
        }
    }
    m_rebuildSoon.start();
}

void DownloadList::apply(const QList<Row> &next) {
    QSet<QString> wanted;
    for (const Row &row : next) wanted.insert(row.key);
    for (qsizetype end = m_rows.size() - 1; end >= 0;) {
        if (wanted.contains(m_rows[end].key)) { --end; continue; }
        qsizetype start = end;
        while (start > 0 && !wanted.contains(m_rows[start - 1].key)) --start;
        beginRemoveRows(QModelIndex(), int(start), int(end));
        m_rows.remove(start, end - start + 1);
        endRemoveRows();
        end = start - 1;
    }
    const auto find = [this](const QString &key, qsizetype from) {
        for (qsizetype i = from; i < m_rows.size(); ++i)
            if (m_rows[i].key == key) return i;
        return qsizetype(-1);
    };
    for (qsizetype i = 0; i < next.size();) {
        if (i < m_rows.size() && m_rows[i].key == next[i].key) {
            if (!(m_rows[i] == next[i])) {
                m_rows[i] = next[i];
                emit dataChanged(index(int(i)), index(int(i)));
            }
            ++i;
            continue;
        }
        if (const qsizetype from = find(next[i].key, i + 1); from >= 0) {
            beginMoveRows(QModelIndex(), int(from), int(from), QModelIndex(), int(i));
            m_rows.move(from, i);
            endMoveRows();
            continue;
        }
        qsizetype end = i;
        while (end + 1 < next.size() && find(next[end + 1].key, i) < 0) ++end;
        beginInsertRows(QModelIndex(), int(i), int(end));
        for (qsizetype k = i; k <= end; ++k) m_rows.insert(k, next[k]);
        endInsertRows();
        i = end + 1;
    }
}

void DownloadList::store(const QString &folder, const char *column, const QVariant &value) {
    QSqlQuery query(DownloadQueue::database());
    query.prepare(QStringLiteral("INSERT INTO download_groups (folder, %1) VALUES (?, ?) "
                                 "ON CONFLICT(folder) DO UPDATE SET %1 = excluded.%1").arg(QLatin1String(column)));
    query.addBindValue(QDir::cleanPath(folder));
    query.addBindValue(value);
    if (!query.exec()) logWarn() << "Downloads" << "could not keep the list's state:" << query.lastError().text();
}

void DownloadList::setShowHidden(bool show) {
    if (m_showHidden == show) return;
    m_showHidden = show;
    emit hiddenChanged();
    rebuild();
}

void DownloadList::setOpen(const QString &folder, bool open) {
    store(folder, "open", open);
    rebuild();
}

qint64 DownloadList::hide(const QString &folder) {
    qint64 mark = 0;
    QSqlQuery query(DownloadQueue::database());
    query.prepare(QStringLiteral("SELECT hidden_at FROM download_groups WHERE folder = ?"));
    query.addBindValue(QDir::cleanPath(folder));
    if (query.exec() && query.next()) mark = query.value(0).toLongLong();
    store(folder, "hidden_at", QDateTime::currentSecsSinceEpoch());
    rebuild();
    return mark;
}

void DownloadList::unhide(const QString &folder, qint64 mark) {
    store(folder, "hidden_at", mark);
    rebuild();
}
