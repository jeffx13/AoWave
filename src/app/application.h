#pragma once

#include <QObject>
#include <QGuiApplication>
#include <QClipboard>
#include <QFuture>
#include <QPointer>
#include <QWindow>
#include "system/platform.h"
#include "ui/shortcuts.h"

#include "app/logger.h"
#include "app/qmlsingleton.h"
#include "system/discordpresence.h"
#include "app/settings.h"
#include "library/downloads/downloadqueue.h"
#include "library/library.h"
#include "media/playlist/playlist.h"
#include "media/player/skiptimes.h"
#include "providers/providerlist.h"
#include "library/trackers/tracker.h"
#include "ui/searchresults.h"
#include "ui/currentshow.h"
#include "media/player/subtitlesearch.h"

class Application : public QObject
{
    Q_OBJECT
    Q_PROPERTY(ProviderList      *providers      READ providers      CONSTANT)
    Q_PROPERTY(TrackerList       *trackers       READ trackers       CONSTANT)
    Q_PROPERTY(CurrentShow       *show           READ show           CONSTANT)
    Q_PROPERTY(SearchResults     *explorer       READ explorer       CONSTANT)
    Q_PROPERTY(SearchResults     *migrateSearch  READ migrateSearch  CONSTANT)
    Q_PROPERTY(Library           *library        READ library        CONSTANT)
    Q_PROPERTY(LibraryProxyModel *libraryModel   READ libraryModel   CONSTANT)
    Q_PROPERTY(Playlist          *playlist       READ playlist       CONSTANT)
    Q_PROPERTY(SkipTimes         *skipTimes      READ skipTimes      CONSTANT)
    Q_PROPERTY(SubtitleSearch    *subtitleSearch READ subtitleSearch CONSTANT)
    Q_PROPERTY(DownloadQueue     *downloads      READ downloads      CONSTANT)
    Q_PROPERTY(LogListModel      *logList        READ logList        CONSTANT)
    Q_PROPERTY(LogFilterModel    *logView        READ logView        CONSTANT)
    Q_PROPERTY(Settings          *settings       READ settings       CONSTANT)
    Q_PROPERTY(Shortcuts         *shortcuts      READ shortcuts      CONSTANT)
    Q_PROPERTY(bool quiet READ quiet CONSTANT)
    Q_PROPERTY(bool openWith READ openWith WRITE setOpenWith NOTIFY openWithChanged)
    Q_PROPERTY(QString updateVersion READ updateVersion NOTIFY updateChanged)
    Q_PROPERTY(QString updateUrl READ updateUrl NOTIFY updateChanged)
    Q_PROPERTY(QString ytdlpStatus READ ytdlpStatus NOTIFY ytdlpStatusChanged)
    Q_PROPERTY(bool ytdlpBusy READ ytdlpBusy NOTIFY ytdlpStatusChanged)
    Q_PROPERTY(QString jsRuntime READ jsRuntime NOTIFY jsRuntimeChanged)
    Q_PROPERTY(bool jsRuntimeBusy READ jsRuntimeBusy NOTIFY jsRuntimeChanged)

public:
    static bool isNewerVersion(const QString &latest, const QString &current);
    void attachWindow(QWindow *window);
    static bool quiet() { return Platform::quiet(); }
    void openArgument(const QString &argument);
    explicit Application(const QString &launchPath);
    ~Application();
    Application(const Application &) = delete;
    Application &operator=(const Application &) = delete;

    Q_INVOKABLE void search(const QString &query, const QVariantMap &filters = {});
    Q_INVOKABLE void browse(bool latest, const QVariantMap &filters = {});
    Q_INVOKABLE void loadShow(int index, bool fromLibrary);
    Q_INVOKABLE void reloadShow();
    Q_INVOKABLE void playFromEpisodeList(int index, bool append);
    Q_INVOKABLE void continueWatching();
    Q_INVOKABLE void addToLibrary(int index, int libraryType);
    Q_INVOKABLE void appendToPlaylists(int index, bool fromLibrary, bool play = false);
    Q_INVOKABLE void resumeFromHistory(const QString &link);
    Q_INVOKABLE void downloadCurrentShow(int startIndex, int endIndex = -1);
    Q_INVOKABLE int episodeDownloadState(int index) const;
    Q_INVOKABLE void copyToClipboard(const QString &text) { QGuiApplication::clipboard()->setText(text); }
    Q_INVOKABLE double coveredFraction(QWindow *window) const {
        return window ? Platform::coveredFraction(window->winId()) : 0.0;
    }
    Q_INVOKABLE void setPipWindow(QWindow *window, bool pip) {
        if (!window) return;
        Platform::setTopmost(window->winId(), pip);
        Platform::setRoundedCorners(window->winId(), pip);
    }
    Q_INVOKABLE QString showUrl(int index, bool fromLibrary) const;
    Q_INVOKABLE void focusPreviousWindow(QWindow *window) {
        if (window) Platform::focusPreviousWindow(window->winId());
    }
    Q_INVOKABLE void restoreWithoutFocus(QWindow *window) {
        if (window) Platform::restoreWithoutFocus(window->winId());
    }

