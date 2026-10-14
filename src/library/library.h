#pragma once
#include "app/async.h"
#include "providers/showdata.h"
#include <QAbstractListModel>
#include <QSortFilterProxyModel>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QtConcurrent/QtConcurrentRun>
#include <QFutureWatcher>
#include <QVariantList>
#include <QHash>
#include <qqmlintegration.h>
#include <QRegularExpression>

class HistoryModel : public QAbstractListModel {
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
public:
    enum Role { Link = Qt::UserRole, Title, Cover, Episode, Total, Progress, ProviderName, EpisodeTitle, PlayedAt, SearchText };
    struct Row {
        QString link, title, cover, provider;
        int episode = 0;
        int total = 0;
        double progress = 0;
        QString episodeTitle;
        qint64 playedAt = 0;
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
    double progress = 0.0;
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
    Q_PROPERTY(bool searchAllLists READ searchAllLists WRITE setSearchAllLists NOTIFY searchAllListsChanged)
    Q_PROPERTY(int unseenNotifications READ unseenNotifications NOTIFY notificationsChanged)
    Q_PROPERTY(QVariantMap syncOutboxCounts READ syncOutboxCounts NOTIFY syncOutboxChanged)
public:
    enum LibraryType { Watching, Planned, Paused, Dropped, Completed };
    enum Role { Title = Qt::UserRole, Cover, UnwatchedEpisodes, ShowType, ProviderName, Type, TotalEpisodes };

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
    Q_INVOKABLE QVariantList clearHistory();
    Q_INVOKABLE QVariantList removeFromHistory(const QString &link);

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

    struct OutboxEntry {
        QString episodeLink;
        double  seconds = 0;
        double  duration = 0;
        qint64  queuedAt = 0;
    };
    void queueSyncOutbox(const QString &episodeLink, const QString &service, double seconds, double duration);
    QList<OutboxEntry> syncOutbox(const QString &service) const;
    void removeFromSyncOutbox(const QString &service, const OutboxEntry &entry);
    Q_INVOKABLE void clearSyncOutbox(const QString &service);
    QVariantMap syncOutboxCounts() const;

    void recordHistory(const QString &link, int lastWatchedIndex, const QString &episodeTitle = {});

    void cacheHistoryMeta(const QString &link, const QString &title, const QString &cover,
                          const QString &provider, int total);

    LibraryEntry historyEntry(const QString &link) const;

    QList<QPair<QString, double>> localFolderProgress(const QString &folder) const;
    void restoreEpisodeProgress(const QSharedPointer<PlaylistItem> &playlist) const;
    Q_INVOKABLE void refreshAllUnwatched() { fetchUnwatchedEpisodes(-1, true); }
    void updateLocalProgress(const QString &path, const QString &folder, double progress);
    Q_INVOKABLE QVariantList watchedFilesUnder(const QString &folder) const;
    Q_INVOKABLE QVariantMap watchStats() const;
    Q_INVOKABLE QVariantMap note(const QString &link) const;
    Q_INVOKABLE void setNote(const QString &link, int rating, const QString &text);
    Q_INVOKABLE int trashFiles(const QStringList &paths);
    void forgetLocalProgress(const QStringList &paths);

    Q_INVOKABLE bool add(const ShowData &show, int libraryType);
    Q_INVOKABLE QVariantList removeAt(int index, int libraryType = -1);
    Q_INVOKABLE void remove(const QString &link);
    Q_INVOKABLE void restoreRows(const QString &table, const QVariantList &rows);
    Q_INVOKABLE void move(int from, int to);
    Q_INVOKABLE void changeLibraryTypeAt(int index, int newLibraryType, int oldLibraryType = -1);
    void changeLibraryType(const QString &link, int newLibraryType);
    Q_INVOKABLE void cycleDisplayLibraryType() { setDisplayLibraryType((m_displayLibraryType + 1) % (Completed + 1)); }

