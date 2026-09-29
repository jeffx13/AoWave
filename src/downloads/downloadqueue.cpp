#include "downloads/downloadqueue.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QDateTime>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include "library/store.h"
#include <QtConcurrent/QtConcurrentRun>
#include <QDateTime>
#include <QRegularExpression>
#include "shows/playlistitem.h"
#include "shows/showprovider.h"
#include "shows/providerlist.h"
#include "shows/showdata.h"
#include "core/appshell.h"
#include "core/logger.h"
#include "media/serverselector.h"
#include "core/settings.h"
#include "core/exception.h"
#include "net/client.h"
#include <QCryptographicHash>
#include <QSaveFile>
#include <climits>

DownloadTask::DownloadTask(const QString &videoName, const QString &folder, const QString &link,
                           const QString &displayName, const QMap<QString, QString> &headers)
    : videoName(videoName), folder(folder), link(link), headers(headers), displayName(displayName)
{}

DownloadTask::DownloadTask(QSharedPointer<PlaylistItem> episode, ShowProvider *provider, const QString &workDir)
    : m_episode(episode), m_provider(provider)
{
    if (episode && episode->parent()) {
        QString showName = episode->parent()->name;
        videoName = DownloadQueue::episodeFileName(*episode);
        displayName = showName + " : " + videoName;
        folder = workDir;

        origin = {provider ? provider->name() : QString(), episode->link, showName,
                  episode->number, episode->season};
    }
}

QSharedPointer<DownloadTask> DownloadTask::restore(const QString &savedName, const QString &savedFolder,
                                                   const QString &savedLink, const QString &savedLabel,
                                                   const Origin &savedOrigin) {
    if (savedName.isEmpty() || savedFolder.isEmpty()) return {};
    auto task = QSharedPointer<DownloadTask>::create(savedName, savedFolder, savedLink, savedLabel);
    task->origin = savedOrigin;
    ShowProvider *provider = ProviderList::byName(savedOrigin.provider);
    if (provider && !savedOrigin.episodeLink.isEmpty()) {
        auto show = QSharedPointer<PlaylistItem>::create(savedOrigin.showName, provider, savedOrigin.episodeLink);
        show->emplaceBack(savedOrigin.episodeSeason, savedOrigin.episodeNumber, savedOrigin.episodeLink,
                          savedName);
        task->m_episode  = show->at(0);
        task->m_provider = provider;
        task->link.clear();
    }
    task->m_status.store(Paused, std::memory_order_release);
    task->m_isPaused.store(true, std::memory_order_release);
    task->m_progressText = tr("Paused");
    return task;
}

QString DownloadTask::toolPath() {
    static const QString path = Settings::toolPath(QStringLiteral("N_m3u8DL-RE.exe"));
    return path;
}

QString DownloadTask::ffmpegPath() {
    static const QString path = Settings::toolPath(QStringLiteral("ffmpeg.exe"));
    return path;
}

bool DownloadTask::checkDependencies() {
    return QFile::exists(toolPath()) && QFile::exists(ffmpegPath());
}

// ':' separates the option fields, so a drive letter needs escaping.
static QString muxEscape(const QString &value) {
    QString escaped = value;
    escaped.replace(QLatin1Char(':'), QLatin1String("\\:"));
    return escaped;
}

