#include "ui/showdetails.h"
#include "core/async.h"
#include "net/client.h"
#include "shows/playlistitem.h"
#include "shows/showprovider.h"
#include "core/appshell.h"
#include "core/logger.h"
#include "core/settings.h"
#include <QtConcurrent/QtConcurrentRun>

ShowDetails::ShowDetails(QObject *parent) : QObject(parent) {
    connect(&m_watcher, &QFutureWatcher<void>::finished, this, &ShowDetails::onLoadFinished);
    connect(&m_watcher, &QFutureWatcher<void>::started,  this, &ShowDetails::isLoadingChanged);
    connect(&m_watcher, &QFutureWatcher<void>::finished, this, &ShowDetails::isLoadingChanged);
}

ShowDetails::~ShowDetails() {
    m_cancel.cancel();
    waitFor(m_watcher, "ShowDetails load");
}

int ShowDetails::lastWatchedIndex() const {
    auto list = playlist();
    return list ? list->currentIndex() : -1;
}

int ShowDetails::episodeCount() const {
    auto list = playlist();
    return list ? list->episodeCount() : 0;
}

int ShowDetails::watchedCount() const {
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

void ShowDetails::setLastWatchedIndex(int index) {
    auto list = playlist();
    if (!list) return;

    if (list->parent() && list->parent()->currentIndex() == list->row())
        return;

    if (!list->setCurrentIndex(index)) return;
    updateContinueEpisode();
    emit lastWatchedIndexChanged();
}

void ShowDetails::updateContinueEpisode() {
    m_episodes.refreshProgress();
    auto list = playlist();
    if (!list) { m_continueText.clear(); m_continueIndex = -1; return; }

    int idx = qMax(list->currentIndex(), 0);
    // A trailer is never somewhere to continue from.
    while (idx > 0 && list->at(idx)->preview) --idx;

    // Past the threshold, point at the next real episode.
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

void ShowDetails::cancel() {
    if (m_watcher.isRunning())
        m_cancel.cancel();
}

void ShowDetails::setShow(const ShowData &show, const ShowData::WatchState &watchState, bool navigate) {
    if (m_watcher.isRunning()) {
        m_pendingShow = show;
        m_pendingInfo = watchState;
        m_pendingNavigate = navigate;
        m_hasPending = true;
        m_cancel.cancel();
        return;
    }
    if (m_show.link == show.link) {
        if (navigate) AppShell::instance().navigateTo(AppShell::Page::Info);
        return;
    }
    // Fresh, not reset(): a reset would un-cancel the load this supersedes.
    m_cancel = CancelToken{};
    m_watcher.setFuture(QtConcurrent::run(&ShowDetails::load, this, show, watchState, navigate, m_cancel));
}

void ShowDetails::reload(const ShowData &show, const ShowData::WatchState &watchState) {
    if (m_watcher.isRunning()) return;
    m_cancel = CancelToken{};
    m_watcher.setFuture(QtConcurrent::run(&ShowDetails::load, this, show, watchState, false, m_cancel));
}

void ShowDetails::onLoadFinished() {
    if (!m_hasPending) return;
    m_hasPending = false;
    setShow(m_pendingShow, m_pendingInfo, m_pendingNavigate);
}

// Worker thread, on by-value copies including the token.
void ShowDetails::load(ShowData show, ShowData::WatchState watchState, bool navigate, CancelToken cancel) {
    auto list = watchState.playlist;
    const bool usingExistingPlaylist = (list != nullptr);

    bool success = false;
    QString failure;
    if (show.provider) {
        logInfo() << show.provider->name() << "Loading" << show.title << "using" << show.link;
        Client client(cancel);
        try {
            success = show.provider->loadShow(&client, show);
            if (!success) failure = client.lastError();
        } catch (const std::exception &ex) {
            failure = QString::fromUtf8(ex.what());
        } catch (...) {
            failure = tr("an unknown error");
        }
    }
    if (cancel.isCancelled()) return;
    if (!success) {
        logWarn() << "ShowDetails" << "Failed to load" << show.title << failure;
        const QString provider = show.provider ? show.provider->name() : tr("The provider");
        AppShell::instance().reportError(
            failure.isEmpty()
                ? tr("%1 returned nothing for %2. The show may have been removed "
                     "from the site.").arg(provider, show.title)
                : tr("%1 could not load %2.\n\n%3").arg(provider, show.title, failure),
            tr("Could not load show"));
        return;
    }

    if (!list)
        list = show.playlist();

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

    logInfo() << "ShowDetails" << "Loaded" << show.title;

    QMetaObject::invokeMethod(this, [this, show = std::move(show), list, shouldReverse, navigate, cancel]() {
        // A newer request arrived.
        if (cancel.isCancelled()) return;
        m_show = show;
        m_episodes.setPlaylist(list);
        // Unconditional: setting it only when true keeps the previous show's order.
        m_episodes.setReversed(shouldReverse);
        updateContinueEpisode();
        if (navigate) AppShell::instance().navigateTo(AppShell::Page::Info);
        emit showChanged();
        emit lastWatchedIndexChanged();
    }, Qt::QueuedConnection);
}

void ShowDetails::markWatched(int index) {
    auto list = playlist();
    if (!list || !list->isValidIndex(index)) return;
    auto item = list->at(index);
    if (item->preview) return;
    item->setProgress(1.0);
    list->setCurrentIndex(index);
    emit episodeMarked(list->link, index, 1.0);
    onPlaybackIndexChanged();
}

// Episodes before the place count as watched whatever their progress, so the place moves to it.
void ShowDetails::markUnwatched(int index) {
    auto list = playlist();
    if (!list || !list->isValidIndex(index)) return;
    auto item = list->at(index);
    if (item->preview) return;
    item->setProgress(0.0);
    list->setCurrentIndex(index);
    emit episodeMarked(list->link, index, 0.0);
    onPlaybackIndexChanged();
}
