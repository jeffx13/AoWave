#include "ui/currentshow.h"
#include "app/async.h"
#include "net/client.h"
#include "providers/showdata.h"
#include "providers/provider.h"
#include "app/mainwindow.h"
#include "app/logger.h"
#include "app/settings.h"
#include <QtConcurrent/QtConcurrentRun>
#include <cmath>

CurrentShow::CurrentShow(QObject *parent) : QObject(parent) {
    connect(&m_watcher, &QFutureWatcher<void>::finished, this, &CurrentShow::onLoadFinished);
    connect(&m_watcher, &QFutureWatcher<void>::started,  this, &CurrentShow::isLoadingChanged);
    connect(&m_watcher, &QFutureWatcher<void>::finished, this, &CurrentShow::isLoadingChanged);
}

CurrentShow::~CurrentShow() {
    m_cancel.cancel();
    waitFor(m_watcher, "CurrentShow load");
}

int CurrentShow::lastWatchedIndex() const {
    auto list = playlist();
    return list ? list->currentIndex() : -1;
}

int CurrentShow::episodeCount() const {
    auto list = playlist();
    return list ? list->episodeCount() : 0;
}

int CurrentShow::watchedCount() const {
    auto list = playlist();
    if (!list) return 0;
    const double watched = Settings::instance().watchedFraction();
    int count = 0;
    for (int i = 0; i < list->count(); ++i) {
        const auto episode = list->at(i);
        if (!episode->preview && (i < m_continueIndex || episode->progress() >= watched)) ++count;
    }
    return count;
}

static bool isPlaying(const PlaylistItem &list) {
    return list.parent() && list.parent()->currentIndex() == list.row();
}

void CurrentShow::setLastWatchedIndex(int index) {
    auto list = playlist();
    if (!list) return;

    if (isPlaying(*list)) return;
    if (!list->setCurrentIndex(index)) return;
    updateContinueEpisode();
    emit lastWatchedIndexChanged();
}

void CurrentShow::updateContinueEpisode() {
    m_episodes.refreshProgress();
    auto list = playlist();
    if (!list) { m_continueText.clear(); m_continueIndex = -1; return; }

    int idx = qMax(list->currentIndex(), 0);
    while (idx > 0 && list->at(idx)->preview) --idx;

    if (auto watched = list->at(idx);
        watched && watched->progress() >= Settings::instance().watchedFraction()) {
        int next = idx + 1;
        while (next < list->count() && list->at(next)->preview) ++next;
        if (next < list->count()) idx = next;
    }

    m_continueIndex = idx;
    auto episode = list->at(m_continueIndex);
    if (!episode) { m_continueText.clear(); return; }

    m_continueText = (m_continueIndex == 0 ? tr("Play %1") : tr("Continue from %1"))
                         .arg(episode->displayName.simplified());
}

void CurrentShow::cancel() {
    if (m_watcher.isRunning())
        m_cancel.cancel();
}

void CurrentShow::setShow(const ShowData &show, const ShowData::WatchState &watchState, bool navigate) {
    if (m_watcher.isRunning()) {
        m_pendingShow = show;
        m_pendingInfo = watchState;
        m_pendingNavigate = navigate;
        m_hasPending = true;
        m_cancel.cancel();
        return;
    }
    if (m_show.link == show.link) {
        if (navigate) MainWindow::instance().navigateTo(MainWindow::Page::Details);
        return;
    }
    // Fresh, not reset(): a reset would un-cancel the load this supersedes.
    m_cancel = CancelToken{};
    m_watcher.setFuture(QtConcurrent::run(&CurrentShow::load, this, show, watchState, navigate, m_cancel));
}

void CurrentShow::reload(const ShowData &show, const ShowData::WatchState &watchState) {
    if (m_watcher.isRunning()) return;
    m_cancel = CancelToken{};
    m_watcher.setFuture(QtConcurrent::run(&CurrentShow::load, this, show, watchState, false, m_cancel));
}

void CurrentShow::onLoadFinished() {
    if (!m_hasPending) return;
    m_hasPending = false;
    setShow(m_pendingShow, m_pendingInfo, m_pendingNavigate);
}