    Q_INVOKABLE void updateProgress(const QString &link, int lastWatchedIndex, double progress);
    void updateShowCover(const QString &link, const QString &cover);

    Q_INVOKABLE void fetchUnwatchedEpisodes(int libraryType, bool force = false);
    void applyEpisodeCounts(const QList<QPair<QString, int>> &counts);
    void applyAiringTimes(const QList<QPair<QString, qint64>> &times);
    Q_INVOKABLE QVariantList airingSchedule() const;

    int displayLibraryType() const { return m_displayLibraryType; }
    bool searchAllLists() const { return m_allLists; }
    void setSearchAllLists(bool on);
    Q_INVOKABLE QVariantList findShows(const QString &text, int limit = 5) const;

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
    QHash<int, qint64> m_lastFetchMs;

    struct HistoryMeta { QString title, cover, provider; int total = 0; };
    QHash<QString, HistoryMeta> m_historyMeta;
    HistoryModel m_history;
    HistoryFilter m_historyView;
    double m_watchedFraction = 0.8;
    CancelToken m_cancel;
};

class LibraryProxyModel : public QSortFilterProxyModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by Application; QML uses it for SortMode.")
    Q_PROPERTY(QString titleFilter              READ titleFilter              WRITE setTitleFilter              NOTIFY titleFilterChanged)
    Q_PROPERTY(int     typeFilter               READ typeFilter               WRITE setTypeFilter               NOTIFY typeFilterChanged)
    Q_PROPERTY(bool    hasUnwatchedEpisodesOnly READ hasUnwatchedEpisodesOnly WRITE setHasUnwatchedEpisodesOnly NOTIFY hasUnwatchedEpisodesOnlyChanged)
    Q_PROPERTY(bool    caseSensitive            READ caseSensitive            WRITE setCaseSensitive            NOTIFY caseSensitiveChanged)
    Q_PROPERTY(bool    useRegex                 READ useRegex                 WRITE setUseRegex                 NOTIFY useRegexChanged)
    Q_PROPERTY(int     sortMode                 READ sortMode                 WRITE setSortMode                 NOTIFY sortModeChanged)

public:
    enum SortMode { Manual, TitleAscending, MostUnwatched };
    Q_ENUM(SortMode)

    explicit LibraryProxyModel(QObject *parent = nullptr)
        : QSortFilterProxyModel(parent) { setDynamicSortFilter(true); }

    QString titleFilter() const { return m_titleFilter; }
    void setTitleFilter(const QString &filter);

    bool useRegex() const { return m_useRegex; }
    void setUseRegex(bool enabled);

    bool caseSensitive() const { return m_caseSensitive; }
    void setCaseSensitive(bool sensitive);

    int typeFilter() const { return m_typeFilter; }
    void setTypeFilter(int type);

    bool hasUnwatchedEpisodesOnly() const { return m_hasUnwatchedEpisodesOnly; }
    void setHasUnwatchedEpisodesOnly(bool enabled);

    int  sortMode() const { return m_sortMode; }
    void setSortMode(int mode);

    Q_INVOKABLE int mapToAbsoluteIndex(int proxyIndex) const {
        return mapToSource(index(proxyIndex, 0)).row();
    }

    void refreshFilter() {
        beginFilterChange();
        endFilterChange();
    }

signals:
    void titleFilterChanged();
    void typeFilterChanged();
    void useRegexChanged();
    void caseSensitiveChanged();
    void hasUnwatchedEpisodesOnlyChanged();
    void sortModeChanged();

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;
    bool lessThan(const QModelIndex &left, const QModelIndex &right) const override;

private:
    void rebuildRegex();

    QString m_titleFilter;
    QRegularExpression m_titleRegex;
    bool m_useRegex                 = false;
    bool m_caseSensitive            = false;
    bool m_hasUnwatchedEpisodesOnly = savedUnwatchedOnly();
    static bool savedUnwatchedOnly();
    int  m_typeFilter               = 0;
    int  m_sortMode                 = Manual;
};
