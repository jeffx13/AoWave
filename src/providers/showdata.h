#pragma once
#include <QDateTime>
#include <QList>
#include <QSharedPointer>
#include <QString>
#include <QWeakPointer>
#include <QEnableSharedFromThis>
#include <QFuture>
#include <QUrl>
#include <QMap>
#include <QRegularExpression>
#include <functional>

class PlaylistItem;
class Provider;

struct ShowData
{
    ShowData(const QString &title = "", const QString &link = "", const QString &coverUrl = "",
             Provider *provider = nullptr, const QString &latestTxt = "", int type = 0)
        : title(title), link(link), coverUrl(coverUrl), provider(provider), latestTxt(latestTxt), type(type) {}

    QString title;
    QString link;
    QString coverUrl;
    double coverAspect = 0;
    Provider *provider = nullptr;
    QString latestTxt;
    QString subtitle;
    QString description;
    QString releaseDate;
    QString status;
    QList<QString> genres;
    QString updateTime;
    QDateTime nextEpisodeAt;
    QString score;
    QString views;
    QString siteUrl;
    int type = None;

    enum ShowType { None = 0, Anime = 1, Movie = 2, TvSeries = 3, Variety = 4, Documentary = 5, Video = 6 };

    struct WatchState {
        int libraryType = -1;
        int lastWatchedIndex = -1;
        double progress = 0.0;
        QSharedPointer<PlaylistItem> playlist;
    };

    void setPlaylist(QSharedPointer<PlaylistItem> playlist);
    QSharedPointer<PlaylistItem> playlist() const { return m_playlist; }

    void addEpisode(int seasonNumber, float number, const QString &episodeLink, const QString &name,
                    bool preview = false);
    void addNumberedEpisode(int seasonNumber, const QString &episodeLink, QString label);

private:
    QSharedPointer<PlaylistItem> m_playlist;
};

class PlaylistItem : public QEnableSharedFromThis<PlaylistItem> {
public:
    enum Type { List = 1, Online = 2, Local = 4, Pasted = 8 };

    PlaylistItem(const QString& name = "", Provider* provider = nullptr, const QString &link = "");
    PlaylistItem(int seasonNumber, float number, const QString &link, const QString &name,
                 QSharedPointer<PlaylistItem> parent, bool isLocal = false, bool preview = false);
    ~PlaylistItem();

    PlaylistItem(const PlaylistItem&) = delete;
    PlaylistItem& operator=(const PlaylistItem&) = delete;

    QString name;
    QString displayName;
    QString link;
    int     season = 0;
    float   number = -1;
    int     type;
    bool    preview = false;
    bool    solo = false;
    QString thumbnail;
    qint64  airedAt = 0;
    qint64  availableAt = 0;

    bool isList()     const { return type & Type::List; }
    bool isLocalDir() const { return (type & Type::Local) && (type & Type::List); }

    QSharedPointer<PlaylistItem> at(int i)     const { return isValidIndex(i) ? m_children.at(i) : nullptr; }
    QSharedPointer<PlaylistItem> first()       const { return at(0); }
    QSharedPointer<PlaylistItem> last()        const { return at(count() - 1); }
    QSharedPointer<PlaylistItem> parent()      const { return m_parent.toStrongRef(); }
    bool  isEmpty()       const { return m_children.isEmpty(); }
    int   count()         const { return m_children.size(); }
    int   episodeCount()  const;
    int   row()           const { return m_row; }
    bool  isValidIndex(int index) const;
    int   indexOf(const QString &childLink) const;
    int   indexOf(const QSharedPointer<PlaylistItem> &child) const { return m_children.indexOf(child); }
    const QList<QSharedPointer<PlaylistItem>> &children() const { return m_children; }

