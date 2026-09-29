#pragma once
#include <QAbstractListModel>
#include <QSortFilterProxyModel>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QtConcurrent/QtConcurrentRun>
#include <QFutureWatcher>
#include <QVariantList>
#include <QHash>
#include "net/canceltoken.h"
#include "shows/showdata.h"
#include <qqmlintegration.h>

class HistoryModel : public QAbstractListModel {
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
public:
    // SearchText, the show and episode titles together, is what the History page filters on.
    enum Role { Link = Qt::UserRole, Title, Cover, Episode, Total, Progress, Provider, EpisodeTitle, PlayedAt, SearchText };
    struct Row {
        QString link, title, cover, provider;
        int episode = 0;
        int total = 0;
        double progress = 0;
        QString episodeTitle;   // empty for an untitled episode, or one played before it was kept
        qint64 playedAt = 0;    // unix seconds
        bool operator==(const Row &) const = default;
    };

    using QAbstractListModel::QAbstractListModel;
    void setRows(QList<Row> rows);
    void removeLink(const QString &link);
    void clear();
    Q_INVOKABLE QString linkAt(int row) const;
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void countChanged();

private:
    QList<Row> m_rows;
};

// The History page's rows: those whose show or episode title holds `filterText`. A view of its
// own, since the jump list and Continue watching read the model's first rows.
class HistoryFilter : public QSortFilterProxyModel {
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(QString filterText READ filterText WRITE setFilterText NOTIFY filterTextChanged)
public:
    explicit HistoryFilter(QObject *parent = nullptr) : QSortFilterProxyModel(parent) {
        setFilterRole(HistoryModel::SearchText);
        setFilterCaseSensitivity(Qt::CaseInsensitive);
    }
    Q_INVOKABLE QString linkAt(int row) const { return data(index(row, 0), HistoryModel::Link).toString(); }
    QString filterText() const { return m_filterText; }
    void setFilterText(const QString &text) {
        if (text == m_filterText) return;
        m_filterText = text;
        setFilterFixedString(text.trimmed());
        emit filterTextChanged();
    }
signals:
    void filterTextChanged();
private:
    QString m_filterText;
};

struct LibraryEntry {
    Q_GADGET
    QML_ANONYMOUS
    Q_PROPERTY(QString title MEMBER title CONSTANT)
    Q_PROPERTY(QString link MEMBER link CONSTANT)
    Q_PROPERTY(QString cover MEMBER cover CONSTANT)
    Q_PROPERTY(QString provider MEMBER provider CONSTANT)
    Q_PROPERTY(int libraryType MEMBER libraryType CONSTANT)
    Q_PROPERTY(int lastWatchedIndex MEMBER lastWatchedIndex CONSTANT)
    Q_PROPERTY(double progress MEMBER progress CONSTANT)
    Q_PROPERTY(int totalEpisodes MEMBER totalEpisodes CONSTANT)
    Q_PROPERTY(bool valid MEMBER valid CONSTANT)
public:
    QString title;
    QString link;
    QString cover;
    QString provider;
    int libraryType = -1;
    int lastWatchedIndex = -1;
    double progress = 0.0;       // 0..1
    int totalEpisodes = 0;
    int showType = 0;
    bool valid = false;

    ShowData::WatchState watchState() const {
        ShowData::WatchState watch;
        watch.libraryType      = libraryType;
        watch.lastWatchedIndex = lastWatchedIndex;
        watch.progress         = progress;
        return watch;
    }
};

class Library : public QAbstractListModel
{
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(int libraryType READ displayLibraryType WRITE setDisplayLibraryType NOTIFY libraryTypeChanged)
    Q_PROPERTY(HistoryModel *historyModel READ historyModel CONSTANT)
    Q_PROPERTY(HistoryFilter *historyView READ historyView CONSTANT)
    Q_PROPERTY(int displayedCount READ rowCount NOTIFY displayedCountChanged)
    // Every list at once, for a search; rows then keep their own list in `libraryType`.
    Q_PROPERTY(bool searchAllLists READ searchAllLists WRITE setSearchAllLists NOTIFY searchAllListsChanged)
    Q_PROPERTY(int unseenNotifications READ unseenNotifications NOTIFY notificationsChanged)
    // service -> pushes waiting to be sent
    Q_PROPERTY(QVariantMap syncOutboxCounts READ syncOutboxCounts NOTIFY syncOutboxChanged)
public:
    enum LibraryType { Watching, Planned, Paused, Dropped, Completed };
    enum Role { Title = Qt::UserRole, Cover, UnwatchedEpisodes, ShowType, Provider, Type, TotalEpisodes };

