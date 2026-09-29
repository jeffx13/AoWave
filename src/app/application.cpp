#include "app/application.h"
#include <QDir>
#include <QFutureWatcher>
#include "media/ytdlp.h"
#include "trackers/anilist.h"
#include "trackers/mal.h"
#include "trackers/trakt.h"
#include <QNetworkProxyFactory>
#include <QFontDatabase>
#include <QQuickStyle>
#include <QtConcurrent/QtConcurrentRun>
#include <QThreadPool>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include <QBuffer>
#include <QClipboard>
#include <QGuiApplication>
#include <QImage>
#include <QDeadlineTimer>
#include <QFileInfo>
#include <QProcess>
#include <QThread>
#include <libxml/parser.h>
#include "core/async.h"
#include "core/logger.h"
#include "core/settings.h"
#include "core/appshell.h"
#include "ui/qrcode.h"
#include "media/danmaku.h"
#include "media/mpvplayer.h"
#include "net/client.h"
#include "net/hlsproxy.h"
#include "net/cloudflare.h"
#include "platform/mediasession.h"
#include "platform/platform.h"
#include "providers/anikoto.h"
#include "providers/bilibili.h"
#include "providers/iyf.h"
#include "providers/animepahe.h"
#include "providers/olevod.h"
#include "providers/allanime.h"
#include "providers/duboku.h"
#include "providers/pstream.h"
#include "providers/miruro.h"

// A jump list entry's argument: the show to resume, its link percent-encoded after this. No link
// means the show watched last.
static const QString kResumeArgument = QStringLiteral("aowave:resume?");

Application::Application(const QString &launchPath)
    : m_explorer(this)
    , m_library(this)
    , m_libraryProxyModel(&m_library)
    , m_playlist(this)
    , m_downloads(this)
{
    REGISTER_QML_SINGLETON(Application, this);
    REGISTER_QML_SINGLETON(AppShell, &AppShell::instance());
    REGISTER_QML_SINGLETON(Settings, &Settings::instance());
    AppShell::instance().installPointerFilter();

    xmlInitParser();
    new HlsProxy(this);
    DanmakuAss::pruneCache(Settings::tempDir() + QStringLiteral("/danmaku"));
    DanmakuAss::pruneCache(Settings::tempDir() + QStringLiteral("/subtitles"));
    DanmakuAss::pruneCache(Settings::tempDir() + QStringLiteral("/downloadsubs"));
    m_libraryProxyModel.setSourceModel(&m_library);
    m_logView.setSourceModel(&QLog::logListModel);

    QList<ShowProvider *> providers{
        new Anikoto, new Bilibili, new Iyf, new AnimePahe, new Olevod,
        new AllAnime, new Duboku, new PStream, new Miruro,
    };
    // Until syncsProgress() existed, every provider's pushes were queued as undelivered.
    for (ShowProvider *provider : std::as_const(providers))
        if (!provider->syncsProgress()) m_library.clearSyncOutbox(provider->name());
    m_providers.setProviders(std::move(providers));

    m_downloads.restoreQueue();

    m_trackers.setTrackers({ new Trakt, new AniList, new Mal });
    connect(&m_library, &Library::newEpisodesFound, this, &Application::announceNewEpisodes);
    connect(&m_downloads, &DownloadQueue::downloadFinished, this, [this](const QString &path, const QString &title) {
        m_library.addNotification(QStringLiteral("download"), title, tr("Downloaded"), path);
    });

    connect(&m_playlist, &Playlist::episodeCompleted, this,
            [this](const QString &showLink, int episodeNumber) {
                m_trackers.autoPush(showLink, episodeNumber,
                                    [this](const QString &link, const QString &service) {
                                        return m_library.trackerLink(link, service);
                                    });
            });

    // Off the startup path: the window should be up before the request goes out.
    QTimer::singleShot(1500, this, [this]() { refreshBilibiliAccount(); });

    m_playlist.setEpisodeResumeLookup([this](const QSharedPointer<PlaylistItem> &playlist) {
        m_library.restoreEpisodeProgress(playlist);
    });
    m_playlist.setSyncHooks({
        [this](const QString &link, const QString &service) { return m_library.syncState(link, service); },
        [this](const QString &link, const QString &service, const Library::SyncState &state) {
            m_library.setSyncState(link, service, state);
        },
        [this](const QString &link, const QString &service, double seconds, double duration) {
            m_library.queueSyncOutbox(link, service, seconds, duration);
        },
        [this](const QString &service) { return m_library.syncOutbox(service); },
        [this](const QString &service, const Library::OutboxEntry &entry) {
            m_library.removeFromSyncOutbox(service, entry);
        },
    });
    // The account is further on than the local file; seek there.
    connect(&m_playlist, &Playlist::remoteResumeResolved, this, [](QString, double seconds) {
        if (auto *mpv = MpvPlayer::instance(); mpv && seconds > mpv->preciseTime() + 5.0)
            mpv->showText(tr("Your account is further ahead - press Ctrl+R to resume there."));
    });
    connect(&m_playlist, &Playlist::metadataLoaded, &m_library, &Library::cacheHistoryMeta);
    connect(&m_show, &ShowDetails::episodeMarked, &m_library, &Library::updateProgress);

    m_playlist.setLocalResumeLookup([this](const QString &folder) {
        return m_library.localFolderProgress(folder);
    });
    connect(&m_playlist, &Playlist::localProgressUpdated,
            &m_library,  &Library::updateLocalProgress);
    connect(&m_playlist, &Playlist::localProgressStale,
            &m_library,  &Library::forgetLocalProgress);

    if (launchPath.startsWith(kResumeArgument))
        // Once the window is up: resuming navigates.
        QTimer::singleShot(0, this, [this, launchPath] { openArgument(launchPath); });
    else if (!launchPath.isEmpty())
        m_playlist.openUrl(QUrl::fromUserInput(launchPath), false);

    connect(&m_playlist, &Playlist::progressUpdated,
            &m_library,  &Library::updateProgress);

    connect(&m_playlist, &Playlist::progressUpdated,
            &m_show, [this](const QString &link, int, double) {
                if (m_show.show().link == link) m_show.onPlaybackIndexChanged();
            });

    connect(&m_playlist, &Playlist::episodeStarted,
            &m_library, &Library::recordHistory);

    connect(&m_playlist, &Playlist::currentItemChanged, this,
            [this](const QModelIndex &index) {
                auto *item = static_cast<PlaylistItem *>(index.internalPointer());
                m_skipTimes.onCurrentItemChanged(item);
                m_discordPresence.onCurrentItemChanged(item);
                m_show.onPlaybackIndexChanged();
            });

    connect(&m_library, &Library::fetchedAllEpCounts,
            &m_libraryProxyModel, &LibraryProxyModel::refreshFilter);

    connect(&m_show, &ShowDetails::showChanged, this, [this]() {
        const ShowData &show = m_show.show();
        m_library.updateShowCover(show.link, show.coverUrl);
        auto playlist = m_show.playlist();
        m_library.restoreEpisodeProgress(playlist);
        m_library.cacheHistoryMeta(show.link, show.title, show.coverUrl,
                                   show.provider ? show.provider->name() : QString(),
                                   playlist ? playlist->episodeCount() : 0);
        if (m_pendingAutoResume) {
            m_pendingAutoResume = false;
            continueWatching();
        }
    });

    std::setlocale(LC_NUMERIC, "C");
    QQuickStyle::setStyle("Universal");

    browse(true);
    checkForUpdates();
}