// Worker thread, on by-value copies including the token.
void CurrentShow::load(ShowData show, ShowData::WatchState watchState, bool navigate, CancelToken cancel) {
    auto list = watchState.playlist;
    const bool usingExistingPlaylist = (list != nullptr);

    bool success = false;
    QString failure;
    if (show.provider) {
        logInfo() << "Library" << "Loading" << show.title << "from" << show.provider->name() << "using" << show.link;
        Client client(cancel);
        client.setProvider(show.provider->name());
        try {
            success = show.provider->loadShow(&client, show);
            if (!success) failure = client.lastError();
        } catch (const std::exception &ex) {
            failure = QString::fromUtf8(ex.what());
        } catch (...) {
            failure = tr("Unknown error.");
        }
    }
    if (cancel.isCancelled()) return;
    if (!success) {
        logWarn() << "Library" << "Failed to load" << show.title << failure;
        const QString provider = show.provider ? show.provider->name() : tr("The provider");
        MainWindow::instance().reportError(
            failure.isEmpty()
                ? tr("%1 returned nothing for %2. The show may have been removed "
                     "from the site.").arg(provider, show.title)
                : tr("%1 could not load %2.\n\n%3").arg(provider, show.title, failure),
            tr("Could not load show"));
        return;
    }

    const auto fresh = show.playlist();
    if (!list)
        list = fresh;

    bool shouldReverse = false;
    if (usingExistingPlaylist) {
        shouldReverse = list->currentIndex() > 0;
        show.setPlaylist(list);
    } else if (list && list->isValidIndex(watchState.lastWatchedIndex)) {
        list->setCurrentIndex(watchState.lastWatchedIndex);
        if (auto item = list->currentItem())
            item->setProgress(watchState.progress);
        shouldReverse = watchState.lastWatchedIndex > 0;
    }

    logInfo() << "Library" << "Loaded" << show.title;

    QMetaObject::invokeMethod(this, [this, show = std::move(show), list, fresh, shouldReverse, navigate, cancel]() {
        if (cancel.isCancelled()) return;
        if (fresh && fresh != list && m_merge) m_merge(list, fresh);
        m_show = show;
        m_episodes.setPlaylist(list);
        m_episodes.setReversed(shouldReverse);
        updateContinueEpisode();
        if (navigate) MainWindow::instance().navigateTo(MainWindow::Page::Details);
        emit showChanged();
        emit lastWatchedIndexChanged();
    }, Qt::QueuedConnection);
}

void CurrentShow::markWatched(int index) {
    auto list = playlist();
    if (!list || !list->isValidIndex(index)) return;
    auto item = list->at(index);
    if (item->preview) return;
    item->setProgress(1.0);
    if (!isPlaying(*list)) list->setCurrentIndex(index);
    emit episodeMarked(list->link, index, 1.0);
    onPlaybackIndexChanged();
}

void CurrentShow::markUnwatched(int index) {
    auto list = playlist();
    if (!list || !list->isValidIndex(index)) return;
    auto item = list->at(index);
    if (item->preview) return;
    item->setProgress(0.0);
    if (!isPlaying(*list)) list->setCurrentIndex(index);
    emit episodeMarked(list->link, index, 0.0);
    onPlaybackIndexChanged();
}

void EpisodeListModel::setPlaylist(const QSharedPointer<PlaylistItem> &playlist) {
    beginResetModel();
    m_playlist = playlist;
    const bool seasonWasSet = m_season != 0;
    m_season = 0;
    QVariantList seasons;
    for (int i = 0; playlist && i < playlist->count(); ++i)
        if (const auto ep = playlist->at(i); ep && ep->season > 0 && !seasons.contains(ep->season))
            seasons.append(ep->season);
    m_seasons = seasons.size() > 1 ? seasons : QVariantList();
    rebuildFilteredIndices();
    endResetModel();
    if (seasonWasSet) emit seasonChanged();
    emit seasonsChanged();
    emit rangeChanged();
}

void EpisodeListModel::setSeason(int season) {
    if (m_season == season) return;
    beginResetModel();
    m_season = season;
    rebuildFilteredIndices();
    endResetModel();
    emit seasonChanged();
    emit rangeChanged();
}