    explicit Library(QObject *parent = nullptr);
    ~Library();

    Q_INVOKABLE int  count(int libraryType = -1) const;
    Q_INVOKABLE int  libraryTypeOf(const QString &link) const;
    bool linkExists(const QString &link) const;

    Q_INVOKABLE LibraryEntry entryAt(int index) const;
    Q_INVOKABLE LibraryEntry entryForLink(const QString &link) const;

    bool migrate(const QString &oldLink, const QString &newLink, const QString &title,
                 const QString &cover, const QString &provider, int showType,
                 int lastWatchedIndex, int totalEpisodes);

    HistoryModel *historyModel() { return &m_history; }
    HistoryFilter *historyView() { return &m_historyView; }
    // Each hands back the rows it deleted, for restoreRows to undo it.
    Q_INVOKABLE QVariantList clearHistory();
    Q_INVOKABLE QVariantList removeFromHistory(const QString &link);

    // Base of Playlist's three-way merge: the position both sides last agreed on.
    struct SyncState {
        double basePos = 0;
        qint64 baseServerAt = 0;
        double pushedPos = 0;
        bool   valid = false;
    };
    Q_INVOKABLE QVariantMap trackerLink(const QString &showLink, const QString &service) const;
    Q_INVOKABLE void setTrackerLink(const QString &showLink, const QString &service,
                                    const QString &remoteId, const QString &status,
                                    double score, int progress);
    Q_INVOKABLE void clearTrackerLink(const QString &showLink, const QString &service);

    SyncState syncState(const QString &episodeLink, const QString &service) const;
    void setSyncState(const QString &episodeLink, const QString &service, const SyncState &state);

    // A push that failed, replayed before the next one.
    struct OutboxEntry {
        QString episodeLink;
        double  seconds = 0;
        double  duration = 0;
        qint64  queuedAt = 0;
    };
    void queueSyncOutbox(const QString &episodeLink, const QString &service, double seconds, double duration);
    // Oldest first, at most 50.
    QList<OutboxEntry> syncOutbox(const QString &service) const;
    // Matches queued_at too, so a push queued for the same episode since the read survives.
    void removeFromSyncOutbox(const QString &service, const OutboxEntry &entry);
    // For a service with nowhere to deliver to, or pushes the user gives up on.
    Q_INVOKABLE void clearSyncOutbox(const QString &service);
    QVariantMap syncOutboxCounts() const;

    void recordHistory(const QString &link, int lastWatchedIndex, const QString &episodeTitle = {});

    // Cover and title live in ShowData, so cache them: history covers shows outside the library.
    void cacheHistoryMeta(const QString &link, const QString &title, const QString &cover,
                          const QString &provider, int total);

    LibraryEntry historyEntry(const QString &link) const;

    // absolute path -> most recent first
    QList<QPair<QString, double>> localFolderProgress(const QString &folder) const;
    void restoreEpisodeProgress(const QSharedPointer<PlaylistItem> &playlist) const;
    Q_INVOKABLE void refreshAllUnwatched() { fetchUnwatchedEpisodes(-1, true); }
    void updateLocalProgress(const QString &path, const QString &folder, double progress);
    // Files under `folder` played past the watched mark and still there: {path, size}.
    Q_INVOKABLE QVariantList watchedFilesUnder(const QString &folder) const;
    // Provider episodes and local files alike: {episodesThisWeek, activeDays (of the last 30),
    // streak (days), watching, completed}.
    Q_INVOKABLE QVariantMap watchStats() const;
    // A personal rating (0 to 5) and note, for any show: {rating, text}.
    Q_INVOKABLE QVariantMap note(const QString &link) const;
    Q_INVOKABLE void setNote(const QString &link, int rating, const QString &text);
    // To the Recycle Bin, so a slip can be undone. How many moved.
    Q_INVOKABLE int trashFiles(const QStringList &paths);
    void forgetLocalProgress(const QStringList &paths);