bool Application::isNewerVersion(const QString &latest, const QString &current) {
    const auto lp = latest.split('.');
    const auto cp = current.split('.');
    for (int i = 0; i < qMax(lp.size(), cp.size()); ++i) {
        int l = i < lp.size() ? lp[i].toInt() : 0;
        int c = i < cp.size() ? cp[i].toInt() : 0;
        if (l != c) return l > c;
    }
    return false;
}

void Application::checkForUpdates() {
    m_updateCancel = CancelToken{};
    const CancelToken cancel = m_updateCancel;
    m_updateFuture = QtConcurrent::run([this, cancel]() {
        Client client(cancel, false);
        auto resp = client.get("https://api.github.com/repos/jeffx13/AoWave/releases/latest",
                               {{"Accept", "application/vnd.github+json"}, {"User-Agent", APP_NAME}});
        if (cancel.isCancelled()) return;
        auto obj = resp.toJsonObject();
        QString latest = obj.value("tag_name").toString();
        if (latest.startsWith('v') || latest.startsWith('V')) latest = latest.mid(1);
        if (latest.isEmpty() || !isNewerVersion(latest, QStringLiteral(APP_VERSION))) return;

        // Opened in the browser, so only ever one of this project's pages on GitHub.
        const QUrl page(obj.value("html_url").toString());
        const QString url = page.scheme() == QLatin1String("https") && page.host() == QLatin1String("github.com")
                                    && page.path().startsWith(QLatin1String("/jeffx13/AoWave/"))
                                ? page.toString()
                                : QStringLiteral("https://github.com/jeffx13/AoWave/releases");
        QMetaObject::invokeMethod(this, [this, latest, url, cancel]() {
            if (cancel.isCancelled() || latest == Settings::instance().get(Config::UpdateDismissed)) return;
            m_updateVersion = latest;
            m_updateUrl = url;
            emit updateChanged();
        }, Qt::QueuedConnection);
    });
}