QStringList DownloadTask::toolArguments() const {
    QStringList args {
        link,
        "--save-dir", folder,
        "--tmp-dir", tmpDir(),
        "--save-name", videoName,
        "--ffmpeg-binary-path", ffmpegPath(),
        "--del-after-done", "--no-date-info", "--no-log",
        "--auto-select", "--no-ansi-color"
    };
    // A drop, not a select: a select that matches nothing fails the run, and a plain media
    // playlist has no resolution to match. With every variant dropped, the tool keeps them all.
    if (maxHeight > 0)
        args << "--drop-video" << ("res=" + tallerThan(maxHeight));
    if (!maxSpeed.isEmpty())
        args << "--max-speed" << maxSpeed;
    for (auto it = headers.constBegin(); it != headers.constEnd(); ++it)
        args << "-H" << (it.key() + ": " + it.value());

    if (!subtitleFiles.isEmpty()) {
        // The bundled ffmpeg is not on PATH.
        args << "-M" << ("format=mkv:muxer=ffmpeg:bin_path="
                         + muxEscape(QDir::toNativeSeparators(ffmpegPath())));
        for (const SubtitleFile &sub : subtitleFiles) {
            QString option = "path=" + muxEscape(QDir::toNativeSeparators(sub.path));
            if (!sub.lang.isEmpty()) option += ":lang=" + sub.lang;
            if (!sub.name.isEmpty()) option += ":name=" + sub.name;
            args << "--mux-import" << option;
        }
    }
    return args;
}

QString DownloadTask::tallerThan(int height) {
    const QString digits = QString::number(height);
    QStringList taller{QStringLiteral("[0-9]{%1,}").arg(digits.size() + 1)};
    // Same leading digits, then a bigger one, then anything.
    for (qsizetype i = 0; i < digits.size(); ++i) {
        const int digit = digits[i].digitValue();
        if (digit < 9)
            taller << digits.left(i) + QStringLiteral("[%1-9][0-9]{%2}").arg(digit + 1).arg(digits.size() - i - 1);
    }
    return QStringLiteral("x(%1)$").arg(taller.join(QLatin1Char('|')));
}

QStringList DownloadTask::ffmpegArguments() const {
    // -headers applies to the input that follows it.
    QString headerBlock;
    for (auto it = headers.constBegin(); it != headers.constEnd(); ++it)
        headerBlock += it.key() + ": " + it.value() + "\r\n";

    QStringList args { "-y", "-hide_banner" };
    if (!headerBlock.isEmpty()) args << "-headers" << headerBlock;
    args << "-i" << link;
    if (!headerBlock.isEmpty()) args << "-headers" << headerBlock;
    args << "-i" << audioLink;
    for (const SubtitleFile &sub : subtitleFiles)
        args << "-i" << QDir::toNativeSeparators(sub.path);

    args << "-map" << "0:v:0" << "-map" << "1:a:0";
    // Whole input, not <n>:s:0: a mis-sniffed file aborts the mux.
    for (int i = 0; i < subtitleFiles.size(); ++i)
        args << "-map" << QString::number(2 + i);

    args << "-c" << "copy";
    if (!videoTitle.isEmpty()) args << "-metadata:s:v:0" << ("title=" + videoTitle);
    if (!audioTitle.isEmpty()) args << "-metadata:s:a:0" << ("title=" + audioTitle);
    for (int i = 0; i < subtitleFiles.size(); ++i) {
        const SubtitleFile &sub = subtitleFiles[i];
        const bool styled = sub.path.endsWith(".ass", Qt::CaseInsensitive)
                         || sub.path.endsWith(".ssa", Qt::CaseInsensitive);
        // WebVTT cannot be copied into Matroska.
        args << QStringLiteral("-c:s:%1").arg(i) << (styled ? "copy" : "srt");
        if (!sub.lang.isEmpty())
            args << QStringLiteral("-metadata:s:s:%1").arg(i) << ("language=" + sub.lang);
        if (!sub.name.isEmpty())
            args << QStringLiteral("-metadata:s:s:%1").arg(i) << ("title=" + sub.name);
    }
    if (subtitleFiles.isEmpty()) args << "-movflags" << "+faststart";   // mp4 only
    args << partPath();
    return args;
}

// Providers throw, and an escaping exception in a runnable is terminate.
QString DownloadTask::extractLink() {
    try {
        return extractLinkInner();
    } catch (AppException &e) {
        e.log();
    } catch (const std::exception &e) {
        logWarn() << "Downloader" << displayName << e.what();
    } catch (...) {
        logWarn() << "Downloader" << displayName << "unknown extraction error";
    }
    return {};
}