    Q_INVOKABLE bool add(const ShowData &show, int libraryType);
    Q_INVOKABLE QVariantList removeAt(int index, int libraryType = -1);
    Q_INVOKABLE void remove(const QString &link);
    // Undo: puts rows of "shows" or "history" back exactly as they were, place in the list included.
    Q_INVOKABLE void restoreRows(const QString &table, const QVariantList &rows);
    Q_INVOKABLE void move(int from, int to);
    Q_INVOKABLE void changeLibraryTypeAt(int index, int newLibraryType, int oldLibraryType = -1);
    void changeLibraryType(const QString &link, int newLibraryType);
    Q_INVOKABLE void cycleDisplayLibraryType() { setDisplayLibraryType((m_displayLibraryType + 1) % (Completed + 1)); }

    Q_INVOKABLE void updateProgress(const QString &link, int lastWatchedIndex, double progress);
    void updateShowCover(const QString &link, const QString &cover);

    Q_INVOKABLE void fetchUnwatchedEpisodes(int libraryType, bool force = false);
    // Stores fetched totals, first reporting any show on the Watching list that grew.
    void applyEpisodeCounts(const QList<QPair<QString, int>> &counts);
    // link -> unix seconds, 0 once nothing is scheduled.
    void applyAiringTimes(const QList<QPair<QString, qint64>> &times);
    // Watching shows with an episode due, soonest first: {link, title, provider, episode, at}.
    Q_INVOKABLE QVariantList airingSchedule() const;

    int displayLibraryType() const { return m_displayLibraryType; }
    bool searchAllLists() const { return m_allLists; }
    void setSearchAllLists(bool on);
    // Library and history shows whose title has `text`, earliest match first:
    // [{title, link, provider, libraryType}], libraryType -1 for a history-only show.
    Q_INVOKABLE QVariantList findShows(const QString &text, int limit = 5) const;

    // The notification center: new episodes and finished downloads, newest first, the last 100
    // kept. [{id, kind ("episode" or "download"), title, message, link, provider, at, seen}]
    Q_INVOKABLE QVariantList notifications() const;
    Q_INVOKABLE void markNotificationsSeen();
    Q_INVOKABLE void clearNotifications();
    void addNotification(const QString &kind, const QString &title, const QString &message,
                         const QString &link, const QString &provider = {});
    int unseenNotifications() const;
    void setDisplayLibraryType(int newLibraryType);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void fetchedAllEpCounts();
    // One {title, link, total} map per show.
    void newEpisodesFound(const QVariantList &shows);
    void libraryTypeChanged();
    void libraryChanged();
    void displayedCountChanged();
    void searchAllListsChanged();
    void notificationsChanged();
    void syncOutboxChanged();

private:
    void reloadHistory();
    QVariantList rowsOf(const QString &table, const QString &link = {}) const;
    void initDatabase();
    void persistEpisodeCounts(const QList<QPair<QString, int>> &counts);
    int indexOf(const QString &link) const;
    QString linkAtIndex(int index, int libraryType) const;

    void refreshDisplayCache();
    static LibraryEntry entryFromQuery(const QSqlQuery &query);
    QList<LibraryEntry> m_displayCache;

    static constexpr int kNoPendingFetch = -2;
    static constexpr qint64 kFetchDebounceMs = 60'000;

    QSqlDatabase m_db;
    int m_displayLibraryType = Watching;
    bool m_allLists = false;
    QFutureWatcher<void> m_fetchWatcher;
    int  m_pendingFetchLibraryType = kNoPendingFetch;
    bool m_pendingFetchForced = false;
    QHash<int, qint64> m_lastFetchMs;   // library type -> epoch ms of last fetch

    struct HistoryMeta { QString title, cover, provider; int total = 0; };
    QHash<QString, HistoryMeta> m_historyMeta;   // link -> display metadata
    HistoryModel m_history;
    HistoryFilter m_historyView;
    double m_watchedFraction = 0.8;
    CancelToken m_cancel;
};