void Application::runYtdlp(const QStringList &arguments, std::function<void(bool, const QString &)> done) {
    const QString exe = Settings::toolPath(QStringLiteral("yt-dlp.exe"));
    if (m_ytdlpBusy) return;
    if (!QFileInfo::exists(exe)) {
        m_ytdlpStatus = tr("yt-dlp.exe is not next to the app");
        emit ytdlpStatusChanged();
        return;
    }
    auto *process = new QProcess(this);
    process->setProcessChannelMode(QProcess::MergedChannels);
    const auto finish = [this, process, done](bool ok) {
        process->deleteLater();
        m_ytdlpBusy = false;
        done(ok, QString::fromUtf8(process->readAll()).trimmed());
        emit ytdlpStatusChanged();
    };
    connect(process, &QProcess::finished, this, [finish](int code, QProcess::ExitStatus status) {
        finish(status == QProcess::NormalExit && code == 0);
    });
    connect(process, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) finish(false);
    });
    m_ytdlpBusy = true;
    emit ytdlpStatusChanged();
    process->start(exe, arguments);
}

void Application::refreshYtdlpVersion() {
    runYtdlp({QStringLiteral("--version")}, [this](bool ok, const QString &output) {
        m_ytdlpStatus = ok ? tr("Version %1").arg(output) : tr("yt-dlp did not run");
    });
}

void Application::updateYtdlp() {
    runYtdlp({QStringLiteral("-U")}, [this](bool, const QString &output) {
        // yt-dlp's own last word: updated, already current, or why not.
        m_ytdlpStatus = output.section(QLatin1Char('\n'), -1).trimmed();
        // A newer one may take the runtime an older one refused.
        YtDlp::forgetVersion();
        applyJsRuntime(QStringLiteral("saved"));
    });
}

void Application::useJsRuntime(const QUrl &file) {
    const QString path = QDir::toNativeSeparators(file.toLocalFile());
    const QString name = QFileInfo(path).baseName().toLower();
    if (name != QLatin1String("node") && name != QLatin1String("deno")) {
        AppShell::instance().reportError(tr("Pick deno.exe or node.exe."), tr("JavaScript Runtime"));
        return;
    }
    applyJsRuntime(name + QLatin1Char(':') + path);
}

void Application::applyJsRuntime(const QString &how) {
    if (m_jsRuntimeBusy) return;
    m_jsRuntimeBusy = true;
    emit jsRuntimeChanged();
    const QString saved = Settings::instance().value(QStringLiteral("ytdlp/jsRuntime")).toString();
    struct Found { QString runtime, version, ytdlp; };
    auto *watcher = new QFutureWatcher<Found>(this);
    connect(watcher, &QFutureWatcher<Found>::finished, this, [this, watcher, how, saved] {
        watcher->deleteLater();
        const Found found = watcher->result();
        m_jsRuntimeBusy = false;
        // A pick that does not run, or a search that finds nothing, keeps what worked before.
        const bool picked = how != QLatin1String("saved") && how != QLatin1String("find");
        if (found.version.isEmpty() && picked)
            AppShell::instance().reportError(tr("That doesn't run, or is older than yt-dlp takes: it needs "
                                                "Deno 2.3 or Node 22 and later."), tr("JavaScript Runtime"));
        const QString runtime = !found.version.isEmpty() ? found.runtime : saved;
        if (runtime != saved) Settings::instance().setValue(QStringLiteral("ytdlp/jsRuntime"), runtime);

        const QString name = runtime.section(QLatin1Char(':'), 0, 0);
        const QString shown = name == QLatin1String("deno") ? QStringLiteral("Deno") : QStringLiteral("Node");
        const QString version = found.runtime == runtime ? found.version : QString();
        m_jsRuntime = runtime.isEmpty() ? QString()
            : (version.isEmpty() ? shown : shown + QLatin1Char(' ') + version) + QStringLiteral("  ·  ")
                  + runtime.section(QLatin1Char(':'), 1);
        if (!runtime.isEmpty() && !YtDlp::takesJsRuntimes(found.ytdlp))
            m_jsRuntime += QLatin1Char('\n') + tr("This yt-dlp is too old to use it: update yt-dlp above.");
        // mpv runs yt-dlp for page links itself.
        const QStringList arguments = YtDlp::runtimeArguments(runtime, found.ytdlp);
        if (auto *mpv = MpvPlayer::instance())
            mpv->setMpvProperty(QStringLiteral("ytdl-raw-options"),
                                arguments.isEmpty() ? QString() : QStringLiteral("js-runtimes=") + arguments.last());
        emit jsRuntimeChanged();
    });
    watcher->setFuture(QtConcurrent::run([how, saved]() -> Found {
        QString runtime = how == QLatin1String("find") || (how == QLatin1String("saved") && saved.isEmpty())
                              ? YtDlp::findJsRuntime()
                              : how == QLatin1String("saved") ? saved : how;
        return {runtime, YtDlp::runtimeVersion(runtime), YtDlp::version()};
    }));
}

void Application::dismissUpdate() {
    Settings::instance().set(Config::UpdateDismissed, m_updateVersion);
    m_updateVersion.clear();
    m_updateUrl.clear();
    emit updateChanged();
}

Application::~Application() {
    m_updateCancel.cancel();
    m_loginCancel.cancel();
    m_migrateCancel.cancel();
    waitFor(m_updateFuture, "Application update check");
    waitFor(m_migrateFuture, "Application migrate");
    waitFor(m_loginFuture, "Application bilibili login");
    waitFor(m_accountFuture, "Application bilibili account");
    xmlCleanupParser();
}