QString DownloadTask::extractLinkInner() {
    if (!m_provider || !m_episode || m_cancel.isCancelled())
        return {};

    setProgressText(tr("Extracting source..."));
    Client client(m_cancel);

    auto servers = m_provider->loadServers(&client, m_episode.data());
    if (m_cancel.isCancelled()) return {};

    auto res = ServerSelector::findWorkingServer(&client, m_provider, servers);
    if (!res.found() || m_cancel.isCancelled()) return {};

    // The tallest within the cap, else the shortest; without heights, the provider's first.
    const auto &videos = res.playInfo.videos;
    const int cap = maxHeight > 0 ? maxHeight : INT_MAX;
    qsizetype pick = -1, shortest = -1;
    for (qsizetype i = 0; i < videos.size(); ++i) {
        const int height = videos[i].height;
        if (height <= 0) continue;
        if (height <= cap && (pick < 0 || height > videos[pick].height)) pick = i;
        if (shortest < 0 || height < videos[shortest].height) shortest = i;
    }
    if (pick < 0) pick = qMax<qsizetype>(shortest, 0);
    // The refresh renews the first video's link only.
    if (pick == 0) m_refreshPrimaryVideoUrl = std::move(res.playInfo.refreshPrimaryVideoUrl);
    link = videos[pick].url.toString();
    videoTitle = videos[pick].title;
    if (!res.playInfo.audios.isEmpty()) {
        audioLink  = res.playInfo.audios.first().url.toString();
        audioTitle = res.playInfo.audios.first().title;
    }
    headers = res.playInfo.headers;

    if (subtitleFiles.isEmpty() && !res.playInfo.subtitles.isEmpty()) {
        setProgressText(tr("Fetching subtitles..."));
        fetchSubtitles(client, res.playInfo.subtitles);
    }
    setProgressText(tr("Source found"));

    m_episode = nullptr;
    m_provider = nullptr;
    return link;
}

void DownloadTask::refreshLink() {
    if (!m_refreshPrimaryVideoUrl) return;
    const QUrl refreshed = m_refreshPrimaryVideoUrl();
    if (!refreshed.isEmpty() && refreshed.isValid()) link = refreshed.toString();
}

namespace {

// Sniffed, never trusted from the url. Every HTML comment ends in "-->".
QString sniffSubtitleExtension(const QByteArray &data) {
    const QByteArray head = data.left(4096);
    if (head.startsWith("WEBVTT"))      return QStringLiteral(".vtt");
    if (head.contains("[Script Info]")) return QStringLiteral(".ass");
    static const QRegularExpression cue(
        QStringLiteral(R"(\d{1,2}:\d{2}:\d{2}[,.]\d{1,3}\s*-->\s*\d{1,2}:\d{2}:\d{2})"));
    if (cue.match(QString::fromUtf8(head)).hasMatch()) return QStringLiteral(".srt");
    return {};
}

bool isLangCode(const QString &text) {
    if (text.size() < 2 || text.size() > 3) return false;
    for (const QChar c : text)
        if (!c.isLetter() || c.unicode() > 0x7F) return false;
    static const QSet<QString> notCodes{"sub", "dub", "cc", "sdh", "srt", "vtt", "ass"};
    return !notCodes.contains(text.toLower());
}

// These hosts put the language in the filename: "ara-9.vtt", "..._sub_eng-0.vtt".
QString langFromFileName(const QUrl &url) {
    QString stem = QFileInfo(url.path()).completeBaseName();
    static const QRegularExpression trailingIndex(QStringLiteral(R"(-\d+$)"));
    stem.remove(trailingIndex);
    const QString tail = stem.section(QLatin1Char('_'), -1);
    return isLangCode(tail) ? tail.toLower() : QString();
}

QString subtitleLang(const Track &track) {
    if (const QString lang = track.lang.trimmed(); isLangCode(lang)) return lang.toLower();
    if (const QString fromName = langFromFileName(track.url); !fromName.isEmpty()) return fromName;

    static const QMap<QString, QString> byLabel{
        {"english", "eng"}, {"spanish", "spa"}, {"portuguese", "por"}, {"french", "fra"},
        {"german", "deu"},  {"italian", "ita"}, {"arabic", "ara"},     {"russian", "rus"},
        {"japanese", "jpn"},{"korean", "kor"},  {"chinese", "zho"},    {"indonesian", "ind"},
    };
    return byLabel.value(track.title.trimmed().toLower());
}

// Feeds --mux-import's name=, where ':' splits the option.
QString subtitleName(const Track &track) {
    QString name = track.title;
    name.remove(QRegularExpression(QStringLiteral("[\\x00-\\x1F:\"]")));
    name = name.simplified();
    return name.left(40);
}

}

