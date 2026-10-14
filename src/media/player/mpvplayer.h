#pragma once

#include "media/player/mpv.h"
#include "media/render/upscaler.h"
#include "media/render/videooutput.h"
#include <QByteArray>
#include <QFutureWatcher>
#include <QMutex>
#include <QQuickItem>
#include <QQuickWindow>
#include <QElapsedTimer>
#include <QHash>
#include <QTimer>
#include <QVariantList>
#include "providers/showdata.h"
#include "media/player/tracks.h"
#include <atomic>
#include <memory>
#include <optional>

class MpvPlayer : public QQuickItem, private UpscalerHost {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(qreal             radius         READ radius         WRITE setRadius      NOTIFY radiusChanged)
    Q_PROPERTY(QVariantList      upscalers      READ upscalers                           NOTIFY upscalersChanged)
    Q_PROPERTY(State             state          READ state                               NOTIFY mpvStateChanged)
    Q_PROPERTY(qint64            duration       READ duration                            NOTIFY durationChanged)
    Q_PROPERTY(qint64            time           READ time                                NOTIFY timeChanged)
    Q_PROPERTY(int               nextIn         READ nextIn                              NOTIFY nextInChanged)
    Q_PROPERTY(bool              subVisible     READ subVisible     WRITE setSubVisible  NOTIFY subVisibleChanged)
    Q_PROPERTY(double            subDelay       READ subDelay       WRITE setSubDelay    NOTIFY subDelayChanged)
    Q_PROPERTY(bool              skipOP         READ skipOP         WRITE setSkipOP      NOTIFY skipOPChanged)
    Q_PROPERTY(bool              skipED         READ skipED         WRITE setSkipED      NOTIFY skipEDChanged)
    Q_PROPERTY(qint64            primarySubId   READ primarySubId                        NOTIFY primarySubIdChanged)
    Q_PROPERTY(qint64            secondarySubId READ secondarySubId                      NOTIFY secondarySubIdChanged)
    Q_PROPERTY(bool              hasOP          READ hasOP                               NOTIFY hasOPChanged)
    Q_PROPERTY(bool              hasED          READ hasED                               NOTIFY hasEDChanged)
    Q_PROPERTY(qint64            skipOPStart    READ skipOPStart    WRITE setOPStart     NOTIFY skipOPStartChanged)
    Q_PROPERTY(qint64            skipOPLength   READ skipOPLength   WRITE setOPLength    NOTIFY skipOPLengthChanged)
    Q_PROPERTY(qint64            skipEDLength   READ skipEDLength   WRITE setEDLength    NOTIFY skipEDLengthChanged)
    Q_PROPERTY(qint64            aniOPStart     READ aniOPStart                          NOTIFY aniOPStartChanged)
    Q_PROPERTY(qint64            aniOPLength    READ aniOPLength                         NOTIFY aniOPLengthChanged)
    Q_PROPERTY(qint64            aniEDLength    READ aniEDLength                         NOTIFY aniEDLengthChanged)
    Q_PROPERTY(int               volume         READ volume         WRITE setVolume      NOTIFY volumeChanged)
    Q_PROPERTY(float             speed          READ speed          WRITE setSpeed       NOTIFY speedChanged)
    Q_PROPERTY(bool              muted          READ muted          WRITE setMuted       NOTIFY mutedChanged)
    Q_PROPERTY(bool              isLoading      READ isLoading                           NOTIFY isLoadingChanged)
    Q_PROPERTY(bool              buffering      READ buffering                           NOTIFY bufferingChanged)
    Q_PROPERTY(double            bufferedTime   READ bufferedTime                        NOTIFY bufferedTimeChanged)
    Q_PROPERTY(double            videoAspect    READ videoAspect                         NOTIFY videoSizeChanged)
    Q_PROPERTY(double            loopA          READ loopA                               NOTIFY abLoopChanged)
    Q_PROPERTY(double            loopB          READ loopB                               NOTIFY abLoopChanged)
    Q_PROPERTY(QVariantList      danmakuHeat    READ danmakuHeat                         NOTIFY danmakuHeatChanged)
    Q_PROPERTY(bool              hasDanmaku     READ hasDanmaku                          NOTIFY danmakuHeatChanged)
    Q_PROPERTY(TrackListModel*   subtitleList   READ subtitleList CONSTANT)
    Q_PROPERTY(TrackListModel*   videoList      READ videoList    CONSTANT)
    Q_PROPERTY(TrackListModel*   audioList      READ audioList    CONSTANT)

public:
    enum State { Stopped, Playing, Paused };
    Q_ENUM(State)