// Items copy the application font at creation and never hear about changes.
// Only text that followed the old app font moves: a family set on purpose (a subtitle preview,
// say) stays, where overwriting it would also break its binding.
static void applyFontFamily(QQuickItem *item, const QString &from, const QString &to) {
    if (item->metaObject()->indexOfProperty("font") >= 0) {
        QFont font = item->property("font").value<QFont>();
        if (font.family() == from) {
            font.setFamily(to);
            item->setProperty("font", font);
        }
    }
    for (QQuickItem *child : item->childItems()) applyFontFamily(child, from, to);
}

void Application::setFont(const QString &fontPath) {
    int fontId = QFontDatabase::addApplicationFont(fontPath);
    auto families = QFontDatabase::applicationFontFamilies(fontId);
    const QString defaultFamily = fontId != -1 && !families.isEmpty() ? families.first() : QGuiApplication::font().family();
    // The signal also comes whenever settings.ini is written, so an unchanged family walks nothing.
    const auto updateFont = [defaultFamily, current = std::make_shared<QString>()](bool live) {
        const QString chosen = Settings::instance().appFont();
        QFont font(chosen.isEmpty() ? defaultFamily : chosen);
        font.setPixelSize(20);
        const QString previous = std::exchange(*current, font.family());
        if (live && previous == font.family()) return;
        QGuiApplication::setFont(font);
        if (!live) return;
        for (QWindow *window : QGuiApplication::allWindows())
            if (auto *quick = qobject_cast<QQuickWindow *>(window))
                applyFontFamily(quick->contentItem(), previous, font.family());
    };
    connect(&Settings::instance(), &Settings::appFontChanged, this, [updateFont]() { updateFont(true); });
    updateFont(false);
}

void Application::reportMissingProvider(const QString &provider, const QString &title) {
    AppShell::instance().reportError(
        tr("%1 comes from %2, which is not available any more. Use Migrate Provider "
           "in the library to move it to another source.").arg(title, provider),
        tr("Provider not available"));
}

void Application::announceNewEpisodes(const QVariantList &shows) {
    for (const QVariant &entry : shows) {
        const QVariantMap show = entry.toMap();
        m_library.addNotification(QStringLiteral("episode"), show.value(QStringLiteral("title")).toString(),
                                  tr("Episode %1 is out.").arg(show.value(QStringLiteral("total")).toInt()),
                                  show.value(QStringLiteral("link")).toString(),
                                  show.value(QStringLiteral("provider")).toString());
    }
    const QString first = shows.first().toMap().value(QStringLiteral("title")).toString();
    QString title, message;
    if (shows.size() == 1) {
        title = tr("New episode of %1").arg(first);
        message = tr("Episode %1 is out.").arg(shows.first().toMap().value(QStringLiteral("total")).toInt());
    } else {
        QStringList names;
        for (const QVariant &show : shows.mid(0, 3)) names << show.toMap().value(QStringLiteral("title")).toString();
        title = tr("New episodes");
        message = shows.size() > 3 ? tr("%1 and %2 more").arg(names.join(QStringLiteral(", "))).arg(shows.size() - 3)
                                   : names.join(QStringLiteral(", "));
    }
    AppShell::instance().reportInfo(message, title);
    if (!Settings::instance().episodeNotifications()) return;
    Platform::notify(title, message, [this] {
        AppShell::instance().navigateTo(AppShell::Library);
        if (!m_window) return;
        m_window->setWindowStates(m_window->windowStates() & ~Qt::WindowMinimized);
        m_window->show();
        m_window->raise();
        m_window->requestActivate();
    });
}

void Application::attachWindow(QWindow *window) {
    if (!window) return;
    m_window = window;
    // Once the player exists, and off the start: both ask programs for their versions.
    QTimer::singleShot(3000, this, [this] { applyJsRuntime(QStringLiteral("saved")); });
    // Here rather than in the constructor, so no test run touches the user's taskbar.
    HistoryModel *history = m_library.historyModel();
    connect(history, &QAbstractItemModel::modelReset, this, &Application::updateJumpList);
    connect(history, &QAbstractItemModel::rowsRemoved, this, &Application::updateJumpList);
    connect(history, &QAbstractItemModel::dataChanged, this, &Application::updateJumpList);
    updateJumpList();
    auto *session = new MediaSession(window);
    const auto updateTrack = [this, window, session] {
        const QString item = m_playlist.currentItemName();
        const QString show = m_playlist.currentShowName();
        window->setTitle(show.isEmpty() ? (item.isEmpty() ? QStringLiteral(APP_NAME) : item) : show);
        session->setTrack(item);
    };
    connect(&m_playlist, &Playlist::currentItemChanged, session, updateTrack, Qt::QueuedConnection);
    if (auto *player = MpvPlayer::instance()) {
        connect(player, &MpvPlayer::mpvStateChanged, session, [session, player] {
            session->setPlaying(player->state() == MpvPlayer::Playing);
        }, Qt::QueuedConnection);
        connect(player, &MpvPlayer::timeChanged, session, [session, player] {
            session->setProgress(player->preciseTime(), player->preciseDuration());
        }, Qt::QueuedConnection);
    }
    connect(session, &MediaSession::previousRequested, this, [this] { m_playlist.stepItem(-1); });
    connect(session, &MediaSession::nextRequested, this, [this] { m_playlist.stepItem(1); });
    connect(session, &MediaSession::playPauseRequested, this, [] {
        if (auto *player = MpvPlayer::instance()) player->togglePlayPause();
    });
    QTimer::singleShot(0, session, updateTrack);
}

