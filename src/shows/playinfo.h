#pragma once
#include <QFuture>
#include <QString>
#include <QUrl>
#include <QMap>
#include <QList>
#include <QRegularExpression>
#include <functional>

struct DanmakuComment {
    int     timeMs   = 0;
    int     mode     = 1;          // 1/2/3 scroll, 4 bottom, 5 top
    quint32 color    = 0xFFFFFFu;  // RGB
    int     weight   = 10;         // spam score, 1..11
    int     fontSize = 25;         // 18 small, 25 normal, 36 large
    QString text;
};

struct VideoServer {
    // Unknown = the provider does not say
    enum Translation { Unknown, Sub, Dub };

    QString name;
    QString link;
    Translation translation = Unknown;
    // From the name ("... 1080p" -> 1080); 0 when it just says "HD".
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
    int bitrate = 0;  // bits/sec
    int height = 0;   // for sorting video quality
    double fps = 0;

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

struct PlayInfo {
    QList<Video> videos;
    QList<Track> audios;
    QList<Track> subtitles;
    QMap<QString, QString> headers;
    double progress = 0.0;   // fraction of the duration

    // Some providers issue very short-lived URLs.
    std::function<QUrl()> refreshPrimaryVideoUrl;

    // Memoised, so a mid-playback toggle joins the running fetch.
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
    }
};
