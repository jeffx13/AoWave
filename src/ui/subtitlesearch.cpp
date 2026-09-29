#include "ui/subtitlesearch.h"
#include "core/async.h"
#include "core/exception.h"
#include "core/logger.h"
#include "core/settings.h"
#include "media/mpvplayer.h"
#include "net/client.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QSaveFile>
#include <QRegularExpression>
#include <QCryptographicHash>
#include <QThread>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <utility>

namespace {

// "s01e05", "season 1 episode 5", "1x05", a bare "s01", and a bare "e05" or "episode 5".
const QRegularExpression &seasonEpisodeRe() {
    static const QRegularExpression re(
        QStringLiteral(R"((?:^|\s)(?:s(?:eason)?\s*(\d{1,3})(?:\s*e(?:pisode)?\s*(\d{1,4}))?)"
                       R"(|(?:(\d{1,3})x(\d{1,4}))|e(?:p(?:isode)?)?\s*(\d{1,4}))(?=\s|$))"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

struct SeasonEpisode { int season = 0; int episode = 0; qsizetype at = -1, length = 0; };

SeasonEpisode seasonEpisodeIn(const QString &text) {
    const auto match = seasonEpisodeRe().match(text);
    if (!match.hasMatch()) return {};
    const QString season  = match.captured(1).isEmpty() ? match.captured(3) : match.captured(1);
    const QString episode = !match.captured(2).isEmpty() ? match.captured(2)
                          : !match.captured(4).isEmpty() ? match.captured(4) : match.captured(5);
    return {season.toInt(), episode.toInt(), match.capturedStart(), match.capturedLength()};
}

// SubDL's per-file season and episode are often 0 in a season pack.
SeasonEpisode seasonEpisodeFromName(const QString &name) {
    return seasonEpisodeIn(QString(name).replace(QRegularExpression(QStringLiteral("[._\\-]+")), QStringLiteral(" ")));
}

// Letters and digits only, for finding an episode's title in a file name.
QString squashed(const QString &text) {
    QString out;
    for (const QChar c : text.toCaseFolded())
        if (c.isLetterOrNumber()) out += c;
    return out;
}

bool isBusy(const QString &error) {
    return error.trimmed().compare(QLatin1String("service_busy"), Qt::CaseInsensitive) == 0;
}

bool isNoSubtitleMatch(const QString &error) {
    const QString message = error.trimmed().toLower();
    return message == QLatin1String("can't find movie or tv")
        || message == QLatin1String("no results found")
        || message == QLatin1String("no subtitles found")
        || message == QLatin1String("no matching subtitles found")
        || message == QLatin1String("movie or tv not found");
}

// Providers throw, and an escaping exception in a worker is terminate.
template <typename F>
auto guarded(const CancelToken &cancel, F &&fn) -> decltype(fn()) {
    try {
        return fn();
    } catch (const AppException &e) {
        if (!cancel.isCancelled()) e.report();
        e.log();
    } catch (const std::exception &e) {
        logWarn() << "Subtitles" << e.what();
    }
    return {};
}

QStringList releaseTags(const QString &text) {
    constexpr int kMaxTags = 4;
    struct Pattern { QRegularExpression re; const char *suffix; };
    static const Pattern patterns[] = {
        {QRegularExpression(R"(\b(2160p|1440p|1080p|720p|480p)\b)", QRegularExpression::CaseInsensitiveOption), ""},
        {QRegularExpression(R"(\b(BluRay|BDRip|BRRip|WEB-?DL|WEBRip|HDTV|DVDRip|REMUX)\b)", QRegularExpression::CaseInsensitiveOption), ""},
        {QRegularExpression(R"(\b(x265|x264|HEVC|AVC)\b)", QRegularExpression::CaseInsensitiveOption), ""},
        {QRegularExpression(R"(\b(\d{2}(?:\.\d{1,3})?)\s*FPS\b)", QRegularExpression::CaseInsensitiveOption), " fps"},
    };

    QStringList tags;
    for (const Pattern &p : patterns) {
        auto it = p.re.globalMatch(text);
        while (it.hasNext() && tags.size() < kMaxTags) {
            const QString tag = it.next().captured(1) + QLatin1String(p.suffix);
            if (!tags.contains(tag, Qt::CaseInsensitive)) tags.append(tag);
        }
    }
    return tags;
}

QString prettyName(const QString &name) {
    static const QRegularExpression extension(R"(\.(srt|ass|ssa|vtt|sub|txt)$)", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression langSuffix(R"([.\s_-]+(en|eng|english)$)", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression separators(R"([._]+)");
    QString s = name;
    s.remove(extension);
    s.remove(langSuffix);
    s.replace(separators, QStringLiteral(" "));
    return s.simplified();
}

}

SubtitleSearch::SubtitleSearch(QObject *parent) : QAbstractListModel(parent) {
    connect(&m_searchWatcher, &QFutureWatcher<QList<SubDl::Result>>::finished, this, [this]() {
        if (!m_searchCancel.isCancelled()) {
            setResults(m_searchWatcher.result());
            m_searchedQuery = m_query;
        }
        emit isLoadingChanged();
    });
}

MpvPlayer *SubtitleSearch::mpv() {
    auto *player = MpvPlayer::instance();
    if (player && !m_mpvConnected) {
        m_mpvConnected = true;
        for (auto signal : {&MpvPlayer::primarySubIdChanged, &MpvPlayer::secondarySubIdChanged,
                            &MpvPlayer::externalSubsChanged})
            connect(player, signal, this, &SubtitleSearch::refreshSlots);
    }
    return player;
}

SubtitleSearch::~SubtitleSearch() {
    m_searchCancel.cancel();
    m_fetchCancel.cancel();
    // The workers capture `this`.
    waitFor(m_searchWatcher, "SubtitleSearch search");
    waitFor(m_fetchWatcher,  "SubtitleSearch fetch");
}

int SubtitleSearch::rowForFileId(const QString &fileId) const {
    for (int i = 0; i < m_rows.size(); ++i)
        if (m_rows[i].result.fileId == fileId) return i;
    return -1;
}

int SubtitleSearch::slotFor(const QString &localPath) {
    auto *player = mpv();
    if (!player || localPath.isEmpty()) return 0;
    const qint64 id = player->externalSubId(localPath);
    if (id == 0) return 0;
    return id == player->primarySubId()   ? 1
         : id == player->secondarySubId() ? 2 : 0;
}

void SubtitleSearch::refreshSlots() {
    bool changed = false;
    for (Row &row : m_rows) {
        const int slot = slotFor(row.localPath);
        if (slot != row.slot) { row.slot = slot; changed = true; }
    }
    if (changed)
        emit dataChanged(index(0), index(m_rows.size() - 1), {SlotRole});
}

int SubtitleSearch::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : m_rows.size();
}

QVariant SubtitleSearch::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= m_rows.size()) return {};
    const Row &row = m_rows[index.row()];
    switch (role) {
    case DisplayNameRole: return row.displayName;
    case ReleaseRole:     return row.result.releaseName;
    case LanguageRole:    return row.result.language;
    case AuthorRole:      return row.result.author;
    case EpisodeRole:     return row.result.episode > 0 ? QString("E%1").arg(row.result.episode) : QString();
    case HiRole:          return row.result.hearingImpaired;
    case TagsRole:        return row.tags;
    case SlotRole:        return row.slot;
    case FetchingRole:    return row.fetching;
    default:              return {};
    }
}

QHash<int, QByteArray> SubtitleSearch::roleNames() const {
    return {{DisplayNameRole, "displayName"}, {ReleaseRole,   "release"},
            {LanguageRole,    "language"},    {AuthorRole,    "author"},
            {EpisodeRole,     "episodeLabel"},{HiRole,        "hearingImpaired"},
            {TagsRole,        "tags"},        {SlotRole,      "slot"},
            {FetchingRole,    "fetching"}};
}

void SubtitleSearch::setResults(const QList<SubDl::Result> &results) {
    QList<Row> rows;
    rows.reserve(results.size());
    for (const SubDl::Result &r : results) {
        Row row{r, prettyName(r.name), releaseTags(r.releaseName + QChar(' ') + r.name), {}, 0, false};
        // An earlier result may still hold a slot.
        if (const QString cached = SubDl::cachePath(r.fileId); QFileInfo(cached).size() > 0) {
            row.localPath = cached;
            row.slot = slotFor(cached);
        }
        rows.append(std::move(row));
    }

    beginResetModel();
    m_rows = std::move(rows);
    endResetModel();
    emit countChanged();
}

void SubtitleSearch::search(const QString &query, const QVariantMap &hintMap) {
    const QString trimmed = query.trimmed();
    const SubDl::Hints hints{hintMap.value("show").toString(), hintMap.value("tmdb").toString(),
                             hintMap.value("title").toString(), hintMap.value("season").toInt(),
                             hintMap.value("episode").toInt()};
    if (trimmed.isEmpty() || m_searchWatcher.isRunning()) return;

    m_query = trimmed;
    emit queryChanged();

    // GUI thread: the worker must not touch QSettings.
    const QString apiKey = Settings::instance().subdlApiKey();
    const QString languages = Settings::instance().subdlLanguages();
    if (apiKey.isEmpty()) {
        AppShell::instance().reportError(
            tr("Subtitle search needs a SubDL API key. Create one at subdl.com and paste it "
               "under Settings, Integrations."), tr("Subtitles"));
        return;
    }

    m_searchCancel = CancelToken{};
    m_searchWatcher.setFuture(QtConcurrent::run([trimmed, apiKey, languages, hints, cancel = m_searchCancel] {
        Client client(cancel);
        return guarded(cancel, [&] { return SubDl::search(&client, trimmed, apiKey, languages, hints); });
    }));
    emit isLoadingChanged();
}

void SubtitleSearch::use(int row, bool secondary) {
    if (row < 0 || row >= m_rows.size()) return;
    auto *player = mpv();
    if (!player) return;

    const SubDl::Result result = m_rows[row].result;
    if (!m_rows[row].localPath.isEmpty()) {
        player->useExternalSubtitle(m_rows[row].localPath, result.name, result.language, secondary);
        return;
    }
    if (m_fetchWatcher.isRunning()) return;

    m_rows[row].fetching = true;
    emit dataChanged(index(row), index(row), {FetchingRole});

    m_fetchCancel = CancelToken{};
    m_fetchWatcher.setFuture(QtConcurrent::run([this, result, secondary, cancel = m_fetchCancel] {
        Client client(cancel);
        const QString path = guarded(cancel, [&] { return SubDl::fetch(&client, result); });
        QMetaObject::invokeMethod(this, [this, result, path, secondary] {
            const int row = rowForFileId(result.fileId);
            if (row < 0) return;
            m_rows[row].fetching = false;
            m_rows[row].localPath = path;
            emit dataChanged(index(row), index(row), {FetchingRole, SlotRole});
            if (path.isEmpty()) return;
            if (auto *player = mpv())
                player->useExternalSubtitle(path, result.name, result.language, secondary);
        }, Qt::QueuedConnection);
    }));
}

void SubtitleSearch::searchIfNew(const QString &query, const QVariantMap &hints) {
    if (query.trimmed() != m_searchedQuery) search(query, hints);
}

void SubtitleSearch::cancel() {
    m_searchCancel.cancel();
    m_fetchCancel.cancel();
}

namespace {

constexpr const char *kApi = "https://api.subdl.com/api/v1/subtitles";
constexpr const char *kFiles = "https://dl.subdl.com";

SubDl::Result fileToResult(const QJsonObject &file, const QJsonObject &release) {
    SubDl::Result r;
    r.fileId          = file["file_n_id"].toVariant().toString();
    r.name            = file["name"].toString();
    r.releaseName     = release["release_name"].toString();
    r.language        = file["language"].toString();
    r.author          = release["author"].toString();
    r.season          = file["season"].toInt();
    r.episode         = file["episode"].toInt();
    r.size            = file["size"].toInteger();
    r.hearingImpaired = file["hi"].toBool();
    r.url             = QUrl(QLatin1String(kFiles)).resolved(QUrl(file["url"].toString()));
    return r;
}

}

QVariantList SubDl::languages() {
    static const QVariantList list = [] {
        static constexpr std::pair<const char *, const char *> table[] = {
            {"AR", "Arabic"},      {"BN", "Bengali"},     {"BG", "Bulgarian"},  {"ZH", "Chinese"},
            {"ZH_BG", "Chinese (Big5)"}, {"HR", "Croatian"}, {"CS", "Czech"},    {"DA", "Danish"},
            {"NL", "Dutch"},       {"EN", "English"},     {"ET", "Estonian"},   {"FI", "Finnish"},
            {"FR", "French"},      {"DE", "German"},      {"EL", "Greek"},      {"HE", "Hebrew"},
            {"HI", "Hindi"},       {"HU", "Hungarian"},   {"ID", "Indonesian"}, {"IT", "Italian"},
            {"JA", "Japanese"},    {"KO", "Korean"},      {"LV", "Latvian"},    {"LT", "Lithuanian"},
            {"MS", "Malay"},       {"NO", "Norwegian"},   {"FA", "Persian"},    {"PL", "Polish"},
            {"PT", "Portuguese"},  {"BR_PT", "Portuguese (Brazil)"}, {"RO", "Romanian"}, {"RU", "Russian"},
            {"SR", "Serbian"},     {"SK", "Slovak"},      {"SL", "Slovenian"},  {"ES", "Spanish"},
            {"SV", "Swedish"},     {"TL", "Tagalog"},     {"TA", "Tamil"},      {"TE", "Telugu"},
            {"TH", "Thai"},        {"TR", "Turkish"},     {"UK", "Ukrainian"},  {"UR", "Urdu"},
            {"VI", "Vietnamese"}};
        QVariantList rows;
        for (const auto &[code, name] : table)
            rows.append(QVariantMap{{QStringLiteral("code"), QLatin1String(code)},
                                    {QStringLiteral("name"), QLatin1String(name)}});
        return rows;
    }();
    return list;
}

namespace {

QList<SubDl::Result> searchPage(Client *client, QMap<QString, QString> params, int page) {
    if (page > 1) params.insert(QStringLiteral("page"), QString::number(page));
    QJsonObject json;
    // SubDL turns searches away with "service_busy" at busy moments; a short wait usually does.
    for (int attempt = 0;; ++attempt) {
        const auto response = client->get(kApi, {}, params);
        json = response.toJsonObject();
        // A timeout (code -1) is the same overload, seen from here.
        const bool busy = response.code <= 0 || response.code == 429 || response.code == 503
                          || isBusy(json["error"].toString());
        if (!busy) {
            if (response.code != 200)
                throw AppException(QObject::tr("SubDL returned %1.").arg(response.code), QObject::tr("Subtitles"));
            break;
        }
        if (attempt == 2 || client->isCancelled())
            throw AppException(response.code <= 0 ? QObject::tr("SubDL did not answer. Try again in a minute.")
                                                  : QObject::tr("SubDL is busy right now. Try again in a minute."),
                               QObject::tr("Subtitles"));
        // It says how long; a few seconds at most is worth waiting here.
        const int wait = json["retryAfterSeconds"].toInt();
        QThread::msleep(wait > 0 ? qMin(wait, 8) * 1000 : 1500 * (attempt + 1));
    }

    if (json.isEmpty()) throw AppException(QObject::tr("SubDL returned an invalid response."), QObject::tr("Subtitles"));
    if (!json["status"].toBool()) {
        const QString error = json["error"].toString();
        if (isNoSubtitleMatch(error)) return {};
        throw AppException(error.isEmpty() ? QObject::tr("SubDL rejected the search.") : error, QObject::tr("Subtitles"));
    }

    QList<SubDl::Result> results;
    for (const QJsonValue &value : json["subtitles"].toArray()) {
        const QJsonObject release = value.toObject();
        for (const QJsonValue &file : release["unpack_files"].toArray()) {
            SubDl::Result r = fileToResult(file.toObject(), release);
            if (!r.fileId.isEmpty() && r.url.scheme() == QLatin1String("https")
                && r.url.host() == QLatin1String("dl.subdl.com"))
                results.append(r);
        }
    }
    return results;
}

}

SubDl::Query SubDl::parseQuery(QString text) {
    text.replace(QRegularExpression(QStringLiteral("[._]+")), QStringLiteral(" "));
    text = text.simplified();
    Query parsed;
    if (const SeasonEpisode found = seasonEpisodeIn(text); found.at >= 0) {
        parsed.season  = found.season;
        parsed.episode = found.episode;
        text.remove(found.at, found.length);
    }
    parsed.title = text.simplified();
    return parsed;
}

QList<SubDl::Result> SubDl::forEpisode(QList<Result> results, int season, int episode, const QString &episodeTitle) {
    // A title like "Episode 5" names nothing a file would carry.
    static const QRegularExpression numbered(QStringLiteral(R"(^\s*(?:e|ep|episode)?\s*\d+\s*$)"),
                                             QRegularExpression::CaseInsensitiveOption);
    const QString title = episodeTitle.size() >= 3 && !numbered.match(episodeTitle).hasMatch()
                              ? squashed(episodeTitle) : QString();
    struct Ranked { Result result; int score; };
    QList<Ranked> kept;
    QSet<QString> seen;
    for (Result &r : results) {
        if (seen.contains(r.fileId)) continue;
        seen.insert(r.fileId);
        const SeasonEpisode named = seasonEpisodeFromName(r.name);
        if (r.episode <= 0) r.episode = named.episode;
        const int fileSeason = r.season > 0 ? r.season : named.season;
        if (r.episode > 0 && r.episode != episode) continue;
        if (season > 0 && fileSeason > 0 && fileSeason != season) continue;
        int score = r.episode == episode ? 4 : 0;
        // SubDL's own tag is sometimes wrong; a name that says the episode is not.
        if (named.episode == episode) score += 1;
        if (!title.isEmpty() && squashed(r.name).contains(title)) score += 2;
        if (season > 0 && fileSeason == season) score += 1;
        kept.append({r, score});
    }
    std::stable_sort(kept.begin(), kept.end(), [](const Ranked &a, const Ranked &b) { return a.score > b.score; });
    QList<Result> ordered;
    ordered.reserve(kept.size());
    for (const Ranked &ranked : std::as_const(kept)) ordered.append(ranked.result);
    return ordered;
}

QList<SubDl::Result> SubDl::search(Client *client, const QString &query,
                                   const QString &apiKey, const QString &languages, const Hints &hints) {
    if (apiKey.isEmpty())
        throw AppException(QObject::tr("No SubDL API key is set. Add one under Settings, Integrations."), QObject::tr("Subtitles"));

    const Query parsed = parseQuery(query);
    if (parsed.title.isEmpty()) return {};

    // The player's own show goes by its TMDB id while the query still names it.
    const bool sameShow = !hints.show.isEmpty()
                          && parsed.title.compare(hints.show.simplified(), Qt::CaseInsensitive) == 0;
    const QStringList tmdb = sameShow ? hints.tmdb.split(QLatin1Char('/')) : QStringList();
    const bool byId = tmdb.size() == 2 && !tmdb[1].isEmpty();

    QMap<QString, QString> params = {
        {"api_key", apiKey},
        {"languages", languages},
        {"subs_per_page", "30"},
        {"unpack", "1"},
    };
    if (byId) {
        params.insert(QStringLiteral("tmdb_id"), tmdb[1]);
        params.insert(QStringLiteral("type"), tmdb[0]);
    } else {
        params.insert(QStringLiteral("film_name"), parsed.title);
        if (parsed.season > 0) params.insert(QStringLiteral("type"), QStringLiteral("tv"));
    }
    if (parsed.season > 0) {
        params.insert(QStringLiteral("season_number"), QString::number(parsed.season));
        // Season packs still come back with an episode asked for, their files listed.
        if (parsed.episode > 0) params.insert(QStringLiteral("episode_number"), QString::number(parsed.episode));
    }

    const auto fetch = [&](int pages) {
        QList<Result> results;
        for (int page = 1; page <= pages; ++page) {
            const QList<Result> batch = searchPage(client, params, page);
            if (batch.isEmpty() || client->isCancelled()) break;
            results += batch;
        }
        return results;
    };
    // Paging only pays when hunting one episode among a show's releases.
    const int pages = parsed.episode > 0 ? 3 : 1;
    QList<Result> results = fetch(pages);

    // Asked too narrowly for how SubDL files this show: less, then the title alone.
    if (results.isEmpty() && params.remove(QStringLiteral("episode_number")) > 0)
        results = fetch(pages);
    if (results.isEmpty() && params.remove(QStringLiteral("season_number")) > 0) {
        if (!byId) params.remove(QStringLiteral("type"));
        results = fetch(pages);
    }
    if (client->isCancelled() || parsed.episode <= 0) return results;

    const bool knownEpisode = sameShow && parsed.season == hints.season && parsed.episode == hints.episode;
    return forEpisode(results, parsed.season, parsed.episode, knownEpisode ? hints.episodeTitle : QString());
}

QString SubDl::cachePath(const QString &fileId) {
    const QString key = QString::fromLatin1(QCryptographicHash::hash(fileId.toUtf8(), QCryptographicHash::Sha256).toHex());
    return Settings::tempDir() + QStringLiteral("/subtitles/") + key + QStringLiteral(".srt");
}

QString SubDl::fetch(Client *client, const Result &result) {
    const QString path = cachePath(result.fileId);
    // A half-written file from an interrupted run is re-fetched.
    if (const qint64 cached = QFileInfo(path).size();
        cached > 0 && (result.size <= 0 || cached == result.size))
        return path;

    const auto response = client->getBytes(result.url.toString());
    if (response.code != 200 || response.bytes.isEmpty())
        throw AppException(QObject::tr("Could not download %1.").arg(result.name), QObject::tr("Subtitles"));

    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        throw AppException(QObject::tr("Could not write the subtitle to the cache."), QObject::tr("Subtitles"));
    if (file.write(response.bytes) != response.bytes.size() || !file.commit())
        throw AppException(QObject::tr("Could not write the subtitle to the cache."), QObject::tr("Subtitles"));
    return path;
}