void Application::openArgument(const QString &argument) {
    if (argument.startsWith(kResumeArgument)) {
        const QString link = QUrl::fromPercentEncoding(argument.mid(kResumeArgument.size()).toUtf8());
        resumeFromHistory(link.isEmpty() ? m_library.historyModel()->linkAt(0) : link);
        return;
    }
    m_playlist.openUrl(QUrl::fromUserInput(argument), true);
}

void Application::setOpenWith(bool on) {
    // Only what a folder load picks up: see LocalMedia.
    Platform::setOpenWith({"mp4", "mkv", "avi", "webm", "mov"}, on);
    emit openWithChanged();
}

void Application::updateJumpList() {
    const HistoryModel *history = m_library.historyModel();
    QList<Platform::JumpItem> items;
    for (int row = 0; row < std::min(history->rowCount(), 6); ++row) {
        const QModelIndex index = history->index(row);
        items.append({index.data(HistoryModel::Title).toString(),
                      kResumeArgument + QString::fromLatin1(QUrl::toPercentEncoding(index.data(HistoryModel::Link).toString()))});
    }
    // History rows change with every progress save; the list only with a title or the order.
    if (items == m_jumpList) return;
    m_jumpList = items;
    QList<Platform::JumpItem> tasks;
    if (!items.isEmpty()) tasks.append({tr("Continue watching"), kResumeArgument});
    Platform::setJumpList(tasks, tr("Recently watched"), items);
}

static QVariantMap setFilters(const QVariantMap &filters) {
    QVariantMap set;
    for (auto it = filters.cbegin(); it != filters.cend(); ++it)
        if (!it.value().toString().isEmpty()) set.insert(it.key(), it.value());
    return set;
}

void Application::search(const QString &query, const QVariantMap &filters) {
    auto *provider = m_providers.currentProvider();
    if (!provider) return;
    const QVariantMap set = setFilters(filters);
    // A provider that cannot narrow words searches them as they are.
    if (!set.isEmpty() && provider->filtersSearch())
        m_explorer.filtered(query, 1, m_providers.currentTypeIndex(), provider, set, false);
    else
        m_explorer.search(query, 1, m_providers.currentTypeIndex(), provider);
}

void Application::browse(bool latest, const QVariantMap &filters) {
    auto *provider = m_providers.currentProvider();
    if (!provider) return;
    const int type = m_providers.currentTypeIndex();
    if (const QVariantMap set = setFilters(filters); !set.isEmpty())
        m_explorer.filtered(QString(), 1, type, provider, set, latest);
    else if (latest) m_explorer.latest(1, type, provider);
    else             m_explorer.popular(1, type, provider);
}

void Application::loadResult(SearchResults &src, int index) {
    ShowData show = src.resultAt(index);
    ShowData::WatchState watch = m_library.entryForLink(show.link).watchState();
    watch.playlist = m_playlist.find(show.link);
    m_show.setShow(show, watch);
}

void Application::appendResult(SearchResults &src, int index, bool play) {
    auto show = src.resultAt(index);
    if (!show.provider) return;
    ShowData::WatchState watch = m_library.entryForLink(show.link).watchState();
    QSharedPointer<PlaylistItem> cached;
    if (m_show.show().link == show.link)
        cached = m_show.playlist();
    m_playlist.appendShow(show.title, show.link, show.provider, cached, watch, play);
}

void Application::openEntry(const LibraryEntry &entry, bool autoResume) {
    if (m_show.show().link == entry.link) {
        m_pendingAutoResume = false;
        if (autoResume) continueWatching();
        else            AppShell::instance().navigateTo(AppShell::Page::Info);
        return;
    }

    auto *provider = m_providers.byName(entry.provider);
    if (!provider) {
        reportMissingProvider(entry.provider, entry.title);
        return;
    }
    ShowData show(entry.title, entry.link, entry.cover, provider);
    ShowData::WatchState watch = entry.watchState();
    watch.playlist = m_playlist.find(entry.link);
    m_pendingAutoResume = autoResume;
    m_show.setShow(show, watch);
}

void Application::reloadShow() {
    const ShowData current = m_show.show();
    if (current.link.isEmpty() || !current.provider) return;

    ShowData show(current.title, current.link, current.coverUrl, current.provider,
                  current.latestTxt, current.type);
    ShowData::WatchState watch = m_library.entryForLink(current.link).watchState();
    watch.playlist = m_playlist.find(current.link);
    m_show.reload(show, watch);
}