    void emplaceBack(int seasonNumber, float episodeNumber, const QString &episodeLink, const QString &episodeName,
                     bool isLocal = false, bool isPreview = false);
    void append(QSharedPointer<PlaylistItem> value);
    void insert(int index, QSharedPointer<PlaylistItem> value);
    void removeAt(int index);
    void removeOne(const QSharedPointer<PlaylistItem> &value);
    void clear();
    void sort();

    bool setCurrentIndex(int index);
    int  currentIndex() const { return m_currentIndex; }
    QSharedPointer<PlaylistItem> currentItem() const { return at(m_currentIndex); }

    void   setProgress(double fraction);
    double progress() const;

    Provider *provider() const { return m_provider; }

private:
    void updateRowIndices(int startIndex = 0);

    Provider* m_provider = nullptr;
    QWeakPointer<PlaylistItem> m_parent;
    QList<QSharedPointer<PlaylistItem>> m_children;
    int m_currentIndex = -1;
    double m_progress = 0.0;
    int m_row = -1;
};

struct DanmakuComment {
    int     timeMs   = 0;
    int     mode     = 1;
    quint32 color    = 0xFFFFFFu;
    int     weight   = 10;
    bool    rated    = false;
    int     fontSize = 25;
    int     pool     = 0;
    int     attr     = 0;
    bool    colorful = false;
    QString text;
};

struct VideoServer {
    enum Translation { Unknown, Sub, Dub };

    QString name;
    QString link;
    Translation translation = Unknown;
    int resolution = 0;

    VideoServer(const QString& name, const QString& link, Translation translation = Unknown)
        : name(name), link(link), translation(translation)
    {
        static const QRegularExpression re(QStringLiteral("(\\d{3,4})\\s*[pP]"));
        if (const auto match = re.match(this->name); match.hasMatch())
            resolution = match.captured(1).toInt();
    }
};

struct Track {
    QUrl url;
    QString title;
    QString lang;
    int bitrate = 0;
    int height = 0;
    // Higher first, ahead of bitrate, which favours the fattest codec.
    int rank = 0;
    double fps = 0;
    QList<QUrl> alternates;

    Track(const QUrl& url, const QString& title = "", const QString& lang = "", int bitrate = 0)
        : url(url), title(title), lang(lang), bitrate(bitrate) {}

    static QString formatBitrate(int bps) {
        if (bps <= 0) return {};
        if (bps >= 1000000)
            return QString("%1 Mbps").arg(bps / 1000000.0, 0, 'f', 1);
        return QString("%1 kbps").arg(bps / 1000);
    }

};

struct Video : public Track {
    Video(const QUrl& url, const QString& title = "", int resolution = 0,
          int bitrate = 0, const QString& lang = "")
        : Track(url, title, lang, bitrate) { height = resolution; }
};

struct Source {
    QList<Video> videos;
    QList<Track> audios;
    QList<Track> subtitles;

    // What a site answers decides what mpv opens: a web stream or a file, never an mpv protocol
    // (av://lavfi, edl://, ytdl://...).
    static bool trusted(const QUrl &url) {
        return url.scheme() == QLatin1String("https") || url.scheme() == QLatin1String("http") || url.isLocalFile();
    }
    void keepTrustedTracks() {
        const auto untrusted = [](const Track &track) { return !trusted(track.url); };
        videos.removeIf(untrusted);
        audios.removeIf(untrusted);
        subtitles.removeIf(untrusted);
    }
    QMap<QString, QString> headers;
    double progress = 0.0;
    double startSeconds = 0.0;

    std::function<QUrl()> refreshPrimaryVideoUrl;

    std::function<QFuture<QList<DanmakuComment>>()> danmakuSource;
    QString danmakuKey;

    void addHeader(const QString& key, const QString& value) {
        headers.insert(key, value);
    }

    void clear() {
        videos.clear();
        audios.clear();
        subtitles.clear();
        headers.clear();
        refreshPrimaryVideoUrl = {};
        danmakuSource = {};
        danmakuKey.clear();
        progress = 0.0;
        startSeconds = 0.0;
    }
};
