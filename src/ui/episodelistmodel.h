#pragma once
#include <QAbstractListModel>
#include <QSharedPointer>
#include <QWeakPointer>
#include <QVariant>
#include <QVector>
#include <qqmlintegration.h>

class PlaylistItem;

class EpisodeListModel : public QAbstractListModel {
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(bool    reversed   READ isReversed  WRITE setReversed NOTIFY reversedChanged)
    Q_PROPERTY(QString filterText READ filterText  WRITE setFilterText  NOTIFY filterTextChanged)
    // 0 for every season; otherwise only that season's episodes.
    Q_PROPERTY(int     season     READ season      WRITE setSeason      NOTIFY seasonChanged)
    // The show's season numbers in list order, empty unless it has more than one.
    Q_PROPERTY(QVariantList seasons READ seasons NOTIFY seasonsChanged)
    // The chosen season's first and last episodes as indices into the whole list: all of it for
    // every season, -1 for none. What the download range and "Next unwatched" keep within.
    Q_PROPERTY(int seasonFirst READ seasonFirst NOTIFY rangeChanged)
    Q_PROPERTY(int seasonLast  READ seasonLast  NOTIFY rangeChanged)

public:
    explicit EpisodeListModel(QObject *parent = nullptr) : QAbstractListModel(parent) {}

    void setPlaylist(const QSharedPointer<PlaylistItem> &playlist);
    bool isReversed() const { return m_isReversed; }
    void setReversed(bool isReversed);
    QString filterText() const { return m_filterText; }
    void setFilterText(const QString &text);
    int season() const { return m_season; }
    void setSeason(int season);
    QVariantList seasons() const { return m_seasons; }
    int seasonFirst() const;
    int seasonLast() const;

    // Both account for filter and reversal.
    Q_INVOKABLE int sourceIndex(int visibleRow) const;
    Q_INVOKABLE int visibleIndex(int sourceIdx) const;
    // Playback moves progress without touching the list.
    Q_INVOKABLE void refreshProgress();

signals:
    void reversedChanged();
    void filterTextChanged();
    void seasonChanged();
    void seasonsChanged();
    void rangeChanged();

private:
    QWeakPointer<PlaylistItem> m_playlist;
    bool    m_isReversed = false;
    QString m_filterText;
    int     m_season = 0;
    QVariantList m_seasons;
    QVector<int> m_filteredIndices; // source indices of matching episodes, forward order

    bool filtering() const { return !m_filterText.isEmpty() || m_season != 0; }
    int  visibleCount() const;
    void rebuildFilteredIndices();

    enum {
        TitleRole = Qt::UserRole,
        EpisodeNumberRole,
        SeasonNumberRole,
        ProgressRole,
        ThumbnailRole,
        AiredAtRole,
        PreviewRole,
    };
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
};