void Application::loadShow(int index, bool fromLibrary) {
    m_pendingAutoResume = false;
    if (!fromLibrary) { loadResult(m_explorer, index); return; }

    auto entry = m_library.entryAt(index);
    if (!entry.valid) return;
    openEntry(entry, false);
}

QString Application::showUrl(int index, bool fromLibrary) const {
    if (!fromLibrary) {
        const ShowData show = index == -1 ? m_show.show() : m_explorer.resultAt(index);
        return show.provider ? show.provider->showUrl(show.link) : QString();
    }
    const LibraryEntry entry = m_library.entryAt(index);
    const ShowProvider *provider = entry.valid ? m_providers.byName(entry.provider) : nullptr;
    return provider ? provider->showUrl(entry.link) : QString();
}

void Application::resumeFromHistory(const QString &link) {
    LibraryEntry entry = m_library.entryForLink(link);
    if (!entry.valid) entry = m_library.historyEntry(link);
    if (entry.valid) openEntry(entry, true);
}

void Application::addToLibrary(int index, int libraryType) {
    auto show = (index == -1) ? m_show.show() : m_explorer.resultAt(index);
    m_library.add(show, libraryType);
}

void Application::searchOnProvider(const QString &providerName, const QString &query) {
    ShowProvider *provider = m_providers.byName(providerName);
    if (!provider) {
        AppShell::instance().reportError(tr("%1 is not available.").arg(providerName), tr("Migrate"));
        return;
    }
    m_migrateSearch.search(query, 1, 0, provider);
}

void Application::openShowInfo(const QString &link, const QString &providerName, const QString &title) {
    if (link.isEmpty()) return;
    if (m_show.show().link == link) { AppShell::instance().navigateTo(AppShell::Page::Info); return; }
    auto *provider = m_providers.byName(providerName);
    if (!provider) { reportMissingProvider(providerName, title); return; }
    LibraryEntry entry = m_library.entryForLink(link);
    if (!entry.valid) entry = m_library.historyEntry(link);
    ShowData show(title, link, entry.valid ? entry.cover : QString(), provider);
    ShowData::WatchState watch = entry.valid ? entry.watchState() : ShowData::WatchState{};
    watch.playlist = m_playlist.find(link);
    m_pendingAutoResume = false;
    m_show.setShow(show, watch);
}

bool Application::bilibiliSignedIn() const {
    return !Bilibili::storedCookies().value(QStringLiteral("SESSDATA")).isEmpty();
}

QString Application::bilibiliAccount() const {
    return Settings::instance().value(QStringLiteral("bilibili/account")).toString();
}

// Doubles as a liveness check: an expired SESSDATA comes back isLogin=false.
void Application::refreshBilibiliAccount() {
    if (!bilibiliSignedIn() || m_accountFuture.isRunning()) return;
    const auto cookies = Bilibili::storedCookies();
    QStringList jar;
    for (auto it = cookies.constBegin(); it != cookies.constEnd(); ++it)
        if (!it.value().isEmpty()) jar << it.key() + QLatin1Char('=') + it.value();

    m_accountFuture = QtConcurrent::run([this, cookie = jar.join(QStringLiteral("; ")),
                                         cancel = m_loginCancel]() {
        Client client(cancel, false);
        const auto response = client.setBypassEnabled(false).get(
            QStringLiteral("https://api.bilibili.com/x/web-interface/nav"),
            {{"Cookie", cookie}, {"Referer", "https://www.bilibili.com/"}});
        if (cancel.isCancelled()) return;
        const QJsonObject data = response.toJsonObject().value("data").toObject();
        const bool live = data.value("isLogin").toBool();
        const QString name = data.value("uname").toString();
        // A transport failure is not a sign-out.
        if (response.code != 200 || data.isEmpty()) return;
        QMetaObject::invokeMethod(this, [this, live, name]() {
            Settings::instance().setValue(QStringLiteral("bilibili/account"),
                                          live ? name : QString());
            if (!live) {
                logWarn() << "Bilibili" << "the stored session is no longer valid";
                bilibiliSignOut();
                return;
            }
            emit bilibiliSignedInChanged();
        }, Qt::QueuedConnection);
    });
}

// QML's Image reads a data URL directly; the alternative is an image provider.
static QString qrDataUrl(const QString &text) {
    const QImage code = QrCode::render(text.toUtf8());
    if (code.isNull()) return {};
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    if (!code.save(&buffer, "PNG")) return {};
    return QStringLiteral("data:image/png;base64,") + QString::fromLatin1(png.toBase64());
}

void Application::setBilibiliLoginState(const QString &url, const QString &status) {
    if (m_loginUrl == url && m_loginStatus == status) return;
    if (m_loginUrl != url) m_loginQr = url.isEmpty() ? QString() : qrDataUrl(url);
    m_loginUrl = url;
    m_loginStatus = status;
    emit bilibiliLoginStateChanged();
}