void DownloadTask::fetchSubtitles(Client &client, const QList<Track> &tracks) {
    const QString dir = Settings::tempDir() + QStringLiteral("/downloadsubs");
    QDir().mkpath(dir);
    const QString key = QString::fromLatin1(
        QCryptographicHash::hash(basePath().toUtf8(), QCryptographicHash::Md5).toHex().left(12));

    int index = 0;
    for (const Track &track : tracks) {
        if (m_cancel.isCancelled()) return;
        if (track.lang == QLatin1String("danmaku")) continue;
        ++index;

        if (track.url.isLocalFile()) {
            const QString local = track.url.toLocalFile();
            if (QFileInfo(local).size() > 0)
                subtitleFiles.append({local, subtitleLang(track), subtitleName(track), false});
            continue;
        }
        const QString scheme = track.url.scheme();
        if (scheme != QLatin1String("http") && scheme != QLatin1String("https")) continue;

        const auto response = client.getBytes(track.url.toString(), headers);
        if (response.code != 200 || response.bytes.isEmpty()) {
            logWarn() << "Downloader" << "Subtitle fetch failed" << response.code << track.url.toString();
            continue;
        }
        const QString extension = sniffSubtitleExtension(response.bytes);
        if (extension.isEmpty()) {
            logWarn() << "Downloader" << "Unrecognised subtitle format" << track.url.toString();
            continue;
        }

        const QString file = QDir::cleanPath(
            QStringLiteral("%1/%2_%3%4").arg(dir, key).arg(index).arg(extension));
        QSaveFile out(file);
        if (!out.open(QIODevice::WriteOnly)) continue;
        out.write(response.bytes);
        if (!out.commit()) continue;
        subtitleFiles.append({file, subtitleLang(track), subtitleName(track), true});
    }
}

void DownloadTask::prepareSubtitles() {
    for (int i = subtitleFiles.size() - 1; i >= 0; --i)
        if (QFileInfo(subtitleFiles[i].path).size() <= 0) subtitleFiles.removeAt(i);
    m_useMkv.store(!subtitleFiles.isEmpty(), std::memory_order_release);
}

void DownloadTask::discardSubtitles() {
    for (const SubtitleFile &sub : subtitleFiles)
        if (sub.owned) QFile::remove(sub.path);
    subtitleFiles.clear();
}

void DownloadTask::setProgressValue(int value) {
    // Q_PROPERTY reads are main-thread.
    QMetaObject::invokeMethod(this, [this, value]() {
        if (m_progressValue == value) return;
        m_progressValue = value;

        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        // N_m3u8DL-RE resumes from existing segments.
        if (m_startTimeMs == 0) { m_startTimeMs = now; m_startProgress = value; }
        const double elapsed = (now - m_startTimeMs) / 1000.0;
        const int done = value - m_startProgress;
        m_rate = done > 0 && elapsed > 2.0 ? done / elapsed : 0;
        if (m_rate > 0 && value < 100)
            m_etaText = formatEta(int((100 - value) / m_rate));
        else
            m_etaText.clear();
        rebuildStats();
    }, Qt::AutoConnection);
}

