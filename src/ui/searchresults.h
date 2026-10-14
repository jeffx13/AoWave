#pragma once
#include <QAbstractListModel>
#include <QFutureWatcher>
#include "app/async.h"
#include "net/client.h"
#include "providers/showdata.h"
#include <qqmlintegration.h>

class SearchResults : public QAbstractListModel
{
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(bool isLoading READ isLoading NOTIFY isLoadingChanged)
    Q_PROPERTY(bool isLoadingMore READ isLoadingMore NOTIFY isLoadingChanged)
    Q_PROPERTY(int  count     READ count     NOTIFY countChanged)
    Q_PROPERTY(bool ran       READ ran       NOTIFY countChanged)
    Q_PROPERTY(QString failure READ failure  NOTIFY countChanged)
    Q_PROPERTY(double listAspect READ listAspect NOTIFY countChanged)
    Q_PROPERTY(bool listSubtitles READ listSubtitles NOTIFY countChanged)
public:
    enum { TitleRole = Qt::UserRole, CoverRole, LinkRole, LatestTxtRole, CoverAspectRole, SubtitleRole };

    explicit SearchResults(QObject *parent = nullptr);
    ~SearchResults() {
        m_cancel.cancel();
        m_runs.waitAll("SearchResults search");
    }

    void search(const QString &query, int page, int type, Provider *provider);
    void latest(int page, int type, Provider *provider);
    void popular(int page, int type, Provider *provider);
    void filtered(const QString &query, int page, int type, Provider *provider,
                  const QVariantMap &filters, bool latest);

    bool isLoading() const { return m_watcher.isRunning(); }
    bool isLoadingMore() const { return m_watcher.isRunning() && m_currentPage > 1; }
    bool ran() const { return m_ran; }
    QString failure() const { return m_failure; }
    double listAspect() const { return m_list.isEmpty() ? 0 : m_list.first().coverAspect; }
    bool listSubtitles() const { return !m_list.isEmpty() && !m_list.first().subtitle.isEmpty(); }

    Q_INVOKABLE bool canFetchMore() const { return !m_watcher.isRunning() && m_hasMore; }
    Q_INVOKABLE void fetchMore();
    Q_INVOKABLE void reload();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void reset() { beginResetModel(); endResetModel(); }

    ShowData resultAt(int index) const { return m_list.value(index); }
    int count() const { return m_list.count(); }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_SIGNAL void countChanged(int count);
    Q_SIGNAL void isLoadingChanged();

private:
    // Captures values only, so a superseded run can finish after this object is gone.
    using SearchFunc = std::function<QList<ShowData>(Client *client, int page)>;
    void runSearch(int page, SearchFunc &&func);
    void startPage(int page);
    void onSearchFinished();
    void failed(const QString &message, const QString &header);
    void setResults(QList<ShowData> results);

    struct Page {
        QList<ShowData> shows;
        QString error;
    };
    CancelToken m_cancel;
    QFutureWatcher<Page> m_watcher;
    RunSet m_runs;   // superseded runs still hold a provider pointer
    QList<ShowData> m_list;
    SearchFunc m_lastSearch;
    bool m_hasMore = false;
    bool m_ran = false;
    QString m_failure;
    int m_currentPage = 1;
};
