#include "ui/searchresults.h"
#include "providers/provider.h"
#include "app/mainwindow.h"
#include "app/logger.h"
#include <QtConcurrent/QtConcurrentRun>
#include "app/exception.h"

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
    case CoverAspectRole: return show.coverAspect;
    case SubtitleRole:  return show.subtitle;
    default:            return {};
    }
}

QHash<int, QByteArray> SearchResults::roleNames() const {
    return {
        {TitleRole,     "title"},
        {CoverRole,     "cover"},
        {LinkRole,      "link"},
        {LatestTxtRole, "latestTxt"},
        {CoverAspectRole, "coverAspect"},
        {SubtitleRole,  "subtitle"},
    };
}

void SearchResults::onSearchFinished() {
    if (m_cancel.isCancelled() || !m_watcher.future().isValid()) return;

    Page page;
    try {
        page = m_watcher.result();
    } catch (const AppException &ex) {
        failed(ex.message(), tr("%1 error").arg(ex.header()));
        return;
    } catch (const std::exception &ex) {
        failed(QString::fromUtf8(ex.what()), tr("Search failed"));
        return;
    } catch (...) {
        failed(tr("Unknown error."), tr("Search failed"));
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

void SearchResults::failed(const QString &message, const QString &header) {
    m_hasMore = false;
    if (m_currentPage > 1) {
        logWarn() << "Library" << QStringLiteral("search:") << "page" << m_currentPage << "failed:" << message;
        return;
    }
    m_failure = message;
    setResults({});
    MainWindow::instance().reportError(message, header);
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

void SearchResults::search(const QString &query, int page, int type, Provider *provider) {
    runSearch(page, [query, type, provider](Client *client, int at) {
        client->setProvider(provider->name());
        return provider->search(client, query, at, type);
    });
}

void SearchResults::latest(int page, int type, Provider *provider) {
    runSearch(page, [type, provider](Client *client, int at) {
        client->setProvider(provider->name());
        return provider->latest(client, at, type);
    });
}

void SearchResults::popular(int page, int type, Provider *provider) {
    runSearch(page, [type, provider](Client *client, int at) {
        client->setProvider(provider->name());
        return provider->popular(client, at, type);
    });
}

void SearchResults::filtered(const QString &query, int page, int type, Provider *provider,
                             const QVariantMap &filters, bool latest) {
    runSearch(page, [query, type, provider, filters, latest](Client *client, int at) {
        client->setProvider(provider->name());
        FilterBrowse *browse = provider->filters();
        return browse ? browse->filtered(client, query, at, type, filters, latest) : QList<ShowData>();
    });
}

void SearchResults::cancel() {
    if (m_watcher.isRunning()) {
        logWarn() << "Library" << QStringLiteral("search:") << "Cancelling operation";
        m_cancel.cancel();
    }
}

void SearchResults::fetchMore() {
    if (!canFetchMore()) return;
    m_hasMore = false;
    startPage(m_currentPage + 1);
}

void SearchResults::reload() {
    if (m_lastSearch) startPage(1);
}
