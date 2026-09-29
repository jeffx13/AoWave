#include "ui/episodelistmodel.h"
#include "shows/playlistitem.h"
#include <cmath>

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
            };
}