void DownloadTask::setProgressText(const QString &text) {
    QMetaObject::invokeMethod(this, [this, text]() {
        if (m_progressText == text) return;
        m_progressText = text;
    }, Qt::AutoConnection);
}

void DownloadTask::setSpeed(const QString &speed) {
    QMetaObject::invokeMethod(this, [this, speed]() {
        if (m_speed == speed) return;
        m_speed = speed;
        rebuildStats();
    }, Qt::AutoConnection);
}

void DownloadTask::resetStats() {
    QMetaObject::invokeMethod(this, [this]() {
        m_startTimeMs = 0;
        m_rate = 0;
        m_speed.clear();
        m_etaText.clear();
        m_stats.clear();
    }, Qt::AutoConnection);
}

void DownloadTask::rebuildStats() {
    QStringList parts;
    if (!m_speed.isEmpty())   parts << m_speed;
    if (!m_etaText.isEmpty()) parts << ("ETA " + m_etaText);
    m_stats = parts.join(QStringLiteral("  •  "));
}

QString DownloadTask::formatEta(int seconds) {
    if (seconds < 0) seconds = 0;
    int h = seconds / 3600, m = (seconds % 3600) / 60, s = seconds % 60;
    if (h > 0)
        return QString("%1:%2:%3").arg(h).arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'));
    return QString("%1:%2").arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'));
}

static bool alreadyDownloaded(const QString &basePath) {
    return QFile::exists(basePath + QStringLiteral(".mp4"))
        || QFile::exists(basePath + QStringLiteral(".mkv"));
}

QString DownloadQueue::cleanFolderName(const QString &name) {
    static const QList<QPair<QChar, QChar>> replacements = {
        {':', u'꞉'}, {'"', '\''}, {'?', u'？'}, {'*', u'∗'},
        {'|', u'｜'}, {'<', u'≺'}, {'>', u'≻'}, {'/', u'∕'}, {'\\', u'⧵'}
    };
    QString result = name;
    for (const auto &[from, to] : replacements)
        result.replace(from, to);
    result.remove(QRegularExpression(QStringLiteral("[\\x00-\\x1F]")));
    // Windows rejects names ending in a space or dot.
    while (!result.isEmpty() && (result.endsWith(' ') || result.endsWith('.')))
        result.chop(1);
    result = result.trimmed();
    return result.isEmpty() ? QStringLiteral("download") : result;
}

QString DownloadQueue::showFolder(const QString &showTitle) {
    return Settings::instance().downloadDir() + "/" + cleanFolderName(showTitle);
}

QString DownloadQueue::episodeFileName(const PlaylistItem &episode) {
    return cleanFolderName(episode.displayName.trimmed().replace("\n", ". "));
}

DownloadQueue::EpisodeState DownloadQueue::episodeState(const QString &showTitle, const PlaylistItem &episode) const {
    const QString base = QDir::cleanPath(showFolder(showTitle) + "/" + episodeFileName(episode));
    if (m_ongoingPaths.contains(base)) return Queued;
    return alreadyDownloaded(base) ? Downloaded : NotDownloaded;
}

DownloadQueue::DownloadQueue(QObject *parent)
    : QAbstractListModel(parent)
    , m_maxDownloads(qBound(1, Settings::instance().get(Config::MaxDownloads), kMaxDownloadsLimit))
{
    m_threadPool.setMaxThreadCount(m_maxDownloads);
    for (auto signal : {&QAbstractItemModel::rowsInserted, &QAbstractItemModel::rowsRemoved})
        connect(this, signal, this, &DownloadQueue::overviewChanged);
    connect(this, &QAbstractItemModel::dataChanged, this, &DownloadQueue::overviewChanged);
}

// Nothing is removed: the saved queue comes back paused next session, and the segments a run
// left behind are what its resume picks up.
DownloadQueue::~DownloadQueue() {
    saveQueue();
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
}

QSqlDatabase &DownloadQueue::database() {
    static QSqlDatabase db = Store::open(QStringLiteral("downloads"));
    return db;
}