    inline static MpvPlayer *instance() { return s_instance.load(std::memory_order_acquire); }
    MpvPlayer(QQuickItem *parent = nullptr);
    ~MpvPlayer() override;

    Mpv::Handle &handle() { return m_mpv; }
    qreal radius() const { return m_radius; }
    void setRadius(qreal radius);

    State state()       const { return m_state;      }
    qint64 duration()   const { return m_duration.load(std::memory_order_relaxed);   }
    qint64 time()       const { return m_time.load(std::memory_order_relaxed);       }
    double preciseTime() const { return m_time.load(std::memory_order_relaxed); }
    double videoAspect() const {
        const int h = m_videoHeight.load(std::memory_order_relaxed);
        return h > 0 ? double(m_videoWidth.load(std::memory_order_relaxed)) / h : 0;
    }
    double preciseDuration() const { return m_duration.load(std::memory_order_relaxed); }
    int nextIn() const { return m_nextIn; }
    bool buffering() const { return m_buffering; }
    double bufferedTime() const { return m_bufferedTime; }
    bool muted()        const { return m_muted;      }
    bool subVisible()   const { return m_subVisible; }
    double subDelay()   const { return m_subDelay;   }
    int volume()        const { return m_volume;     }
    float speed()       const { return m_speed;      }
    bool skipOP()       const { return m_skipOP;     }
    bool skipED()       const { return m_skipED;     }
    qint64 skipOPStart()    const { return m_OPStart;    }
    qint64 skipOPLength()   const { return m_OPLength;   }
    qint64 skipEDLength()   const { return m_EDLength;   }
    bool hasOP()        const { return m_aniOPLength > 0; }
    bool hasED()        const { return m_aniEDLength > 0; }
    qint64 aniOPStart()  const { return m_aniOPStart;  }
    qint64 aniOPLength() const { return m_aniOPLength; }
    qint64 aniEDLength() const { return m_aniEDLength; }
    bool isLoading()    const { return m_isLoading;  }
    QVariantList danmakuHeat() const { return m_danmakuHeat; }
    bool hasDanmaku() const { return !m_danmakuKey.isEmpty() && bool(m_danmakuSource); }

    void open(Source &playItem);
    void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void togglePlayPause();
    void stop();
    Q_INVOKABLE void seek(double offset, bool absolute = true, bool keyframes = false);
    Q_INVOKABLE void setSpeed(float speed);
    void setVolume(int volume);
    void setSubVisible(bool subVisible);
    void setSubDelay(double seconds);
    Q_INVOKABLE void screenshot();
    Q_INVOKABLE void copyFrame();
    Q_INVOKABLE void cycleABLoop();
    double loopA() const { return m_loopA; }
    double loopB() const { return m_loopB; }
    Q_INVOKABLE void setMpvProperty(const QString &name, const QVariant &value);
    Q_INVOKABLE void showText(const QString &text) override;

    Q_INVOKABLE void setVideoIndex(int index);
    Q_INVOKABLE void setAudioIndex(int index);
    qint64 primarySubId()   const { return m_primarySubId;   }
    qint64 secondarySubId() const { return m_secondarySubId; }
    Q_INVOKABLE void setPrimarySub(qint64 id);
    Q_INVOKABLE void setSecondarySub(qint64 id);
    Q_INVOKABLE void clearSubs() { setPrimarySub(0); setSecondarySub(0); }

    qint64 externalSubId(const QString &path) const;
    Q_INVOKABLE QString subNameForId(qint64 id) const;

