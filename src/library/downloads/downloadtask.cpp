#include "library/downloads/downloadtask.h"
#include "library/downloads/downloadqueue.h"
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QDateTime>
#include <QRegularExpression>
#include "providers/showdata.h"
#include "providers/provider.h"
#include "providers/providerlist.h"
#include "app/logger.h"
#include "media/player/danmaku.h"
#include "media/playlist/servers.h"
#include "app/settings.h"
#include "system/platform.h"
#include "app/exception.h"
#include "net/client.h"
#include <QCryptographicHash>
#include <QSaveFile>
#include <climits>

DownloadTask::DownloadTask(const QString &videoName, const QString &folder, const QString &link,
                           const QString &displayName, const QMap<QString, QString> &headers)
    : videoName(videoName), folder(folder), link(link), headers(headers), displayName(displayName)
{}

DownloadTask::DownloadTask(QSharedPointer<PlaylistItem> episode, Provider *provider, const QString &workDir)
    : m_episode(episode), m_provider(provider)
{
    if (episode && episode->parent()) {
        QString showName = episode->parent()->name;
        videoName = DownloadQueue::episodeFileName(*episode);
        displayName = showName + " : " + videoName;
        folder = workDir;

        origin = {provider ? provider->name() : QString(), episode->link, showName,
                  episode->number, episode->season, episode->parent()->link, QString(), episode->row()};
    }
}

