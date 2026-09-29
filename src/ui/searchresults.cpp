#include "ui/searchresults.h"
#include "shows/showprovider.h"
#include "core/appshell.h"
#include "core/logger.h"
#include <QtConcurrent/QtConcurrentRun>
#include "core/exception.h"

SearchResults::SearchResults(QObject *parent)
    : QAbstractListModel(parent)
{
    connect(&m_watcher, &QFutureWatcher<Page>::finished,
            this, &SearchResults::onSearchFinished);
    connect(&m_watcher, &QFutureWatcher<Page>::started,
            this, &SearchResults::isLoadingChanged);
    connect(&m_watcher, &QFutureWatcher<Page>::finished,
            this, &SearchResults::isLoadingChanged);
}

int SearchResults::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : m_list.count();
}

QVariant SearchResults::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= m_list.size()) return {};
    const ShowData &show = m_list.at(index.row());
    switch (role) {
    case TitleRole:     return show.title;
    case CoverRole:     return show.coverUrl;
    case LinkRole:      return show.link;
    case LatestTxtRole: return show.latestTxt;
    default:            return {};
    }
}

QHash<int, QByteArray> SearchResults::roleNames() const {
    return {
        {TitleRole,     "title"},
        {CoverRole,     "cover"},
        {LinkRole,      "link"},
        {LatestTxtRole, "latestTxt"},
    };
}

void SearchResults::onSearchFinished() {
    if (m_cancel.isCancelled() || !m_watcher.future().isValid()) return;

    Page page;
    try {
        page = m_watcher.result();
    } catch (const AppException &ex) {
        failed(ex.message(), tr("%1 Error").arg(ex.header()));
        return;
    } catch (const std::exception &ex) {
        failed(QString::fromUtf8(ex.what()), tr("Search failed"));
        return;
    } catch (...) {
        failed(tr("Something went wrong"), tr("Search failed"));
        return;
    }
    if (page.shows.isEmpty() && !page.error.isEmpty()) {
        failed(page.error, tr("Search failed"));
        return;
    }
    m_hasMore = !page.shows.isEmpty();
    m_failure.clear();
    setResults(std::move(page.shows));
}

void SearchResults::setResults(QList<ShowData> results) {
    if (m_currentPage > 1) {
        if (results.isEmpty()) return;
        const int first = int(m_list.count());
        beginInsertRows(QModelIndex(), first, first + int(results.count()) - 1);
        m_list.append(std::move(results));
        endInsertRows();
    } else {
        m_ran = true;
        beginResetModel();
        m_list = std::move(results);
        endResetModel();
    }
    emit countChanged(int(m_list.count()));
}

// A later page failing is where the list ends: the page the user asked for is already showing.
void SearchResults::failed(const QString &message, const QString &header) {
    m_hasMore = false;
    if (m_currentPage > 1) {
        logWarn() << "Search" << "page" << m_currentPage << "failed:" << message;
        return;
    }
    m_failure = message;
    setResults({});
    AppShell::instance().reportError(message, header);
}

void SearchResults::startPage(int page) {
    m_cancel.cancel();
    m_cancel = CancelToken{};
    m_currentPage = page;
    m_watcher.setFuture(m_runs.add(QtConcurrent::run([search = m_lastSearch, page, cancel = m_cancel]() {
        Client client(cancel);
        Page result{search(&client, page), {}};
        if (result.shows.isEmpty()) result.error = client.lastError();
        return result;
    })));
}

void SearchResults::runSearch(int page, SearchFunc &&func) {
    m_lastSearch = std::move(func);
    startPage(page);
}

void SearchResults::search(const QString &query, int page, int type, ShowProvider *provider) {
    runSearch(page, [query, type, provider](Client *client, int at) {
        return provider->search(client, query, at, type);
    });
}

void SearchResults::latest(int page, int type, ShowProvider *provider) {
    runSearch(page, [type, provider](Client *client, int at) { return provider->latest(client, at, type); });
}

void SearchResults::popular(int page, int type, ShowProvider *provider) {
    runSearch(page, [type, provider](Client *client, int at) { return provider->popular(client, at, type); });
}

void SearchResults::filtered(const QString &query, int page, int type, ShowProvider *provider,
                             const QVariantMap &filters, bool latest) {
    runSearch(page, [query, type, provider, filters, latest](Client *client, int at) {
        return provider->filtered(client, query, at, type, filters, latest);
    });
}

void SearchResults::cancel() {
    if (m_watcher.isRunning()) {
        logWarn() << "Search" << "Cancelling operation";
        m_cancel.cancel();
    }
}

void SearchResults::fetchMore() {
    if (!canFetchMore()) return;
    m_hasMore = false;   // re-armed by a page that returns something
    startPage(m_currentPage + 1);
}

void SearchResults::reload() {
    if (m_lastSearch) startPage(1);
}