int EpisodeListModel::seasonFirst() const {
    const auto playlist = m_playlist.toStrongRef();
    for (int i = 0; playlist && i < playlist->count(); ++i)
        if (const auto ep = playlist->at(i); ep && (m_season == 0 || ep->season == m_season)) return i;
    return -1;
}

int EpisodeListModel::seasonLast() const {
    const auto playlist = m_playlist.toStrongRef();
    for (int i = playlist ? playlist->count() - 1 : -1; i >= 0; --i)
        if (const auto ep = playlist->at(i); ep && (m_season == 0 || ep->season == m_season)) return i;
    return -1;
}

void EpisodeListModel::setReversed(bool isReversed) {
    if (m_isReversed == isReversed) return;
    beginResetModel();
    m_isReversed = isReversed;
    endResetModel();
    emit reversedChanged();
}

void EpisodeListModel::setFilterText(const QString &text) {
    if (m_filterText == text) return;
    beginResetModel();
    m_filterText = text;
    rebuildFilteredIndices();
    endResetModel();
    emit filterTextChanged();
}

void EpisodeListModel::rebuildFilteredIndices() {
    m_filteredIndices.clear();
    if (!filtering()) return;
    auto playlist = m_playlist.toStrongRef();
    if (!playlist) return;
    const int count = playlist->count();
    for (int i = 0; i < count; i++) {
        auto ep = playlist->at(i);
        if (!ep || (m_season != 0 && ep->season != m_season)) continue;
        if (m_filterText.isEmpty()) { m_filteredIndices.append(i); continue; }
        const double n = ep->number;
        const QString numStr = (std::floor(n) == n)
            ? QString::number(static_cast<long long>(n))
            : QString::number(n);
        if (ep->name.contains(m_filterText, Qt::CaseInsensitive) ||
            numStr.contains(m_filterText))
            m_filteredIndices.append(i);
    }
}

int EpisodeListModel::visibleCount() const {
    if (filtering()) return m_filteredIndices.size();
    auto playlist = m_playlist.toStrongRef();
    return playlist ? playlist->count() : 0;
}

int EpisodeListModel::sourceIndex(int visibleRow) const {
    const int total = visibleCount();
    const int row = m_isReversed ? total - 1 - visibleRow : visibleRow;
    if (row < 0 || row >= total) return -1;
    return filtering() ? m_filteredIndices.at(row) : row;
}

int EpisodeListModel::visibleIndex(int sourceIdx) const {
    if (sourceIdx < 0) return -1;
    const int total = visibleCount();
    int row = sourceIdx;
    if (filtering())
        row = m_filteredIndices.indexOf(sourceIdx);
    if (row < 0 || row >= total) return -1;
    return m_isReversed ? total - 1 - row : row;
}

void EpisodeListModel::refreshProgress() {
    if (const int rows = visibleCount(); rows > 0)
        emit dataChanged(index(0), index(rows - 1), {ProgressRole});
}

int EpisodeListModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : visibleCount();
}

QVariant EpisodeListModel::data(const QModelIndex &index, int role) const {
    auto playlist = m_playlist.toStrongRef();
    if (!playlist) return {};
    auto episode = playlist->at(sourceIndex(index.row()));
    if (!episode) return {};

    switch (role) {
    case TitleRole:         return episode->name;
    case EpisodeNumberRole: return episode->number;
    case SeasonNumberRole:  return episode->season;
    case ProgressRole:      return episode->progress();
    case ThumbnailRole:     return episode->thumbnail;
    case AiredAtRole:       return episode->airedAt;
    case PreviewRole:       return episode->preview;
    case AvailableAtRole:   return episode->availableAt;
    default:                return {};
    }
}

QHash<int, QByteArray> EpisodeListModel::roleNames() const {
    return {
            {TitleRole, "title"},
            {EpisodeNumberRole, "episodeNumber"},
            {SeasonNumberRole, "seasonNumber"},
            {ProgressRole, "progress"},
            {ThumbnailRole, "thumbnail"},
            {AiredAtRole, "airedAt"},
            {PreviewRole, "preview"},
            {AvailableAtRole, "availableAt"},
            };
}