    Q_INVOKABLE void setSubIndex(int index, bool secondary = false);
    Q_INVOKABLE void setSubPos(int pos);
    Q_INVOKABLE void applySubtitleStyle();
    void applyAssOverride();
    Q_INVOKABLE void setSpeedBoost(bool on);
    Q_INVOKABLE void applyPicture();
    // Blending smooths 24, 25 and 30 fps on a 60 or 144 Hz screen; it gains nothing where the rate divides evenly.
    static bool smoothMotionHelps(double fps, double refresh);
    QVariantList upscalers() const { return m_upscaler.upscalers(); }
    bool progressing() const;
    quint64 openGeneration() const { return m_openGeneration; }
    void setOPStart(qint64 start);
    void setOPLength(qint64 length);
    void setEDLength(qint64 length);
    void setAniOPStart(qint64 v)  { if (m_aniOPStart == v) return;  m_aniOPStart = v;  emit aniOPStartChanged(); }
    void setAniOPLength(qint64 v) { if (m_aniOPLength == v) return; m_aniOPLength = v; emit aniOPLengthChanged(); emit hasOPChanged(); }
    void setAniEDLength(qint64 v) { if (m_aniEDLength == v) return; m_aniEDLength = v; emit aniEDLengthChanged(); emit hasEDChanged(); }

    Q_INVOKABLE QUrl currentVideoUrl() const { return m_currentVideoUrl; }
    QUrl previewUrl() const { return m_previewUrl; }
    QMap<QString, QString> playbackHeaders() const { return m_playbackHeaders; }
    Q_INVOKABLE void sendKeyPress(const QString &key);

    void refreshDanmaku();

    bool addVideo(const Track &video, bool lazy = false);
    bool addAudio(const Track &audio, bool select = false, bool lazy = false);
    bool addSubtitle(const Track &subtitle);
    void useExternalSubtitle(const QString &path, const QString &title,
                                         const QString &lang, bool secondary = false);
    void setHeaders(const QMap<QString, QString> &headers);
    void setShowKey(const QString &key) { m_showKey = key; }
    void setEpisodeKey(const QString &key) {
        if (key != m_episodeKey) m_earlyEndAt = -1;
        m_episodeKey = key;
    }
    void setSkipOP(bool skip);
    void setSkipED(bool skip);
    void setMuted(bool muted);

signals:
    void durationChanged();
    void timeChanged();
    void volumeChanged();
    void speedChanged();
    void playNext();
    void nextInChanged();
    void streamFailed(const QString &reason, double at, quint64 generation);
    void fileLoaded();
    void videoSizeChanged();
    void abLoopChanged();
    void bufferingChanged();
    void bufferedTimeChanged();
    void skipOPChanged();
    void skipEDChanged();
    void primarySubIdChanged();
    void secondarySubIdChanged();
    void externalSubsChanged();
    void hasOPChanged();
    void hasEDChanged();
    void skipOPStartChanged();
    void skipOPLengthChanged();
    void skipEDLengthChanged();
    void aniOPStartChanged();
    void aniOPLengthChanged();
    void aniEDLengthChanged();
    void mpvStateChanged();
    void subVisibleChanged();
    void subDelayChanged();
    void isLoadingChanged();
    void mutedChanged();
    void danmakuHeatChanged();
    void radiusChanged();
    void upscalersChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private:
    Mpv::Handle m_mpv;
    Upscaler m_upscaler{static_cast<UpscalerHost &>(*this)};
    inline static std::atomic<MpvPlayer *> s_instance{nullptr};
    State m_state = Stopped;

    VideoComposition m_composition{*this, m_mpv, [this] {
        return QSize(m_videoWidth.load(std::memory_order_relaxed), m_videoHeight.load(std::memory_order_relaxed));
    }};

    QStringList m_logRetries;
    QString m_filterFault;
    bool m_filterFaultShown = false;
    QVariantMap m_pictureBaseline;
    QString m_syncBaseline, m_tscaleBaseline;
    bool m_interpolationBaseline = false;
    bool m_smoothing = false;
    void applySmoothMotion();
    bool m_loggedSettings = false;
    qreal m_radius = 0;

    bool command(const QList<QByteArray> &args) override;
    void commandAsync(const QList<QByteArray> &args) override;
    void setPropertyAsync(const char *name, const QByteArray &value) override;
    std::optional<int64_t> intProperty(const char *name) const override;
    std::optional<double> doubleProperty(const char *name) const override;
    QSize displaySize() const override { return m_composition.size(); }
    bool paused() const override { return m_state == Paused; }
    bool steady() const override;

    void setLoading(bool loading) {
        if (m_isLoading == loading) return;
        m_isLoading = loading;
        emit isLoadingChanged();
    }

    std::atomic<double> m_time{0};
    std::atomic<double> m_duration{0};
    bool m_fileLoaded = false;
    // open() before mpv started its file: the file it replaces ends first, and that is not loading's end.
    bool m_openPending = false;
    bool m_speedBoost = false;
    bool m_buffering = false;
    double m_bufferedTime = 0;
    QMap<QString, QString> m_playbackHeaders;