QSharedPointer<DownloadTask> DownloadTask::restore(const QString &savedName, const QString &savedFolder,
                                                   const QString &savedLink, const QString &savedLabel,
                                                   const Origin &savedOrigin) {
    if (savedName.isEmpty() || savedFolder.isEmpty()) return {};
    auto task = QSharedPointer<DownloadTask>::create(savedName, savedFolder, savedLink, savedLabel);
    task->origin = savedOrigin;
    Provider *provider = ProviderList::byName(savedOrigin.provider);
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
    // A drop, not a select: a select matching nothing fails the run; with all dropped the tool keeps all.
    if (maxHeight > 0)
        args << "--drop-video" << ("res=" + tallerThan(maxHeight));
    if (!maxSpeed.isEmpty())
        args << "--max-speed" << maxSpeed;
    for (auto it = headers.constBegin(); it != headers.constEnd(); ++it)
        args << "-H" << (it.key() + ": " + it.value());

    if (useMkv()) {
        args << "-M" << ("format=mkv:muxer=ffmpeg:bin_path="
                         + muxEscape(QDir::toNativeSeparators(ffmpegPath())));
        for (const SubtitleFile &sub : subtitleFiles) {
            // The tool turns imports into WebVTT, flattening danmaku's moves; addDanmaku() adds them after.
            if (sub.danmaku) continue;
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
    for (qsizetype i = 0; i < digits.size(); ++i) {
        const int digit = digits[i].digitValue();
        if (digit < 9)
            taller << digits.left(i) + QStringLiteral("[%1-9][0-9]{%2}").arg(digit + 1).arg(digits.size() - i - 1);
    }
    return QStringLiteral("x(%1)$").arg(taller.join(QLatin1Char('|')));
}

QStringList DownloadTask::ffmpegArguments() const {
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
        args << QStringLiteral("-c:s:%1").arg(i) << (styled ? "copy" : "srt");
        if (!sub.lang.isEmpty())
            args << QStringLiteral("-metadata:s:s:%1").arg(i) << ("language=" + sub.lang);
        if (!sub.name.isEmpty())
            args << QStringLiteral("-metadata:s:s:%1").arg(i) << ("title=" + sub.name);
    }
    if (!danmakuXml.isEmpty())
        args << "-attach" << QDir::toNativeSeparators(danmakuXml)
             << "-metadata:s:t:0" << "mimetype=application/xml" << "-metadata:s:t:0" << "filename=danmaku.xml";
    if (subtitleFiles.isEmpty()) args << "-movflags" << "+faststart";
    args << partPath();
    return args;
}

// Providers throw, and an escaping exception in a runnable is terminate.
QString DownloadTask::extractLink() {
    try {
        return extractLinkInner();
    } catch (AppException &e) {
        e.log("Downloads");
    } catch (const std::exception &e) {
        logWarn() << "Downloads" << displayName << e.what();
    } catch (...) {
        logWarn() << "Downloads" << displayName << "unknown extraction error";
    }
    return {};
}

QString DownloadTask::extractLinkInner() {
    if (!m_provider || !m_episode || m_cancel.isCancelled())
        return {};

    setProgressText(tr("Extracting source…"));
    Client client(m_cancel);

    auto servers = m_provider->loadServers(&client, m_episode.data());
    if (m_cancel.isCancelled()) return {};

    auto res = ServerSelector::findWorkingServer(&client, m_provider, servers);
    if (!res.found() || m_cancel.isCancelled()) return {};

    const auto &videos = res.source.videos;
    const int cap = maxHeight > 0 ? maxHeight : INT_MAX;
    qsizetype pick = -1, shortest = -1;
    for (qsizetype i = 0; i < videos.size(); ++i) {
        const int height = videos[i].height;
        if (height <= 0) continue;
        if (height <= cap && (pick < 0 || height > videos[pick].height)) pick = i;
        if (shortest < 0 || height < videos[shortest].height) shortest = i;
    }
    if (pick < 0) pick = qMax<qsizetype>(shortest, 0);
    if (pick == 0) m_refreshPrimaryVideoUrl = std::move(res.source.refreshPrimaryVideoUrl);
    link = videos[pick].url.toString();
    videoTitle = videos[pick].title;
    if (!res.source.audios.isEmpty()) {
        audioLink  = res.source.audios.first().url.toString();
        audioTitle = res.source.audios.first().title;
    }
    headers = res.source.headers;

    if (subtitleFiles.isEmpty() && !res.source.subtitles.isEmpty()) {
        setProgressText(tr("Fetching subtitles…"));
        fetchSubtitles(client, res.source.subtitles);
    }
    fetchDanmaku(res.source);
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
            logWarn() << "Downloads" << "Subtitle fetch failed" << response.code << track.url.toString();
            continue;
        }
        const QString extension = sniffSubtitleExtension(response.bytes);
        if (extension.isEmpty()) {
            logWarn() << "Downloads" << "Unrecognised subtitle format" << track.url.toString();
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

void DownloadTask::fetchDanmaku(const Source &source) {
    if (!danmaku || !source.danmakuSource || source.danmakuKey.isEmpty() || m_cancel.isCancelled()) return;
    for (const SubtitleFile &sub : std::as_const(subtitleFiles))
        if (sub.danmaku) return;
    setProgressText(tr("Fetching danmaku…"));
    QFuture<QList<DanmakuComment>> comments = source.danmakuSource();
    comments.waitForFinished();
    if (m_cancel.isCancelled() || comments.resultCount() == 0) return;
    DanmakuOptions options = *danmaku;
    options.enabled = true;
    const QString dir = Settings::tempDir() + QStringLiteral("/downloadsubs");
    const QString file = DanmakuAss::writeFile(comments.result(), source.danmakuKey, options, dir);
    if (file.isEmpty()) {
        logInfo() << "Downloads" << displayName << "had no danmaku to save";
        return;
    }
    subtitleFiles.append({file, QString(), QStringLiteral("Danmaku"), true, true});
    QSaveFile xml(dir + QStringLiteral("/danmaku_%1.xml").arg(source.danmakuKey));
    if (xml.open(QIODevice::WriteOnly)) {
        xml.write(DanmakuAss::toXml(comments.result()));
        if (xml.commit()) danmakuXml = xml.fileName();
    }
}

void DownloadTask::addDanmaku() {
    const auto track = std::find_if(subtitleFiles.cbegin(), subtitleFiles.cend(),
                                      [](const SubtitleFile &sub) { return sub.danmaku; });
    if (track == subtitleFiles.cend()) return;
    setProgressText(tr("Adding danmaku…"));
    const QString video = path();
    QProcess probe;
    probe.start(ffmpegPath(), {"-hide_banner", "-i", QDir::toNativeSeparators(video)});
    probe.waitForFinished(30000);
    static const QRegularExpression streamRe(QStringLiteral(R"(Stream #0:\d+)"));
    const int streams = int(QString::fromUtf8(probe.readAllStandardError()).count(streamRe));
    const QString muxed = basePath() + QStringLiteral(".danmaku.mkv");
    QProcess mux;
    QStringList args{"-y", "-hide_banner", "-loglevel", "error",
                     "-i", QDir::toNativeSeparators(video), "-i", QDir::toNativeSeparators(track->path),
                     "-map", "0", "-map", "1", "-c", "copy",
                     QStringLiteral("-metadata:s:%1").arg(streams), "title=Danmaku"};
    if (!danmakuXml.isEmpty())
        args << "-attach" << QDir::toNativeSeparators(danmakuXml)
             << "-metadata:s:t:0" << "mimetype=application/xml" << "-metadata:s:t:0" << "filename=danmaku.xml";
    mux.start(ffmpegPath(), args << QDir::toNativeSeparators(muxed));
    if (!mux.waitForFinished(600000) || mux.exitCode() != 0 || streams == 0) {
        logWarn() << "Downloads" << displayName << "kept its video without the danmaku:"
                  << QString::fromUtf8(mux.readAllStandardError()).trimmed();
        QFile::remove(muxed);
        return;
    }
    QString why;
    if (!Platform::replaceFile(muxed, video, &why)) {
        logWarn() << "Downloads" << displayName << "kept its video without the danmaku, it could not be replaced:" << why;
        QFile::remove(muxed);
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
    if (!danmakuXml.isEmpty()) QFile::remove(std::exchange(danmakuXml, {}));
}

void DownloadTask::setProgressValue(int value) {
    QMetaObject::invokeMethod(this, [this, value]() {
        if (m_progressValue == value) return;
        m_progressValue = value;

        const qint64 now = QDateTime::currentMSecsSinceEpoch();
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
    if (!m_etaText.isEmpty()) parts << tr("%1 left").arg(m_etaText);
    m_stats = parts.join(QStringLiteral("  ·  "));
}

QString DownloadTask::formatEta(int seconds) {
    if (seconds < 0) seconds = 0;
    int h = seconds / 3600, m = (seconds % 3600) / 60, s = seconds % 60;
    if (h > 0)
        return QString("%1:%2:%3").arg(h).arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'));
    return QString("%1:%2").arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'));
}