// Rewritten whole on every change to the queue: it is a handful of rows, and a crash keeps it.
void DownloadQueue::saveQueue() const {
    QSqlDatabase &db = database();
    if (!db.isOpen()) return;

    QList<QSharedPointer<DownloadTask>> tasks;
    {
        QMutexLocker locker(&m_mutex);
        tasks = m_tasks;
    }

    if (!db.transaction()) return;
    QSqlQuery query(db);
    query.exec(QStringLiteral("DELETE FROM downloads"));
    query.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO downloads (base_path, video_name, folder, link, display_name, "
        "provider, episode_link, show_name, episode_number, episode_season, queued_at, max_height) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?)"));
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    for (const auto &task : std::as_const(tasks)) {
        const DownloadTask::Origin &origin = task->origin;
        query.addBindValue(task->basePath());
        query.addBindValue(task->videoName);
        query.addBindValue(task->folder);
        // A provider task's link is the worker's to write, and is re-extracted on resume anyway.
        query.addBindValue(origin.provider.isEmpty() ? task->link : QString());
        query.addBindValue(task->displayName);
        query.addBindValue(origin.provider);
        query.addBindValue(origin.episodeLink);
        query.addBindValue(origin.showName);
        query.addBindValue(double(origin.episodeNumber));
        query.addBindValue(origin.episodeSeason);
        query.addBindValue(now);
        query.addBindValue(task->maxHeight);
        if (!query.exec()) logWarn() << "Downloader" << "could not store a queued task:"
                                     << query.lastError().text();
    }
    if (!db.commit()) {
        db.rollback();
        logWarn() << "Downloader" << "could not save the queue";
    }
}

