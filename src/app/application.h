#pragma once

#include <QObject>
#include <QGuiApplication>
#include <QClipboard>
#include <QFuture>
#include <QPointer>
#include <QWindow>
#include "platform/platform.h"
#include "ui/shortcuts.h"

#include "core/logger.h"
#include "core/qmlsingleton.h"
#include "app/discordpresence.h"
#include "core/settings.h"
#include "downloads/downloadqueue.h"
#include "library/library.h"
#include "library/libraryproxymodel.h"
#include "media/playlist.h"
#include "media/skiptimes.h"
#include "shows/providerlist.h"
#include "trackers/trackerlist.h"
#include "ui/searchresults.h"
#include "ui/showdetails.h"
#include "ui/subtitlesearch.h"

class Application : public QObject
{
    Q_OBJECT
    Q_PROPERTY(ProviderList      *providers      READ providers      CONSTANT)
    Q_PROPERTY(TrackerList       *trackers       READ trackers       CONSTANT)
    Q_PROPERTY(ShowDetails       *show           READ show           CONSTANT)
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
    Q_PROPERTY(bool bilibiliSignedIn READ bilibiliSignedIn NOTIFY bilibiliSignedInChanged)
    Q_PROPERTY(QString bilibiliAccount READ bilibiliAccount NOTIFY bilibiliSignedInChanged)
    // Non-empty while a sign-in waits. The URL is bilibili's approval link; the QR is that link.
    Q_PROPERTY(QString bilibiliLoginQr READ bilibiliLoginQr NOTIFY bilibiliLoginStateChanged)
    Q_PROPERTY(QString bilibiliLoginStatus READ bilibiliLoginStatus NOTIFY bilibiliLoginStateChanged)
    // Read from the registry: the entries outlive the app, and may be another copy's.
    Q_PROPERTY(bool openWith READ openWith WRITE setOpenWith NOTIFY openWithChanged)
    // A newer release, until dismissed; empty otherwise.
    Q_PROPERTY(QString updateVersion READ updateVersion NOTIFY updateChanged)
    Q_PROPERTY(QString updateUrl READ updateUrl NOTIFY updateChanged)
    // yt-dlp's version, or how its update went.
    Q_PROPERTY(QString ytdlpStatus READ ytdlpStatus NOTIFY ytdlpStatusChanged)
    Q_PROPERTY(bool ytdlpBusy READ ytdlpBusy NOTIFY ytdlpStatusChanged)
    // The JavaScript runtime yt-dlp solves YouTube's challenges with, as Settings shows it; empty
    // when there is none.
    Q_PROPERTY(QString jsRuntime READ jsRuntime NOTIFY jsRuntimeChanged)
    Q_PROPERTY(bool jsRuntimeBusy READ jsRuntimeBusy NOTIFY jsRuntimeChanged)

public:
    void attachWindow(QWindow *window);
    // A file, folder or link handed over by a second launch.
    void openArgument(const QString &argument);
    explicit Application(const QString &launchPath);
    ~Application();
    Application(const Application &) = delete;
    Application &operator=(const Application &) = delete;

    // `filters`: {"genre": value, ...} from the provider's filterOptions; empty values are unset.
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
    // A DownloadQueue::EpisodeState, for the Info page's episode rows.
    Q_INVOKABLE int episodeDownloadState(int index) const;
    Q_INVOKABLE void copyToClipboard(const QString &text) { QGuiApplication::clipboard()->setText(text); }
    // Picture-in-picture's window: see Platform.
    Q_INVOKABLE double coveredFraction(QWindow *window) const {
        return window ? Platform::coveredFraction(window->winId()) : 0.0;
    }
    Q_INVOKABLE void setPipWindow(QWindow *window, bool pip) {
        if (!window) return;
        Platform::setTopmost(window->winId(), pip);
        Platform::setRoundedCorners(window->winId(), pip);
    }
    // The show's page on its site, for a search result or (fromLibrary) a library entry.
    // The show's own page on its site, or empty where a link cannot say it. -1: the open show.
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

