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

QStringList DownloadTask::toolArguments() const {
    QStringList args {
        link,
        "--save-dir", outDir(),
        "--tmp-dir", tmpDir(),
        "--save-name", videoName,
        "--ffmpeg-binary-path", ffmpegPath(),
        "--del-after-done", "--no-date-info", "--no-log",
        "--auto-select", "--no-ansi-color",
        "--ui-language", "en-US"
    };
    if (maxHeight > 0)
        args << "--drop-video" << ("res=" + tallerThan(maxHeight));
    if (!maxSpeed.isEmpty())
        args << "--max-speed" << maxSpeed;
    for (auto it = headers.constBegin(); it != headers.constEnd(); ++it)
        args << "-H" << (it.key() + ": " + it.value());
    return args;
}

DownloadTask::ToolReport DownloadTask::readToolOutput(const QByteArray &output, const QString &host) {
    QString text = QString::fromUtf8(output);
    text.remove(QChar(0x2501));
    static const QRegularExpression head(QStringLiteral(R"((\d{2}:\d{2}:\d{2})\.\d{3} (INFO|WARN|ERROR|DEBUG)\s*:\s*)"));
    static const QRegularExpression bar(QStringLiteral(R"((?:Vid|Aud|Sub)\b.*?\d+/\d+\s+[\d.]+%.*$)"));
    static const QRegularExpression lineEnd(QStringLiteral("[\r\n]"));
    static const QStringList routine{QStringLiteral("Writing meta json"), QStringLiteral("Reading media info")};
    struct Message { QString time, level, text; };
    QList<Message> messages;
    QList<QRegularExpressionMatch> heads;
    for (auto it = head.globalMatch(text); it.hasNext();) heads << it.next();
    for (qsizetype i = 0; i < heads.size(); ++i) {
        const QString level = heads[i].captured(2);
        if (level == QLatin1String("INFO") || level == QLatin1String("DEBUG")) continue;
        const qsizetype end = i + 1 < heads.size() ? heads[i + 1].capturedStart() : text.size();
        QString body = text.mid(heads[i].capturedEnd(), end - heads[i].capturedEnd());
        body = body.left(body.indexOf(lineEnd)).remove(bar).trimmed();
        if (std::none_of(routine.cbegin(), routine.cend(), [&body](const QString &r) { return body.startsWith(r); }))
            messages.append({heads[i].captured(1), level, body});
    }

    ToolReport report;
    bool errors = false;
    int total = 0, downloaded = 0, refused = 0, dropped = 0, slow = 0;
    QString lastError;
    static const QRegularExpression count(QStringLiteral(R"(total:\s*(\d+),\s*downloaded:\s*(\d+))"));
    static const QRegularExpression status(QStringLiteral(R"(does not indicate success:\s*(\d{3}))"));
    for (const Message &message : std::as_const(messages)) {
        if (message.level == QLatin1String("ERROR")) {
            errors = true;
            if (const auto match = count.match(message.text); match.hasMatch()) {
                total = match.captured(1).toInt();
                downloaded = match.captured(2).toInt();
            } else if (message.text.compare(QLatin1String("Failed"), Qt::CaseInsensitive) != 0) {
                lastError = message.text;
            }
        } else if (const auto match = status.match(message.text); match.hasMatch()) {
            refused = match.captured(1).toInt();
        } else if (message.text.contains(QLatin1String("ended prematurely"))) {
            ++dropped;
        } else if (message.text.contains(QLatin1String("too slow")) || message.text.contains(QLatin1String("Timeout"))) {
            ++slow;
        }
    }
    report.failed = total > downloaded;
    if (report.failed) {
        const QString cause = refused == 403 ? tr("%1 refused them (HTTP 403)").arg(host)
                            : refused == 404 ? tr("%1 did not have them (HTTP 404)").arg(host)
                            : refused > 0    ? tr("%1 answered HTTP %2").arg(host).arg(refused)
                            : dropped > 0    ? tr("the connection to %1 dropped").arg(host)
                            : slow > 0       ? tr("%1 was too slow").arg(host)
                                             : QString();
        report.reason = cause.isEmpty() ? tr("%1 of %2 parts did not download.").arg(total - downloaded).arg(total)
                                        : tr("%1 of %2 parts did not download: %3.").arg(total - downloaded).arg(total).arg(cause);
    } else if (errors) {
        while (lastError.endsWith(QLatin1Char('.'))) lastError.chop(1);
        report.reason = lastError.isEmpty() ? tr("The download tool stopped.")
                                            : tr("The download tool stopped: %1.").arg(lastError);
    }

    QStringList lines;
    int repeats = 0;
    for (qsizetype i = 0; i < messages.size(); ++i) {
        if (i + 1 < messages.size() && messages[i + 1].level == messages[i].level && messages[i + 1].text == messages[i].text) {
            ++repeats;
            continue;
        }
        QString line = QStringLiteral("%1 %2: %3").arg(messages[i].time, messages[i].level, messages[i].text);
        if (repeats > 0) line += QStringLiteral(" (×%1)").arg(repeats + 1);
        lines << line;
        repeats = 0;
    }
    report.detail = lines.mid(qMax<qsizetype>(0, lines.size() - 15)).join(QLatin1Char('\n'));
    return report;
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

static QStringList subtitleOptions(const QList<DownloadTask::SubtitleFile> &subtitles, const QString &danmakuXml) {
    QStringList args;
    for (int i = 0; i < subtitles.size(); ++i) {
        const DownloadTask::SubtitleFile &sub = subtitles[i];
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
    return args;
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
    // whole input, not <n>:s:0: a mis-sniffed file aborts the mux
    for (int i = 0; i < subtitleFiles.size(); ++i)
        args << "-map" << QString::number(2 + i);

    args << "-c" << "copy";
    if (!videoTitle.isEmpty()) args << "-metadata:s:v:0" << ("title=" + videoTitle);
    if (!audioTitle.isEmpty()) args << "-metadata:s:a:0" << ("title=" + audioTitle);
    args << subtitleOptions(subtitleFiles, danmakuXml);
    if (subtitleFiles.isEmpty()) args << "-movflags" << "+faststart";
    args << partPath();
    return args;
}

// providers throw, and an exception escaping a runnable is terminate
QString DownloadTask::extractLink() {
    extractError.clear();
    try {
        return extractLinkInner();
    } catch (AppException &e) {
        e.log("Downloads");
        extractError = e.message();
    } catch (const std::exception &e) {
        logWarn() << "Downloads" << displayName << e.what();
        extractError = QString::fromUtf8(e.what());
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

DownloadTask::ToolFiles DownloadTask::toolFiles() const {
    static const QStringList videoTypes{"mp4", "mkv", "ts", "m4v", "mov", "webm", "flv"};
    static const QStringList audioTypes{"m4a", "aac", "mp3", "ac3", "eac3", "ec3", "opus", "ogg", "flac", "mka"};
    static const QStringList subtitleTypes{"srt", "vtt", "ass", "ssa"};
    ToolFiles files;
    QString largest;
    qint64 size = 0;
    for (const QFileInfo &file : QDir(outDir()).entryInfoList(QDir::Files)) {
        const QString type = file.suffix().toLower();
        if (videoTypes.contains(type) && file.size() > size) {
            if (!largest.isEmpty()) files.tracks << largest;
            size = file.size();
            largest = file.absoluteFilePath();
        } else if (videoTypes.contains(type) || audioTypes.contains(type)) {
            if (file.size() > 0) files.tracks << file.absoluteFilePath();
        } else if (subtitleTypes.contains(type)) {
            if (file.size() > 0) files.subtitles << file.absoluteFilePath();
        }
    }
    files.video = largest;
    return files;
}

// the tool's mkv mux fails on timestampless frames yet exits 0
bool DownloadTask::place() {
    const ToolFiles made = toolFiles();
    if (made.video.isEmpty()) return false;

    QList<SubtitleFile> subtitles = subtitleFiles;
    for (const QString &file : made.subtitles) {
        const QString base = QFileInfo(file).completeBaseName();
        const QString tag = base.contains(QLatin1Char('.')) ? base.section(QLatin1Char('.'), -1) : QString();
        static const QRegularExpression language(QStringLiteral("^[a-z]{2,3}(-[a-z0-9]{2,4})?$"),
                                                 QRegularExpression::CaseInsensitiveOption);
        const bool isLanguage = language.match(tag).hasMatch();
        subtitles.append({file, isLanguage ? tag : QString(), isLanguage ? QString() : tag, false});
    }

    QString why;
    if (!subtitles.isEmpty() || !made.tracks.isEmpty()) {
        m_useMkv.store(!subtitles.isEmpty(), std::memory_order_release);
        setProgressText(subtitles.isEmpty() ? tr("Saving…") : tr("Adding subtitles…"));
        QStringList args{"-y", "-hide_banner", "-loglevel", "error", "-i", QDir::toNativeSeparators(made.video)};
        for (const QString &track : made.tracks) args << "-i" << QDir::toNativeSeparators(track);
        for (const SubtitleFile &sub : std::as_const(subtitles)) args << "-i" << QDir::toNativeSeparators(sub.path);
        args << "-map" << "0:v" << "-map" << "0:a?";
        for (int i = 0; i < made.tracks.size(); ++i)
            args << "-map" << QStringLiteral("%1:a?").arg(1 + i);
        for (int i = 0; i < subtitles.size(); ++i)
            args << "-map" << QString::number(1 + made.tracks.size() + i);
        args << "-c" << "copy" << subtitleOptions(subtitles, danmakuXml) << QDir::toNativeSeparators(partPath());

        QProcess mux;
        mux.start(ffmpegPath(), args);
        if (!mux.waitForFinished(600000)) {
            mux.kill();
            why = QStringLiteral("timed out");
        } else if (mux.exitCode() != 0) {
            why = QString::fromUtf8(mux.readAllStandardError()).trimmed();
        } else if (Platform::replaceFile(partPath(), path(), &why)) {
            QDir(outDir()).removeRecursively();
            return true;
        }
        logWarn() << "Downloads" << displayName << "kept its video without the other tracks and subtitles:" << why;
        QFile::remove(partPath());
        m_useMkv.store(false, std::memory_order_release);
    }
    if (!Platform::replaceFile(made.video, path(), &why)) {
        logWarn() << "Downloads" << displayName << "could not move" << made.video << "to" << path() << why;
        return false;
    }
    for (const QString &file : made.tracks + made.subtitles) {
        if (!Platform::replaceFile(file, folder + QLatin1Char('/') + QFileInfo(file).fileName(), &why))
            logWarn() << "Downloads" << displayName << "left" << file << "behind:" << why;
    }
    QDir(outDir()).removeRecursively();
    return true;
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

void DownloadTask::setDetail(const QString &text) {
    QMetaObject::invokeMethod(this, [this, text]() { m_detail = text; }, Qt::AutoConnection);
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