// A queue that resumed itself would hammer providers with stale links.
void DownloadQueue::restoreQueue() {
    QSqlDatabase &db = database();
    if (!db.isOpen()) return;

    QSqlQuery query(db);
    if (!query.exec(QStringLiteral(
            "SELECT video_name, folder, link, display_name, provider, episode_link, show_name, "
            "episode_number, episode_season, max_height FROM downloads ORDER BY queued_at, rowid"))) {
        logWarn() << "Downloader" << "could not read the queue:" << query.lastError().text();
        return;
    }

    QList<QSharedPointer<DownloadTask>> restored;
    while (query.next()) {
        const DownloadTask::Origin origin{query.value(4).toString(), query.value(5).toString(),
                                          query.value(6).toString(), float(query.value(7).toDouble()),
                                          query.value(8).toInt()};
        auto task = DownloadTask::restore(query.value(0).toString(), query.value(1).toString(),
                                          query.value(2).toString(), query.value(3).toString(), origin);
        if (!task || alreadyDownloaded(task->basePath())) continue;   // finished between runs
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
    logInfo() << "Downloader" << "restored" << QString::number(restored.size()) << "paused task(s)";
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
    // As if every episode were the same size and the pace held.
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
        AppShell::instance().reportError(
            tr("N_m3u8DL-RE.exe and ffmpeg.exe must sit next to %1.exe.").arg(QStringLiteral(APP_NAME)), tr("Download"));
        return;
    }

    QString showLink;
    int episodeIndex = -1;
    if (ShowProvider *provider = ProviderList::forUrl(QUrl::fromUserInput(link.trimmed()), showLink, episodeIndex)) {
        const QString displayName = name.trimmed();
        const CancelToken cancel = m_resolveCancel;
        m_threadPool.start([this, provider, showLink, episodeIndex, displayName, cancel]() {
            ShowData show(displayName, showLink, {}, provider);
            try {
                Client client(cancel);
                provider->loadShow(&client, show);
            } catch (const std::exception &error) {
                if (!cancel.isCancelled())
                    AppShell::instance().reportError(QString::fromUtf8(error.what()), tr("Download"));
                return;
            }
            if (cancel.isCancelled()) return;

            const auto playlist = show.playlist();
            if (!playlist || playlist->isEmpty()) {
                AppShell::instance().reportError(tr("Nothing to download at that address."), tr("Download"));
                return;
            }
            const int named = episodeIndex >= 0 ? episodeIndex : playlist->currentIndex();
            const int first = named >= 0 ? named : 0;
            const int last  = named >= 0 ? named : playlist->count() - 1;
            if (!playlist->isValidIndex(first)) {
                AppShell::instance().reportError(tr("The requested part does not exist."), tr("Download"));
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
        logWarn() << "Downloader" << "Already exists or downloading" << task->path();
        return;
    }
    // Model signals stay outside the lock; the workers take it too.
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
        AppShell::instance().reportError(
            tr("N_m3u8DL-RE.exe and ffmpeg.exe must sit next to %1.exe.").arg(QStringLiteral(APP_NAME)), tr("Download"));
        return;
    }

    auto playlist = show.playlist();
    if (!playlist || !playlist->isValidIndex(startIndex)) return;

    if (endIndex < startIndex) std::swap(startIndex, endIndex);
    endIndex = qMin(endIndex, playlist->count() - 1);

    const QString workDir = showFolder(show.title);
    logInfo() << "Downloader" << show.title << "from" << startIndex << "to" << endIndex;

    for (int i = startIndex; i <= endIndex; ++i) {
        auto task = QSharedPointer<DownloadTask>::create(playlist->at(i), show.provider, workDir);
        task->maxHeight = Settings::instance().downloadMaxHeight();
        if (alreadyDownloaded(task->basePath()) || m_ongoingPaths.contains(task->basePath())) {
            logInfo() << "Downloader" << "Already exists or downloading" << task->path();
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
        // startTasks() already counted this slot.
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
                    task->setProgressText(tr("Could not find a source - press Retry"));
                    emitRowChanged(rowOf(task));
                }
                startTasks();
            }, Qt::QueuedConnection);
            return;
        }
    }
    task->refreshLink();

    // Sets the container, so it runs before the argument builders read it.
    task->prepareSubtitles();
    emitRowChanged(rowOf(task));

    const bool ffmpeg = task->usesFfmpeg();
    if (ffmpeg) QDir().mkpath(task->folder);   // N_m3u8DL-RE makes its save-dir; ffmpeg won't.
    else        QDir().mkpath(task->tmpDir());

    auto *process = new QProcess(nullptr);
    process->setProgram(task->program());
    process->setArguments(ffmpeg ? task->ffmpegArguments() : task->toolArguments());
    process->setProcessChannelMode(QProcess::MergedChannels);
    task->setProcess(process);
    process->start();

    static QRegularExpression percentRegex(R"((\d+\.\d+)%)");
    static QRegularExpression speedRegex(R"(([\d.]+\s*[KMGTP]?i?B(?:ps|/s)))");
    // ffmpeg has no %, so derive it from `time=` against `Duration:`.
    static QRegularExpression ffDurRegex(R"(Duration:\s*(\d+):(\d+):(\d+)\.(\d+))");
    static QRegularExpression ffTimeRegex(R"(time=\s*(\d+):(\d+):(\d+)\.(\d+))");
    auto ffSeconds = [](const QRegularExpressionMatch &m) {
        return m.captured(1).toInt() * 3600 + m.captured(2).toInt() * 60
             + m.captured(3).toInt() + m.captured(4).toInt() / 100.0;
    };
    double ffTotal = -1;
    bool reportedError = false;

    // A full pipe blocks the child.
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
                    task->setProgressText(tr("Downloading..."));
                }
            } else {
                auto match = percentRegex.match(line);
                if (match.hasMatch())
                    task->setProgressValue(static_cast<int>(match.captured(1).toFloat()));
                else if (line.contains("ERROR:") && !reportedError) {
                    // N_m3u8DL-RE emits an ERROR line per failed segment.
                    reportedError = true;
                    AppShell::instance().reportError(QString("%1\n%2").arg(task->displayName, line),
                                                     tr("Download error"));
                }
                auto sm = speedRegex.match(line);
                if (sm.hasMatch()) task->setSpeed(sm.captured(1).simplified());
                task->setProgressText(line);
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
            QFile::remove(task->path());
            if (!QFile::rename(task->partPath(), task->path())) {
                // The part file is the only copy now.
                logWarn() << "Downloader" << "Could not rename" << task->partPath() << "to" << task->path();
                task->keepPart = true;
                succeeded = false;
            }
        }
        if (!succeeded && !paused && !task->keepPart) QFile::remove(task->partPath());
    } else if (succeeded) {
        QDir(task->tmpDir()).removeRecursively();   // --del-after-done leaves the wrapper behind
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
            recordFinished(task->path(), task->displayName);
            removeTask(task, true);
        } else {
            task->setStatus(DownloadTask::Failed);
            task->setProgressText(tr("Failed - press Retry to resume"));
            emitRowChanged(rowOf(task));
        }
        startTasks();
    }, Qt::QueuedConnection);
}