    Q_INVOKABLE void searchOnProvider(const QString &providerName, const QString &query);
    Q_INVOKABLE void migrateShow(const QString &oldLink, int resultIndex, int resumeEpisode);
    Q_INVOKABLE void openShowInfo(const QString &link, const QString &providerName, const QString &title);

    bool openWith() const { return Platform::openWithRegistered(); }
    void setOpenWith(bool on);
    Q_SIGNAL void openWithChanged();

    QString updateVersion() const { return m_updateVersion; }
    QString updateUrl() const { return m_updateUrl; }
    Q_INVOKABLE void dismissUpdate();
    Q_SIGNAL void updateChanged();

    QString ytdlpStatus() const { return m_ytdlpStatus; }
    bool ytdlpBusy() const { return m_ytdlpBusy; }
    Q_INVOKABLE void refreshYtdlpVersion();
    Q_INVOKABLE void updateYtdlp();
    QString jsRuntime() const { return m_jsRuntime; }
    bool jsRuntimeBusy() const { return m_jsRuntimeBusy; }
    Q_INVOKABLE void findJsRuntime() { applyJsRuntime(QStringLiteral("find")); }
    Q_INVOKABLE void useJsRuntime(const QUrl &file);
    Q_SIGNAL void jsRuntimeChanged();
    Q_SIGNAL void ytdlpStatusChanged();

    void setFont(const QString &fontPath);

private:
    void checkForUpdates();
    void announceNewEpisodes(const QVariantList &shows);
    void updateJumpList();
    void runYtdlp(const QStringList &arguments, std::function<void(bool ok, const QString &output)> done);
    void applyJsRuntime(const QString &how);
    QPointer<QWindow> m_window;
    QList<Platform::JumpItem> m_jumpList;

    ProviderList      *providers()      { return &m_providers; }
    TrackerList       *trackers()       { return &m_trackers; }
    CurrentShow       *show()           { return &m_show; }
    SearchResults     *explorer()       { return &m_explorer; }
    SearchResults     *migrateSearch()  { return &m_migrateSearch; }
    Library           *library()        { return &m_library; }
    LibraryProxyModel *libraryModel()   { return &m_libraryProxyModel; }
    Playlist          *playlist()       { return &m_playlist; }
    SkipTimes         *skipTimes()      { return &m_skipTimes; }
    SubtitleSearch    *subtitleSearch() { return &m_subtitleSearch; }
    DownloadQueue     *downloads()      { return &m_downloads; }
    LogListModel      *logList()        { return &Log::logListModel; }
    LogFilterModel    *logView()        { return &m_logView; }
    Settings          *settings()       { return &Settings::instance(); }
    Shortcuts         *shortcuts()      { return &m_shortcuts; }

    void loadResult(SearchResults &src, int index);
    void appendResult(SearchResults &src, int index, bool play);
    void openEntry(const LibraryEntry &entry, bool autoResume);
    void reportMissingProvider(const QString &provider, const QString &title);

    // Destroyed last: it outlives the models whose workers are still in flight.
    ProviderList        m_providers{this};
    TrackerList         m_trackers{this};

    SearchResults       m_explorer;
    SearchResults       m_migrateSearch;
    bool                m_pendingAutoResume = false;

    // Before the playlist, which writes to it until its own workers have stopped.
    Library             m_library;
    LibraryProxyModel   m_libraryProxyModel;

    Playlist            m_playlist;

    DownloadQueue       m_downloads;

    CurrentShow         m_show{this};
    SkipTimes           m_skipTimes{this};
    SubtitleSearch      m_subtitleSearch{this};
    DiscordPresence     m_discordPresence{this};
    LogFilterModel      m_logView{this};

    // migrate() runs off-thread against a provider this object owns.
    CancelToken         m_migrateCancel;
    QFuture<void>       m_migrateFuture;

    CancelToken         m_updateCancel;
    Shortcuts           m_shortcuts;
    QString             m_updateVersion;
    QString             m_updateUrl;
    QString             m_ytdlpStatus;
    bool                m_ytdlpBusy = false;
    QString             m_jsRuntime;
    bool                m_jsRuntimeBusy = false;
    QFuture<void>       m_updateFuture;
};

DECLARE_QML_NAMED_SINGLETON(Application, App)
