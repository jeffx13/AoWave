#include "shows/showdata.h"
#include "shows/playlistitem.h"
#include "shows/showprovider.h"
#include <QRegularExpression>

void ShowData::setPlaylist(QSharedPointer<PlaylistItem> playlist) {
    m_playlist = std::move(playlist);
}

void ShowData::addEpisode(int seasonNumber, float number, const QString &episodeLink, const QString &name,
                          bool preview) {
    if (!m_playlist)
        m_playlist = QSharedPointer<PlaylistItem>::create(title, provider, link);
    m_playlist->emplaceBack(seasonNumber, number, episodeLink, name, false, preview);
}

void ShowData::addNumberedEpisode(int seasonNumber, const QString &episodeLink, QString label) {
    if (label.startsWith(QStringLiteral("第"))) {
        static const QRegularExpression digits(QStringLiteral(R"(\d+)"));
        label = digits.match(label).captured(0);
    }
    bool ok = false;
    const float number = label.toFloat(&ok);
    addEpisode(seasonNumber, ok ? number : -1.0f, episodeLink, ok ? QString() : label);
}