void Application::copyBilibiliLoginLink() const {
    if (!m_loginUrl.isEmpty()) QGuiApplication::clipboard()->setText(m_loginUrl);
}

// All three cookies or nothing.
void Application::adoptBilibiliCookies(const QMap<QString, QString> &cookies) {
    if (!Bilibili::storeCookies(cookies)) {
        AppShell::instance().reportError(tr("The session could not be stored securely, so it was not kept."),
                                         "Bilibili");
        return;
    }
    // settingsChanged is what the provider listens on to rebuild its headers.
    setBilibiliLoginState({}, {});
    emit bilibiliSignedInChanged();
    refreshBilibiliAccount();
    AppShell::instance().reportInfo(tr("Signed in to Bilibili."), "Bilibili");
}

// The approval link is an H5 page for the phone app; a desktop browser cannot complete it.
void Application::bilibiliLogin() {
    if (m_loginFuture.isRunning()) return;
    setBilibiliLoginState({}, tr("Starting"));

    m_loginFuture = QtConcurrent::run([this, cancel = m_loginCancel]() {
        Client client(cancel, false);
        const Bilibili::LoginTicket ticket = Bilibili::beginLogin(&client);
        if (cancel.isCancelled()) return;
        if (!ticket.isValid()) {
            QMetaObject::invokeMethod(this, [this]() {
                setBilibiliLoginState({}, {});
                AppShell::instance().reportError(
                    tr("Bilibili would not start a sign-in. Check the connection and try again."),
                    "Bilibili");
            }, Qt::QueuedConnection);
            return;
        }

        const QString url = ticket.confirmUrl.toString();
        QMetaObject::invokeMethod(this, [this, url]() {
            setBilibiliLoginState(url, tr("Scan with the Bilibili app"));
            if (bilibiliLoginQr().isEmpty())
                AppShell::instance().reportError(
                    tr("Bilibili's sign-in link could not be turned into a QR code. Copy it instead "
                       "and open it on your phone."), "Bilibili");
        }, Qt::QueuedConnection);

        // The ticket lives three minutes; once a second is what the web client does.
        const QDeadlineTimer deadline(180000);
        Bilibili::LoginPoll result;
        while (!deadline.hasExpired() && !cancel.isCancelled()) {
            QThread::msleep(1000);
            if (cancel.isCancelled()) return;
            result = Bilibili::pollLogin(&client, ticket);
            if (result.status == Bilibili::LoginStatus::Confirmed
                || result.status == Bilibili::LoginStatus::Expired) break;
            if (result.status == Bilibili::LoginStatus::Scanned)
                QMetaObject::invokeMethod(this, [this, url]() {
                    setBilibiliLoginState(url, tr("Confirm on your phone"));
                }, Qt::QueuedConnection);
        }
        if (cancel.isCancelled()) return;

        const auto status = result.status;
        const auto cookies = result.cookies;
        QMetaObject::invokeMethod(this, [this, status, cookies]() {
            if (status == Bilibili::LoginStatus::Confirmed && !cookies.isEmpty()) {
                adoptBilibiliCookies(cookies);
                return;
            }
            setBilibiliLoginState({}, {});
            AppShell::instance().reportError(
                status == Bilibili::LoginStatus::Expired
                    ? tr("The Bilibili sign-in code expired before it was scanned. Try again.")
                    : tr("The Bilibili sign-in was not completed."),
                "Bilibili");
        }, Qt::QueuedConnection);
    });
}

void Application::bilibiliCancelLogin() {
    abandonBilibiliLogin();
    setBilibiliLoginState({}, {});
}

// The next login or account check needs a live token, not the one just cancelled.
void Application::abandonBilibiliLogin() {
    m_loginCancel.cancel();
    m_loginCancel = CancelToken{};
}

bool Application::bilibiliSignInWithCookies(const QString &pastedCookies) {
    const auto cookies = Bilibili::parseCookieString(pastedCookies);
    if (cookies.isEmpty()) {
        AppShell::instance().reportError(
            tr("That does not look like a Bilibili session. Paste the whole cookie string - it has "
               "to contain SESSDATA, bili_jct and DedeUserID."), "Bilibili");
        return false;
    }
    abandonBilibiliLogin();
    adoptBilibiliCookies(cookies);
    return true;
}

void Application::bilibiliSignOut() {
    abandonBilibiliLogin();
    setBilibiliLoginState({}, {});
    Bilibili::storeCookies({});
    Settings::instance().setValue(QStringLiteral("bilibili/account"), QString());
    emit bilibiliSignedInChanged();
}