void DownloadQueue::recordFinished(const QString &path, const QString &title) {
    QSqlQuery query(database());
    query.prepare(QStringLiteral("INSERT OR REPLACE INTO finished_downloads (path, title, finished_at) VALUES (?, ?, ?)"));
    query.addBindValue(path);
    query.addBindValue(title);
    query.addBindValue(QDateTime::currentSecsSinceEpoch());
    if (!query.exec()) logWarn() << "Downloader" << "Could not record a finished download:" << query.lastError().text();
    emit finishedChanged();
    emit downloadFinished(path, title);
}

QVariantList DownloadQueue::finished() const {
    QSqlQuery query(database());
    QVariantList list;
    if (!query.exec(QStringLiteral("SELECT path, title, finished_at FROM finished_downloads ORDER BY finished_at DESC")))
        return list;
    while (query.next()) {
        // A file moved or deleted since is no longer a download to play.
        const QString path = query.value(0).toString();
        if (QFileInfo::exists(path))
            list.append(QVariantMap{{"path", path}, {"title", query.value(1)}, {"at", query.value(2)}});
    }
    return list;
}

void DownloadQueue::forgetFinished(const QString &path) {
    QSqlQuery query(database());
    query.prepare(QStringLiteral("DELETE FROM finished_downloads WHERE path = ?"));
    query.addBindValue(path);
    if (query.exec()) emit finishedChanged();
}

void DownloadQueue::clearFinished() {
    QSqlQuery query(database());
    if (query.exec(QStringLiteral("DELETE FROM finished_downloads"))) emit finishedChanged();
}

void DownloadQueue::showInFolder(const QString &path) {
    // Explorer takes the file after /select, as one quoted argument, which QProcess would quote whole.
    QProcess explorer;
    explorer.setProgram(QStringLiteral("explorer.exe"));
    explorer.setNativeArguments(QStringLiteral("/select,\"%1\"").arg(QDir::toNativeSeparators(path)));
    explorer.startDetached();
}

void DownloadQueue::removeTask(const QSharedPointer<DownloadTask> &task, bool force) {
    // Main thread only.
    int idx;
    bool workerOwnsIt;
    {
        QMutexLocker locker(&m_mutex);
        idx = m_tasks.indexOf(task);
        if (idx == -1) return;
        // A queued task never reaches a worker.
        m_taskQueue.removeOne(task);
        workerOwnsIt = !force && task->status() == DownloadTask::Running;
        if (workerOwnsIt) {
            logInfo() << "Downloader" << "Cancelling" << task->displayName;
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
        // Resumed in the meantime means kept.
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
        // Killing the process keeps the temp segments, so a resume re-runs from them.
        task->setPaused(true);
        task->setProgressText(tr("Pausing..."));
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
            task->setStatus(DownloadTask::Running);
            task->resetStats();
            task->setProgressText(tr("Starting..."));
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