    // An H5 page for the phone app, not one a desktop browser can complete.
    Q_INVOKABLE void bilibiliLogin();
    // For sending to a phone by hand.
    Q_INVOKABLE void copyBilibiliLoginLink() const;
    Q_INVOKABLE void bilibiliCancelLogin();
    // For a session copied out of a signed-in browser. False if the paste is unusable.
    Q_INVOKABLE bool bilibiliSignInWithCookies(const QString &pastedCookies);
    Q_INVOKABLE void bilibiliSignOut();
    // Empty until the nav endpoint has answered once.
    void refreshBilibiliAccount();
    bool bilibiliSignedIn() const;
    QString bilibiliAccount() const;
    QString bilibiliLoginQr() const { return m_loginQr; }
    QString bilibiliLoginStatus() const { return m_loginStatus; }
    Q_SIGNAL void bilibiliSignedInChanged();
    Q_SIGNAL void bilibiliLoginStateChanged();

    bool openWith() const { return Platform::openWithRegistered(); }
    void setOpenWith(bool on);
    Q_SIGNAL void openWithChanged();

    QString updateVersion() const { return m_updateVersion; }
    QString updateUrl() const { return m_updateUrl; }
    // Not offered again until a newer release.
    Q_INVOKABLE void dismissUpdate();
    Q_SIGNAL void updateChanged();

    QString ytdlpStatus() const { return m_ytdlpStatus; }
    bool ytdlpBusy() const { return m_ytdlpBusy; }
    Q_INVOKABLE void refreshYtdlpVersion();
    // yt-dlp -U: sites change faster than releases of this app.
    Q_INVOKABLE void updateYtdlp();
    QString jsRuntime() const { return m_jsRuntime; }
    bool jsRuntimeBusy() const { return m_jsRuntimeBusy; }
    // Looks again on PATH and in the usual install folders, and keeps what it finds.
    Q_INVOKABLE void findJsRuntime() { applyJsRuntime(QStringLiteral("find")); }
    // One picked by hand: deno.exe or node.exe.
    Q_INVOKABLE void useJsRuntime(const QUrl &file);
    Q_SIGNAL void jsRuntimeChanged();
    Q_SIGNAL void ytdlpStatusChanged();

    void setFont(const QString &fontPath);

private:
    void checkForUpdates();
    void announceNewEpisodes(const QVariantList &shows);
    void updateJumpList();
    void runYtdlp(const QStringList &arguments, std::function<void(bool ok, const QString &output)> done);
    // `how`: "saved" uses the saved runtime, finding one only if none is saved; "find" looks
    // again; otherwise it is a runtime to try ("node:<path>"). Hands the result to mpv.
    void applyJsRuntime(const QString &how);
    QPointer<QWindow> m_window;
    QList<Platform::JumpItem> m_jumpList;
    static bool isNewerVersion(const QString &latest, const QString &current);

    ProviderList      *providers()      { return &m_providers; }
    TrackerList       *trackers()       { return &m_trackers; }
    ShowDetails       *show()           { return &m_show; }
    SearchResults     *explorer()       { return &m_explorer; }
    SearchResults     *migrateSearch()  { return &m_migrateSearch; }
    Library           *library()        { return &m_library; }
    LibraryProxyModel *libraryModel()   { return &m_libraryProxyModel; }
    Playlist          *playlist()       { return &m_playlist; }
    SkipTimes         *skipTimes()      { return &m_skipTimes; }
    SubtitleSearch    *subtitleSearch() { return &m_subtitleSearch; }
    DownloadQueue     *downloads()      { return &m_downloads; }
    LogListModel      *logList()        { return &QLog::logListModel; }
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

    ShowDetails         m_show{this};
    SkipTimes           m_skipTimes{this};
    SubtitleSearch      m_subtitleSearch{this};
    DiscordPresence     m_discordPresence{this};
    LogFilterModel      m_logView{this};

    // migrate() runs off-thread against a provider this object owns.
    CancelToken         m_migrateCancel;
    QFuture<void>       m_migrateFuture;

    // Must not keep the process alive after the window closes.
    CancelToken         m_loginCancel;
    QFuture<void>       m_loginFuture;
    QFuture<void>       m_accountFuture;
    QString             m_loginUrl;
    QString             m_loginQr;
    QString             m_loginStatus;
    // One place decides what a finished sign-in does.
    void adoptBilibiliCookies(const QMap<QString, QString> &cookies);
    void abandonBilibiliLogin();
    void setBilibiliLoginState(const QString &url, const QString &status);

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