void Application::migrateShow(const QString &oldLink, int resultIndex, int resumeEpisode) {
    const auto oldEntry = m_library.entryForLink(oldLink);
    if (!oldEntry.valid) { AppShell::instance().reportError(tr("Library item not found"), tr("Migrate")); return; }
    if (resultIndex < 0 || resultIndex >= m_migrateSearch.count()) {
        AppShell::instance().reportError(tr("No show selected"), tr("Migrate")); return;
    }
    ShowData newShow = m_migrateSearch.resultAt(resultIndex);
    if (!newShow.provider) { AppShell::instance().reportError(tr("Selected show has no provider"), tr("Migrate")); return; }

    if (newShow.link != oldLink && m_library.linkExists(newShow.link)) {
        AppShell::instance().reportError(tr("That show is already in your library."), tr("Migrate"));
        return;
    }

    if (m_migrateFuture.isRunning()) {
        AppShell::instance().reportError(tr("A migration is already running."), tr("Migrate"));
        return;
    }

    ShowProvider *provider = newShow.provider;

    m_migrateCancel = CancelToken{};
    m_migrateFuture = QtConcurrent::run([this, newShow, oldLink, resumeEpisode, provider,
                                         cancel = m_migrateCancel]() mutable {
        Client client(cancel);
        try {
            provider->loadPlaylist(&client, newShow);
        } catch (const std::exception &e) {
            AppShell::instance().reportError(
                tr("Could not load the show on that provider:\n%1").arg(QString::fromUtf8(e.what())), tr("Migrate"));
            return;
        } catch (...) {
            logWarn() << "Migrate" << "provider threw a non-standard exception";
            return;
        }
        if (cancel.isCancelled()) return;

        auto playlist = newShow.playlist();
        const int total = playlist ? playlist->episodeCount() : 0;
        int targetIndex = qBound(0, resumeEpisode - 1, total > 0 ? total - 1 : 0);
        // last_watched_index is positional, so the episode is found by number.
        if (playlist) {
            for (int i = 0; i < playlist->count(); ++i) {
                auto ep = playlist->at(i);
                if (ep && int(ep->number) == resumeEpisode) { targetIndex = i; break; }
            }
        }
        const QString title = newShow.title, cover = newShow.coverUrl, newLink = newShow.link;
        const QString provName = provider->name();
        const int showType = newShow.type;
        QMetaObject::invokeMethod(this, [=, this]() {
            const bool wasPlaying = m_playlist.isPlaying(oldLink);
            bool ok = m_library.migrate(oldLink, newLink, title, cover, provName, showType,
                                        targetIndex, total);
            if (!ok) {
                AppShell::instance().reportError(tr("Migration failed (target may already be in the library)."), tr("Migrate"));
                return;
            }
            if (wasPlaying) {
                m_playlist.relink(oldLink, newLink);
                // The open tree still has the old provider's episode order until it is reopened.
                AppShell::instance().reportInfo(
                    tr("Migrated. The episode playing now still comes from the old source; reopen the "
                       "show to switch over."), tr("Migrate"));
            }
            Settings &s = Settings::instance();
            const QString skipVal = s.value(Config::skipProfile(oldLink)).toString();
            if (!skipVal.isEmpty()) s.setValue(Config::skipProfile(newLink), skipVal);
            const QString malVal = s.value(Config::skipMal(oldLink)).toString();
            if (!malVal.isEmpty()) s.setValue(Config::skipMal(newLink), malVal);

            auto migrated = newShow.playlist();
            if (migrated) migrated->setCurrentIndex(targetIndex);
            m_playlist.rekey(oldLink, migrated);
            if (newLink != oldLink && m_show.show().link == oldLink) {
                ShowData::WatchState watch = m_library.entryForLink(newLink).watchState();
                watch.playlist = migrated;
                m_show.setShow(newShow, watch, false);
            }
        }, Qt::QueuedConnection);
    });
}

void Application::playFromEpisodeList(int index, bool append) {
    auto playlist = m_show.playlist();
    if (!playlist) return;
    if (!playlist->isValidIndex(index)) return;

    if (append)
        m_playlist.append(playlist);
    else
        m_playlist.playShowNow(playlist, index);
}

void Application::continueWatching() {
    playFromEpisodeList(qMax(m_show.continueIndex(), 0), false);
}

void Application::downloadCurrentShow(int startIndex, int endIndex) {
    if (endIndex < 0) endIndex = startIndex;
    m_downloads.downloadShow(m_show.show(), startIndex, endIndex);
}

int Application::episodeDownloadState(int index) const {
    const auto playlist = m_show.playlist();
    const auto episode = playlist ? playlist->at(index) : nullptr;
    return episode ? m_downloads.episodeState(m_show.show().title, *episode) : DownloadQueue::NotDownloaded;
}

void Application::appendToPlaylists(int index, bool fromLibrary, bool play) {
    if (!fromLibrary) { appendResult(m_explorer, index, play); return; }

    auto entry = m_library.entryAt(index);
    if (!entry.valid) return;
    auto *provider = m_providers.byName(entry.provider);
    if (!provider) {
        reportMissingProvider(entry.provider, entry.title);
        return;
    }

    QSharedPointer<PlaylistItem> cached;
    if (m_show.show().link == entry.link)
        cached = m_show.playlist();

    m_playlist.appendShow(entry.title, entry.link, provider, cached, entry.watchState(), play);
}