    std::atomic<int> m_videoWidth{0};
    std::atomic<int> m_videoHeight{0};
    QList<Track> m_audiosToBeAdded;
    QList<Track> m_subtitlesToBeAdded;
    QSet<int64_t> m_bakedSubIds;
    QList<Video> m_videosToBeAdded;
    QSet<QUrl> m_lazyTracks;
    void   applyAudioNormalization();
    double m_loopA = -1;
    double m_loopB = -1;
    QUrl m_currentVideoUrl;
    QUrl m_mainVideoUrl;
    QUrl m_previewUrl;

    QList<DanmakuComment> m_danmaku;
    QVariantList m_danmakuHeat;
    void rebuildDanmakuHeat();
    QString m_danmakuKey;
    std::function<QFuture<QList<DanmakuComment>>()> m_danmakuSource;
    QString m_danmakuFetchKey;
    QTimer  m_danmakuRefresh;
    QFutureWatcher<QList<DanmakuComment>> m_danmakuFetch;
    void startDanmakuFetch();
    void writeDanmaku();

    TrackListModel m_subtitleListModel;
    TrackListModel m_audioListModel;
    TrackListModel m_videoListModel;

    double m_subDelay = 0.0;
    bool m_subVisible = false;
    float m_speed     = 1.0;
    int m_volume      = 100;
    int m_lastVolume  = 0;
    bool m_isLoading  = false;
    bool m_muted      = false;
    bool m_playNextEmitted = false;
    int m_nextIn = -1;
    bool m_skipOP     = false;
    bool m_skipED     = false;
    bool m_streamFailed = false;
    bool m_danmakuPlaced = false;
    double m_earlyEndAt = -1;
    qint64 m_streamFailedAtMs = 0;
    qint64 m_progressAtMs = 0;
    quint64 m_openGeneration = 0;
    QTimer m_stallTimer;
    double m_stallMark = 0;
    void reportStreamFailure(const QString &reason);
    bool endedEarly();
    void noteSkippedSegment(const QString &segment);
    qint64 m_OPStart  = 0;
    qint64 m_OPLength = 120;
    qint64 m_EDLength = 60;
    qint64 m_aniOPStart  = 0;
    qint64 m_aniOPLength = 0;
    qint64 m_aniEDLength = 0;

    QString m_showKey;
    bool    m_subRestored        = false;
    bool    m_videoPrefApplied   = false;
    bool    m_audioPrefApplied   = false;
    bool    m_applyingTrackPrefs = false;
    void saveTrackPrefs();
    void restoreTrackPrefs();

    TrackListModel *subtitleList() { return &m_subtitleListModel; }
    TrackListModel *audioList()    { return &m_audioListModel;    }
    TrackListModel *videoList()    { return &m_videoListModel;    }

    Q_INVOKABLE void onMpvEvent();
    void onPlaybackTime(double time);
    void applyAutoSkip(double time);
    void onStartFile();
    void onFileLoaded();
    void onEndFile(const mpv_event *event);
    void onIdle();
    void onVideoReconfig();
    void onLogMessage(const mpv_event *event);
    void onPropertyChange(const mpv_event *event);
    void onClientMessage(const mpv_event *event);
    void parseTrackList(const Mpv::Node &trackList);
    int  danmakuTrackIndex() const;
    int  bakedDanmakuIndex() const;
    int  activeDanmakuIndex() const;
    void useBakedDanmaku();
    void applySubLayout();
    void setSubSlot(bool primary, qint64 id);
    qint64 m_primarySubId = 0;
    qint64 m_secondarySubId = 0;
    QHash<QString, qint64> m_externalSubIds;
    QString m_pendingSubPath;
    bool    m_pendingSubSecondary = false;
    QString m_episodeKey;
    QString pathForSubId(qint64 id) const;
    void    rememberEpisodeSub(const QString &path) const;
    int    m_subPos = 100;
    int    m_secondarySubLines = 0;
    double m_subScale = 1.0;
    QFutureWatcher<QString> m_danmakuWriter;
    QString m_danmakuWriterKey;
    bool    m_danmakuRewrite = false;
    void attachDanmakuTrack();
    void ensureDanmaku();
};
