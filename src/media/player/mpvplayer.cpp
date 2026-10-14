#include "media/player/mpvplayer.h"
#include "media/player/tracks.h"
#include "app/async.h"
#include "app/settings.h"
#include <QClipboard>
#include <QColor>
#include <QDateTime>
#include <QImage>
#include <QScopeGuard>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QtEndian>
#include <QProcess>
#include <cstring>
#include <QDir>
#include <QDirIterator>
#include "system/platform.h"
#include <QFileInfo>
#include <QSettings>
#include <QtConcurrent/QtConcurrentRun>
#include <QCoreApplication>
#include <QMetaType>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>
#include <iterator>
#include <stdexcept>
#include <QStringList>
#include <QRunnable>
#include <QSGGeometryNode>
#include <QSGMaterial>
#include <QSGMaterialShader>
#include <QSGSimpleRectNode>
#include <QGuiApplication>
#include <QScreen>
#include "app/mainwindow.h"
#include "app/logger.h"
#include "media/player/danmaku.h"
#include "media/render/videooutput.h"
#include <dxgi1_2.h>
#include <windows.h>

namespace {
void keepDisplayAwake() { Platform::keepDisplayAwake(); }
void allowDisplaySleep() { Platform::allowDisplaySleep(); }

enum class Observed : uint64_t {
    Duration, PlaybackTime, PausedForCache, BaseIdle, Pause, TrackList,
    AudioId, SubtitleId, SecondarySubtitleId, SecondarySubtitleText, SubtitleScale,
    SubtitleDelay, VideoId, CacheEnd, LoopA, LoopB, EofReached, Speed,
};

constexpr const char *kObservedProperties[] = {
    "duration", "playback-time", "paused-for-cache", "base-idle", "pause", "track-list",
    "aid", "sid", "secondary-sid", "secondary-sub-text", "sub-scale",
    "sub-delay", "vid", "demuxer-cache-time", "ab-loop-a", "ab-loop-b", "eof-reached", "speed",
};
static_assert(std::size(kObservedProperties) == size_t(Observed::Speed) + 1,
              "kObservedProperties must list every Observed property, in the same order");

}

class HoleMaterial : public QSGMaterial {
public:
    HoleMaterial() { setFlag(Blending); }
    QSGMaterialType *type() const override { static QSGMaterialType type; return &type; }
    QSGMaterialShader *createShader(QSGRendererInterface::RenderMode) const override;
    int compare(const QSGMaterial *other) const override {
        const auto *o = static_cast<const HoleMaterial *>(other);
        if (size != o->size) return size.width() < o->size.width() || size.height() < o->size.height() ? -1 : 1;
        return radius < o->radius ? -1 : radius > o->radius ? 1 : 0;
    }
    QSizeF size;
    float radius = 0;
};

class HoleShader : public QSGMaterialShader {
public:
    HoleShader() {
        setShaderFileName(VertexStage, QStringLiteral(":/App/src/ui/shaders/hole.vert.qsb"));
        setShaderFileName(FragmentStage, QStringLiteral(":/App/src/ui/shaders/hole.frag.qsb"));
        setFlag(UpdatesGraphicsPipelineState);
    }
    bool updateUniformData(RenderState &state, QSGMaterial *material, QSGMaterial *) override {
        QByteArray *buffer = state.uniformData();
        if (state.isMatrixDirty()) std::memcpy(buffer->data(), state.combinedMatrix().constData(), 64);
        if (state.isOpacityDirty()) {
            const float opacity = state.opacity();
            std::memcpy(buffer->data() + 64, &opacity, 4);
        }
        const auto *hole = static_cast<const HoleMaterial *>(material);
        const float size[2] = {float(hole->size.width()), float(hole->size.height())};
        std::memcpy(buffer->data() + 72, size, 8);
        std::memcpy(buffer->data() + 80, &hole->radius, 4);
        return true;
    }
    bool updateGraphicsPipelineState(RenderState &, GraphicsPipelineState *state, QSGMaterial *, QSGMaterial *) override {
        state->blendEnable = true;
        state->srcColor = GraphicsPipelineState::Zero;
        state->dstColor = GraphicsPipelineState::OneMinusSrcAlpha;
        return true;
    }
};

QSGMaterialShader *HoleMaterial::createShader(QSGRendererInterface::RenderMode) const { return new HoleShader; }

class HoleNode : public QSGGeometryNode {
public:
    HoleNode() : m_geometry(QSGGeometry::defaultAttributes_TexturedPoint2D(), 4) {
        setGeometry(&m_geometry);
        setMaterial(&m_material);
    }
    void update(const QRectF &rect, qreal radius) {
        QSGGeometry::updateTexturedRectGeometry(&m_geometry, rect, QRectF(0, 0, 1, 1));
        markDirty(DirtyGeometry);
        if (m_material.size != rect.size() || m_material.radius != float(radius)) {
            m_material.size = rect.size();
            m_material.radius = float(radius);
            markDirty(DirtyMaterial);
        }
    }

private:
    QSGGeometry m_geometry;
    HoleMaterial m_material;
};

MpvPlayer::MpvPlayer(QQuickItem *parent) : QQuickItem(parent) {
    s_instance.store(this, std::memory_order_release);
    setFlag(ItemHasContents);
    m_time.store(0, std::memory_order_relaxed);
    m_duration.store(0, std::memory_order_relaxed);
    int vol = Settings::instance().get(Config::Volume);
    m_volume = (vol >= 0 && vol <= 200) ? vol : 100;
    double speed = Settings::instance().get(Config::Speed);
    m_speed = (speed > 0.0 && speed <= 10.0) ? static_cast<float>(speed) : 1.0f;

    connect(this, &QQuickItem::windowChanged, &m_composition, &VideoComposition::attach);
    connect(this, &QQuickItem::windowChanged, this, [this](QQuickWindow *window) {
        if (window) connect(window, &QWindow::screenChanged, this, &MpvPlayer::applySmoothMotion);
    });
    connect(&m_composition, &VideoComposition::resized, &m_upscaler, &Upscaler::onResized);
    connect(&m_upscaler, &Upscaler::upscalersChanged, this, &MpvPlayer::upscalersChanged);
    connect(&m_upscaler, &Upscaler::frameRateChanged, this, &MpvPlayer::applySmoothMotion);
    VideoOutput::syncConfig(Platform::moduleDir() + QStringLiteral("/mpv"), Settings::dataDir() + QStringLiteral("/mpv"));
    const QDir mpvDir(Settings::dataDir() + QStringLiteral("/mpv"));
    m_mpv.set_option("script-opts",
                     ("ytdl_hook-ytdl_path=" + Settings::toolPath(QStringLiteral("yt-dlp.exe"))).toUtf8().constData());

    m_mpv.set_option("ytdl", Settings::instance().mpvYtdlEnabled());
    m_mpv.set_option("pause", false);
    m_mpv.set_option("softvol", true);
    if (Platform::quiet()) {
        m_mpv.set_option("ao", "null");
        m_mpv.set_option("mute", true);
    }
    // Silence fills the gap a missing segment leaves, or audio plays on ahead of the frozen picture.
    m_mpv.set_option("af", "@gapfill:lavfi=[aresample=async=1]");
    QByteArray adapter;
    if (QGuiApplication::platformName() == QLatin1String("offscreen")) {
        if (mpvDir.exists()) {
            m_mpv.set_option("config-dir", mpvDir.absolutePath().toUtf8().constData());
            m_mpv.set_option("config", "yes");
        }
        m_mpv.set_option("vo", "libmpv");
    } else {
        ShaderCache::seed();
        adapter = VideoOutput::setOptions(m_mpv);
        m_mpv.set_option("d3d11-composition-size", "1280x720");
        auto *warmup = new QTimer(this);
        warmup->setInterval(15000);
        connect(warmup, &QTimer::timeout, this, [this, warmup] {
            if (ShaderCache::startWarmup(window(), m_state == Playing)) warmup->deleteLater();
            else warmup->setInterval(10 * 60 * 1000);
        });
        if (!Platform::quiet()) warmup->start();
    }
    m_mpv.set_option("keep-open", true);
    m_mpv.set_option("screenshot-directory",
                     QStandardPaths::writableLocation(QStandardPaths::PicturesLocation).toUtf8().constData());

    // vid/aid/sid and visibility are sticky: a Bilibili external track's id names nothing next file.
    m_mpv.set_option("reset-on-next-file",
                     "video-aspect-override,af,audio-delay,pause,vid,aid,sid,secondary-sid,"
                     "ab-loop-a,ab-loop-b,sub-visibility,secondary-sub-visibility");

    m_mpv.set_option("hwdec", "auto-safe");
    m_mpv.set_option("curl-connect-timeout", "10");
    m_mpv.set_option("curl-max-retries", "10");

    m_mpv.set_option("cache", "yes");
    m_mpv.set_option("demuxer-max-bytes", "1GiB");
    m_mpv.set_option("demuxer-max-back-bytes", "512MiB");
    const bool mpvLogOn = Settings::instance().mpvLogEnabled();
    m_mpv.set_option("msg-level", mpvLogOn ? "all=v" : "all=error");
    m_mpv.set_option("force-seekable", "yes");
    m_mpv.set_option("cookies", "no");
    m_mpv.set_option("tls-verify", Settings::lenientTls() ? "no" : "yes");
    m_mpv.set_option("sub-ass-override", "scale");

    for (size_t i = 0; i < std::size(kObservedProperties); ++i)
        m_mpv.observe_property(i, kObservedProperties[i]);
    m_mpv.request_log_messages(mpvLogOn ? "info" : "warn");

    m_stallTimer.setSingleShot(true);
    m_stallTimer.setInterval(20000);
    connect(&m_stallTimer, &QTimer::timeout, this, [this]() {
        if (!m_buffering) return;
        if (m_bufferedTime > m_stallMark + 1) {
            m_stallMark = m_bufferedTime;
            m_stallTimer.start();
            return;
        }
        reportStreamFailure(tr("the stream stopped sending video"));
    });

    QObject::connect(&Settings::instance(), &Settings::subStyleChanged, this, &MpvPlayer::applySubtitleStyle);
    QObject::connect(&Settings::instance(), &Settings::pictureChanged, this, &MpvPlayer::applyPicture);
    QObject::connect(&Settings::instance(), &Settings::lenientTlsChanged, this,
                     [this] { m_mpv.set_property_async("tls-verify", !Settings::lenientTls()); });
    QObject::connect(&Settings::instance(), &Settings::normalizeAudioChanged, this, [this]() {
        applyAudioNormalization();
        showText(Settings::instance().normalizeAudio() ? tr("Volume normalisation on")
                                                       : tr("Volume normalisation off"));
    });

    QObject::connect(&Settings::instance(), &Settings::mpvYtdlEnabledChanged, this, [this]() {
        bool enabled = Settings::instance().mpvYtdlEnabled();
        m_mpv.set_property_async("ytdl", enabled);
        showText(enabled ? tr("ytdl on") : tr("ytdl off"));
    });

    m_danmakuRefresh.setSingleShot(true);
    m_danmakuRefresh.setInterval(100);
    connect(&m_danmakuWriter, &QFutureWatcher<QString>::finished, this, [this]() {
        attachDanmakuTrack();
        if (std::exchange(m_danmakuRewrite, false)) writeDanmaku();
    });
    connect(&m_danmakuFetch, &QFutureWatcher<QList<DanmakuComment>>::finished, this, [this]() {
        if (m_danmakuFetch.future().resultCount() == 0 || m_danmakuFetchKey != m_danmakuKey) return;
        m_danmaku = m_danmakuFetch.result();
        rebuildDanmakuHeat();
        if (m_danmaku.isEmpty()) { useBakedDanmaku(); return; }
        if (!DanmakuOptions::current().enabled) {
            logInfo() << "Danmaku" << m_danmaku.size() << "comments held - danmaku is off";
            return;
        }
        writeDanmaku();
    });
    connect(&m_danmakuRefresh, &QTimer::timeout, this, &MpvPlayer::refreshDanmaku);
    QObject::connect(&Settings::instance(), &Settings::danmakuStyleChanged, this,
                     [this]() { m_danmakuRefresh.start(); });

    QObject::connect(&Settings::instance(), &Settings::danmakuEnabledChanged, this, [this]() {
        const int index = activeDanmakuIndex();
        const qint64 id = index >= 0 ? m_subtitleListModel.idForIndex(index) : 0;
        if (!Settings::instance().danmakuEnabled()) {
            if (id != 0 && m_primarySubId == id) setPrimarySub(0);
            restoreTrackPrefs();
        } else if (id != 0) {
            setPrimarySub(id);
            if (danmakuTrackIndex() < 0) ensureDanmaku();
        } else {
            ensureDanmaku();
        }
    });

    QObject::connect(&Settings::instance(), &Settings::mpvLogEnabledChanged, this, [this]() {
        bool enabled = Settings::instance().mpvLogEnabled();
        m_mpv.set_property_async("msg-level", enabled ? "all=v" : "all=error");
        m_mpv.request_log_messages(enabled ? "info" : "warn");
    });

    if (m_mpv.initialize() < 0)
        throw std::runtime_error("could not initialize mpv context");

    m_mpv.set_property("volume", static_cast<double>(m_volume));
    m_mpv.set_property("speed",  static_cast<double>(m_speed));

    m_upscaler.init(adapter);
    for (const char *name : {"brightness", "contrast", "saturation", "gamma"}) m_pictureBaseline.insert(QLatin1String(name), intProperty(name).value_or(0));
    m_pictureBaseline.insert(QStringLiteral("deband"), m_mpv.get_string("deband") == "yes");
    m_syncBaseline = QString::fromStdString(m_mpv.get_string("video-sync"));
    m_interpolationBaseline = m_mpv.get_string("interpolation") == "yes";
    m_tscaleBaseline = QString::fromStdString(m_mpv.get_string("tscale"));

    m_mpv.set_wakeup_callback(
        [](void *ctx) {
            MpvPlayer *obj = static_cast<MpvPlayer *>(ctx);
            QMetaObject::invokeMethod(obj, "onMpvEvent", Qt::QueuedConnection);
        },
        this);

}

MpvPlayer::~MpvPlayer() {
    s_instance.store(nullptr, std::memory_order_release);
    waitFor(m_danmakuWriter, "MpvPlayer danmaku writer");
    const char *stopCmd[] = {"stop", nullptr};
    m_mpv.command(stopCmd);   // terminate_destroy alone lets it play on
    if (QQuickWindow *win = window()) win->disconnect(this);
    m_composition.release();
}

void MpvPlayer::open(Source &playItem) {
    if (playItem.videos.isEmpty()) return;

    if (playItem.refreshPrimaryVideoUrl) {
        const QUrl refreshed = playItem.refreshPrimaryVideoUrl();
        if (refreshed.isValid() && Source::trusted(refreshed))
            playItem.videos[0].url = refreshed;
    }

    setLoading(true);
    m_openPending = true;
    m_fileLoaded = false;
    ++m_openGeneration;
    m_duration.store(0, std::memory_order_relaxed);
    m_time.store(0, std::memory_order_relaxed);
    emit durationChanged();
    emit timeChanged();

    m_state = Stopped;
    emit mpvStateChanged();

    if (playItem.danmakuKey.isEmpty() || playItem.danmakuKey != m_danmakuKey || m_danmaku.isEmpty()) {
        m_danmaku.clear();
        m_danmakuKey = playItem.danmakuKey;
        m_danmakuSource = playItem.danmakuSource;
        rebuildDanmakuHeat();
        emit danmakuHeatChanged();
        startDanmakuFetch();
    }

    const QScreen *screen = window() ? window()->screen() : QGuiApplication::primaryScreen();
    const int screenHeight = screen ? qRound(screen->size().height() * screen->devicePixelRatio()) : 0;
    std::stable_sort(playItem.videos.begin(), playItem.videos.end(),
                     [screenHeight](const Video &a, const Video &b) {
                         const bool aFits = screenHeight <= 0 || a.height <= screenHeight;
                         const bool bFits = screenHeight <= 0 || b.height <= screenHeight;
                         if (aFits != bFits) return aFits;
                         if (a.height != b.height) return a.height > b.height;
                         if (a.rank != b.rank) return a.rank > b.rank;
                         return a.bitrate > b.bitrate;
                     });
    m_previewUrl = playItem.videos[0].url;
    for (int i = 1, smallest = playItem.videos[0].height; i < playItem.videos.size(); ++i) {
        const Video &video = playItem.videos[i];
        if (video.height > 0 && (smallest <= 0 || video.height < smallest)) {
            smallest = video.height;
            m_previewUrl = video.url;
        }
    }

    std::stable_sort(playItem.audios.begin(), playItem.audios.end(),
                     [](const Track &a, const Track &b) {
                         return a.bitrate > b.bitrate;
                     });

    setHeaders(playItem.headers);

    QByteArray videoUrlData = Mpv::target(playItem.videos[0].url);
    QByteArray start;
    if (playItem.startSeconds > 0)
        start = "start=" + QByteArray::number(playItem.startSeconds, 'f', 3);
    else if (playItem.progress > 0)
        start = "start=" + QByteArray::number(playItem.progress * 100.0, 'f', 3) + '%';
    const char *plain[]  = {"loadfile", videoUrlData.constData(), nullptr};
    const char *placed[] = {"loadfile", videoUrlData.constData(), "replace", "-1", start.constData(), nullptr};
    m_mpv.command_async(start.isEmpty() ? plain : placed);

    m_audiosToBeAdded = playItem.audios;
    m_subtitlesToBeAdded = playItem.subtitles;
    m_videosToBeAdded = playItem.videos;

    m_currentVideoUrl = m_mainVideoUrl = playItem.videos[0].url;
    MainWindow::instance().navigateTo(MainWindow::Page::Player);
}

void MpvPlayer::play() {
    if (m_state == Paused)
        m_mpv.set_property_async("pause", false);
}

void MpvPlayer::pause() {
    if (m_state == Playing)
        m_mpv.set_property_async("pause", true);
}

void MpvPlayer::togglePlayPause() {
    if (m_state == Playing) pause();
    else                          play();
}

void MpvPlayer::stop() {
    const char *args[] = {"stop", nullptr};
    m_mpv.command_async(args);
}

void MpvPlayer::setSubDelay(double seconds) {
    seconds = qBound(-60.0, seconds, 60.0);
    seconds = qRound(seconds * 10.0) / 10.0;
    if (qFuzzyCompare(m_subDelay + 1.0, seconds + 1.0)) return;
    m_subDelay = seconds;
    m_mpv.set_property_async("sub-delay", seconds);
    showText(tr("Subtitle delay: %1s").arg(seconds, 0, 'f', 1));
    emit subDelayChanged();
}

void MpvPlayer::setSpeed(float speed) {
    speed = qBound(0.1f, speed, 10.0f);
    if (m_speed == speed) return;
    m_speed = speed;
    m_mpv.set_property_async("speed", static_cast<double>(speed) * (m_speedBoost ? 2 : 1));
    showText(m_speedBoost ? tr("Speed: %1x (%2x while held)").arg(speed).arg(speed * 2)
                          : tr("Speed: %1x").arg(speed));
    emit speedChanged();
    Settings::instance().set(Config::Speed, static_cast<double>(speed));
}

void MpvPlayer::seek(double time, bool absolute, bool keyframes) {
    if (m_state == Stopped || !std::isfinite(time)) return;
    if (absolute && time == m_time.load(std::memory_order_relaxed)) return;
    if (absolute && time < 0) time = 0;
    QByteArray time_str = QByteArray::number(time, 'f', 3);
    const char *flags = absolute ? (keyframes ? "absolute+keyframes" : "absolute")
                                 : (keyframes ? "relative+keyframes" : "relative+exact");
    const char *args[] = {"seek", time_str.constData(), flags, nullptr};
    m_mpv.command_async(args);
}

void MpvPlayer::setVolume(int volume) {
    volume = qBound(0, volume, 200);
    if (m_volume == volume) return;
    m_volume = volume;
    m_mpv.set_property_async("volume", static_cast<double>(volume));
    showText(tr("Volume: %1%").arg(volume));
    emit volumeChanged();
    if (volume > 0)
        Settings::instance().set(Config::Volume, volume);
}

void MpvPlayer::setSubVisible(bool subVisible) {
    if (subVisible && m_primarySubId == 0 && m_secondarySubId == 0
                   && m_subtitleListModel.count() > 0) {
        setSubIndex(0);
        return;
    }
    if (m_subVisible == subVisible) return;
    m_subVisible = subVisible;
    m_mpv.set_property_async("sub-visibility", m_subVisible);
    m_mpv.set_property_async("secondary-sub-visibility", m_subVisible);
    emit subVisibleChanged();
    if (!m_applyingTrackPrefs) saveTrackPrefs();
}

bool MpvPlayer::addVideo(const Track &video, bool lazy) {
    if (m_state == Stopped) return false;
    if (!m_videoListModel.append(video.url, video.title, video.lang, video.height)) return true;
    if (lazy) { m_lazyTracks.insert(video.url); return true; }
    QByteArray url = Mpv::target(video.url);
    const char *args[] = {"video-add", url.constData(), "auto", "", nullptr};
    m_mpv.command_async(args);
    return true;
}

void MpvPlayer::sendKeyPress(const QString &key) {
    if (key.isEmpty() || key.endsWith('+')) return;
    QByteArray cmd = key.toUtf8();
    const char *args[] = {"keypress", cmd.constData(), nullptr};
    m_mpv.command_async(args);
}

bool MpvPlayer::addAudio(const Track &audio, bool select, bool lazy) {
    if (m_state == Stopped) return false;
    if (!m_audioListModel.append(audio.url, audio.title, audio.lang)) return true;
    if (lazy) { m_lazyTracks.insert(audio.url); return true; }
    QByteArray url = Mpv::target(audio.url);
    QByteArray title = audio.title.toUtf8();
    const char *mode = select ? "select" : "auto";
    const char *args[] = {"audio-add", url.constData(), mode, title.constData(), "", nullptr};
    m_mpv.command_async(args);
    return true;
}

bool MpvPlayer::addSubtitle(const Track &subtitle) {
    if (m_state == Stopped) return false;
    if (!m_subtitleListModel.append(subtitle.url, subtitle.title, subtitle.lang)) return true;
    QByteArray url = Mpv::target(subtitle.url);
    QByteArray title = subtitle.title.toUtf8();
    QByteArray lang = subtitle.lang.toUtf8();
    const QString l = subtitle.lang.toLower();
    const bool isEnglish = l == "en" || l == "eng" || l.startsWith("en-") || l.startsWith("en_")
                           || subtitle.title.contains("english", Qt::CaseInsensitive);
    const bool isDanmaku = l == "danmaku";
    const char *args[] = {"sub-add", url.constData(),
                          (isEnglish || isDanmaku) ? "select" : "auto",
                          title.constData(), lang.constData(), nullptr};
    m_mpv.command_async(args);
    return true;
}

int MpvPlayer::danmakuTrackIndex() const {
    for (int i = 0; i < m_subtitleListModel.count(); ++i) {
        const Track *track = m_subtitleListModel.at(i);
        if (track && track->lang == QLatin1String("danmaku")) return i;
    }
    return -1;
}

int MpvPlayer::bakedDanmakuIndex() const {
    for (int i = 0; i < m_subtitleListModel.count(); ++i)
        if (m_bakedSubIds.contains(m_subtitleListModel.idForIndex(i))) return i;
    return -1;
}

int MpvPlayer::activeDanmakuIndex() const {
    const int live = danmakuTrackIndex();
    return live >= 0 ? live : bakedDanmakuIndex();
}

void MpvPlayer::useBakedDanmaku() {
    if (!Settings::instance().danmakuEnabled() || danmakuTrackIndex() >= 0) return;
    const int row = bakedDanmakuIndex();
    if (row < 0 || !m_subtitleListModel.hasRealId(row)) return;
    const qint64 id = m_subtitleListModel.idForIndex(row);
    if (m_primarySubId != id) setPrimarySub(id);
    if (!m_subVisible) setSubVisible(true);
}

void MpvPlayer::refreshDanmaku() {
    if (m_state == Stopped || !m_fileLoaded || danmakuTrackIndex() < 0) return;
    writeDanmaku();
}

void MpvPlayer::startDanmakuFetch() {
    if (!m_danmakuSource || m_danmakuKey.isEmpty()) return;
    if (!DanmakuOptions::current().enabled) {
        logInfo() << "Danmaku" << "not fetching - turned off in settings";
        return;
    }
    const QFuture<QList<DanmakuComment>> future = m_danmakuSource();
    m_danmakuFetchKey = m_danmakuKey;
    if (future.isFinished()) {
        m_danmaku = future.resultCount() > 0 ? future.result() : QList<DanmakuComment>{};
        rebuildDanmakuHeat();
        if (m_danmaku.isEmpty()) { useBakedDanmaku(); return; }
        writeDanmaku();
        return;
    }
    m_danmakuFetch.setFuture(future);
}

void MpvPlayer::rebuildDanmakuHeat() {
    constexpr int kBuckets = 160;
    const double duration = preciseDuration();
    if (m_danmaku.isEmpty() || duration <= 1.0) {
        if (m_danmakuHeat.isEmpty()) return;
        m_danmakuHeat.clear();
        emit danmakuHeatChanged();
        return;
    }

    QList<int> counts(kBuckets, 0);
    for (const DanmakuComment &c : std::as_const(m_danmaku)) {
        const int bucket = int(c.timeMs / 1000.0 / duration * kBuckets);
        if (bucket >= 0 && bucket < kBuckets) ++counts[bucket];
    }

    QList<int> sorted = counts;
    std::sort(sorted.begin(), sorted.end());
    const double ceiling = qMax(1, sorted.at(int(kBuckets * 0.97)));

    QVariantList heat;
    heat.reserve(kBuckets);
    for (int n : std::as_const(counts)) {
        heat.append(qMin(1.0, std::sqrt(n / ceiling)));
    }
    m_danmakuHeat = std::move(heat);
    emit danmakuHeatChanged();
}

void MpvPlayer::writeDanmaku() {
    if (m_danmaku.isEmpty() || m_danmakuKey.isEmpty()) return;
    if (m_danmakuWriter.isRunning()) { m_danmakuRewrite = true; return; }
    m_danmakuWriterKey = m_danmakuKey;
    m_danmakuWriter.setFuture(QtConcurrent::run(
        [comments = m_danmaku, key = m_danmakuKey, opt = DanmakuOptions::current(),
         dir = Settings::tempDir() + QStringLiteral("/danmaku")] {
            return DanmakuAss::writeFile(comments, key, opt, dir);
        }));
}

void MpvPlayer::attachDanmakuTrack() {
    if (m_danmakuWriter.future().resultCount() == 0 || m_danmakuWriter.result().isEmpty()) {
        useBakedDanmaku();
        return;
    }
    if (m_danmakuWriterKey != m_danmakuKey) {
        logInfo() << "Danmaku" << "discarding a file written for the previous episode";
        return;
    }
    const Track track(QUrl::fromLocalFile(m_danmakuWriter.result()), tr("Danmaku"), QStringLiteral("danmaku"));
    const int index = m_fileLoaded ? danmakuTrackIndex() : -1;
    if (index < 0) {
        const bool later = m_state == Stopped || !m_fileLoaded;
        logOk() << "Danmaku" << "attaching" << m_danmakuWriter.result()
                << (later ? "(queued until the file loads)" : "(now)");
        if (later) m_subtitlesToBeAdded.append(track);
        else       addSubtitle(track);
        return;
    }
    // Only once mpv has reported it: before that the row has a placeholder id and sub-reload does nothing.
    if (!m_subtitleListModel.hasRealId(index)) {
        logInfo() << "Danmaku" << "track row exists but mpv has not reported it yet; re-adding";
        addSubtitle(track);
        return;
    }
    // mpv's own "select" on sub-add loses to any sid written in the same batch of events.
    if (Settings::instance().danmakuEnabled()
        && m_primarySubId != m_subtitleListModel.idForIndex(index)) {
        setPrimarySub(m_subtitleListModel.idForIndex(index));
    }
    const QByteArray id = QByteArray::number(qlonglong(m_subtitleListModel.idForIndex(index)));
    const char *args[] = {"sub-reload", id.constData(), nullptr};
    if (m_mpv.command(args) < 0)
        logWarn() << "Danmaku" << "sub-reload failed for track" << id;
}

void MpvPlayer::screenshot() {
    if (m_state == Stopped) return;
    const char *args[] = {"osd-msg", "screenshot", nullptr};
    m_mpv.command_async(args);
}

void MpvPlayer::copyFrame() {
    if (m_state == Stopped) return;
    const char *args[] = {"screenshot-raw", "video", nullptr};
    mpv_node frame{};
    if (m_mpv.command_ret(args, &frame) < 0) {
        showText(tr("Could not copy the frame"));
        return;
    }
    const auto release = qScopeGuard([&frame] { mpv_free_node_contents(&frame); });

    int64_t width = 0, height = 0, stride = 0;
    QByteArray format;
    const mpv_byte_array *pixels = nullptr;
    if (frame.format == MPV_FORMAT_NODE_MAP) {
        for (int i = 0; i < frame.u.list->num; ++i) {
            const char *key = frame.u.list->keys[i];
            const mpv_node &field = frame.u.list->values[i];
            if (!std::strcmp(key, "w"))           width  = field.u.int64;
            else if (!std::strcmp(key, "h"))      height = field.u.int64;
            else if (!std::strcmp(key, "stride")) stride = field.u.int64;
            else if (!std::strcmp(key, "format")) format = field.u.string;
            else if (!std::strcmp(key, "data"))   pixels = field.u.ba;
        }
    }
    if (width <= 0 || height <= 0 || !pixels || (format != "bgr0" && format != "bgra")) {
        showText(tr("Could not copy the frame"));
        return;
    }
    const QImage view(static_cast<const uchar *>(pixels->data), int(width), int(height), qsizetype(stride),
                      format == "bgra" ? QImage::Format_ARGB32 : QImage::Format_RGB32);
    QGuiApplication::clipboard()->setImage(view.convertToFormat(QImage::Format_RGB888));
    showText(tr("Frame copied"));
}

void MpvPlayer::cycleABLoop() {
    if (m_state == Stopped) return;
    const char *args[] = {"osd-msg", "ab-loop", nullptr};
    m_mpv.command_async(args);
}

void MpvPlayer::setSpeedBoost(bool on) {
    if (m_speedBoost == on) return;
    m_speedBoost = on;
    const double effective = static_cast<double>(m_speed) * (on ? 2 : 1);
    m_mpv.set_property_async("speed", effective);
    showText(tr("Speed: %1x").arg(effective));
}

void MpvPlayer::applyAssOverride() {
    const bool restyle = Settings::instance().subOverrideStyled();
    const int danmaku = activeDanmakuIndex();
    const bool danmakuFirst = danmaku >= 0 && m_primarySubId != 0
                              && m_primarySubId == m_subtitleListModel.idForIndex(danmaku);
    const QString wanted = restyle ? QStringLiteral("force") : QStringLiteral("scale");
    setMpvProperty(QStringLiteral("sub-ass-override"), danmakuFirst ? QStringLiteral("scale") : wanted);
    setMpvProperty(QStringLiteral("secondary-sub-ass-override"), wanted);
}

void MpvPlayer::applySubtitleStyle() {
    const Settings &settings = Settings::instance();
    setMpvProperty(QStringLiteral("sub-font"),
                   settings.subFont().isEmpty() ? QStringLiteral("sans-serif") : settings.subFont());
    setMpvProperty(QStringLiteral("sub-color"), settings.subColor());
    setMpvProperty(QStringLiteral("sub-bold"), settings.subBold());
    setMpvProperty(QStringLiteral("sub-border-size"), settings.subOutline());
    setMpvProperty(QStringLiteral("sub-shadow-offset"), settings.subShadow());
    const int box = settings.subBackground();
    setMpvProperty(QStringLiteral("sub-border-style"),
                   box > 0 ? QStringLiteral("background-box") : QStringLiteral("outline-and-shadow"));
    setMpvProperty(QStringLiteral("sub-back-color"),
                   QColor(0, 0, 0, qRound(box * 2.55)).name(QColor::HexArgb));
    applyAssOverride();
}

namespace {
std::vector<const char *> argv(const QList<QByteArray> &args) {
    std::vector<const char *> list;
    for (const QByteArray &arg : args) list.push_back(arg.constData());
    list.push_back(nullptr);
    return list;
}
}

bool MpvPlayer::command(const QList<QByteArray> &args) {
    std::vector<const char *> list = argv(args);
    return m_mpv.command(list.data()) >= 0;
}

void MpvPlayer::commandAsync(const QList<QByteArray> &args) {
    std::vector<const char *> list = argv(args);
    m_mpv.command_async(list.data());
}

void MpvPlayer::setPropertyAsync(const char *name, const QByteArray &value) {
    m_mpv.set_property_async(name, value.constData());
}

std::optional<int64_t> MpvPlayer::intProperty(const char *name) const {
    int64_t value = 0;
    return m_mpv.get_int64(name, value) ? std::optional(value) : std::nullopt;
}

std::optional<double> MpvPlayer::doubleProperty(const char *name) const {
    double value = 0;
    return m_mpv.get_double(name, value) ? std::optional(value) : std::nullopt;
}

bool MpvPlayer::steady() const {
    return m_state == Playing && !m_buffering && !m_composition.stale() && window()
           && window()->isExposed();
}

void MpvPlayer::applyPicture() {
    const QVariantMap picture = Settings::instance().pictureTab() ? Settings::instance().picture() : m_pictureBaseline;
    for (const char *name : {"brightness", "contrast", "saturation", "gamma"})
        setMpvProperty(QLatin1String(name), picture.value(QLatin1String(name)).toInt());
    setMpvProperty(QStringLiteral("deband"), picture.value(QStringLiteral("deband")).toBool());
    applySmoothMotion();
    m_upscaler.onPictureChanged();
}

bool MpvPlayer::smoothMotionHelps(double fps, double refresh) {
    if (fps <= 0 || refresh <= 0) return false;
    const double ratio = refresh / fps;
    return std::abs(ratio - std::round(ratio)) > 0.03 && fps / refresh >= 0.12;
}

// In composition mode mpv cannot read the screen's refresh rate, so it is given; display-resample stays off otherwise.
void MpvPlayer::applySmoothMotion() {
    const QScreen *screen = window() ? window()->screen() : nullptr;
    const double refresh = screen ? screen->refreshRate() : 0;
    const double fps = doubleProperty("container-fps").value_or(0) * m_upscaler.frameFactor();
    const bool on = Settings::instance().smoothMotion() && m_fileLoaded && smoothMotionHelps(fps, refresh);
    if (on) {
        setMpvProperty(QStringLiteral("display-fps-override"), refresh);
        setMpvProperty(QStringLiteral("video-sync"), QStringLiteral("display-resample"));
        setMpvProperty(QStringLiteral("interpolation"), true);
        setMpvProperty(QStringLiteral("tscale"), QStringLiteral("oversample"));
    } else if (m_smoothing) {
        setMpvProperty(QStringLiteral("video-sync"), m_syncBaseline);
        setMpvProperty(QStringLiteral("interpolation"), m_interpolationBaseline);
        setMpvProperty(QStringLiteral("tscale"), m_tscaleBaseline);
        setMpvProperty(QStringLiteral("display-fps-override"), 0.0);
    }
    if (on != m_smoothing) logInfo() << "Player" << QStringLiteral("smooth motion %1 (%2 fps on %3 Hz)")
                                                      .arg(on ? QStringLiteral("on") : QStringLiteral("off"))
                                                      .arg(fps, 0, 'f', 2).arg(refresh, 0, 'f', 0);
    m_smoothing = on;
}

void MpvPlayer::onClientMessage(const mpv_event *event) {
    const auto *message = static_cast<const mpv_event_client_message *>(event->data);
    if (message->num_args < 2 || qstrcmp(message->args[0], "aowave-upscaler") != 0) return;
    const QString id = QString::fromUtf8(message->args[1]);
    m_upscaler.pick(id == QLatin1String("off") ? QString() : id);
}

void MpvPlayer::applyAudioNormalization() {
    const char *add[]    = {"af", "add", "@normalize:lavfi=[dynaudnorm=f=75:g=25:p=0.55]", nullptr};
    const char *remove[] = {"af", "remove", "@normalize", nullptr};
    m_mpv.command_async(Settings::instance().normalizeAudio() ? add : remove);
}

void MpvPlayer::onMpvEvent() {
    while (true) {
        const mpv_event *event = m_mpv.wait_event();
        if (!event || event->event_id == MPV_EVENT_NONE) break;
        switch (event->event_id) {
        case MPV_EVENT_START_FILE:      onStartFile(); break;
        case MPV_EVENT_FILE_LOADED:     onFileLoaded(); break;
        case MPV_EVENT_END_FILE:        onEndFile(event); break;
        case MPV_EVENT_IDLE:            onIdle(); break;
        case MPV_EVENT_VIDEO_RECONFIG:  onVideoReconfig(); break;
        case MPV_EVENT_LOG_MESSAGE:     onLogMessage(event); break;
        case MPV_EVENT_PROPERTY_CHANGE: onPropertyChange(event); break;
        case MPV_EVENT_CLIENT_MESSAGE:  onClientMessage(event); break;
        case MPV_EVENT_SEEK:
        case MPV_EVENT_PLAYBACK_RESTART: m_upscaler.restartWatch(); break;
        default: break;
        }
    }
}

void MpvPlayer::onStartFile() {
    m_openPending = false;
    m_upscaler.onStartFile();
    m_logRetries.clear();
    m_filterFaultShown = false;
    m_playNextEmitted = false;
    m_streamFailed = false;
    m_danmakuPlaced = false;
    m_bakedSubIds.clear();
    m_stallTimer.stop();
    if (std::exchange(m_nextIn, -1) != -1) emit nextInChanged();
    m_subRestored = false;
    m_videoPrefApplied = false;
    m_audioPrefApplied = false;
    m_videoWidth.store(0, std::memory_order_relaxed);
    m_videoHeight.store(0, std::memory_order_relaxed);
    emit videoSizeChanged();
    m_time.store(0, std::memory_order_relaxed);
    m_duration.store(0, std::memory_order_relaxed);
    m_bufferedTime = 0;
    m_buffering = false;
    emit durationChanged();
    emit bufferingChanged();
    emit bufferedTimeChanged();
    m_subVisible = true;
    emit timeChanged();
    emit subVisibleChanged();
}

void MpvPlayer::onFileLoaded() {
    if (!std::exchange(m_loggedSettings, true))
        logInfo() << "Player" << QStringLiteral("mpv settings: video-sync %1, interpolation %2, framedrop %3, hwdec %4, deband %5, tls-verify %6")
                                     .arg(QString::fromStdString(m_mpv.get_string("video-sync")), QString::fromStdString(m_mpv.get_string("interpolation")),
                                          QString::fromStdString(m_mpv.get_string("framedrop")), QString::fromStdString(m_mpv.get_string("hwdec-current")),
                                          QString::fromStdString(m_mpv.get_string("deband")), QString::fromStdString(m_mpv.get_string("tls-verify")));
    m_fileLoaded = true;
    m_state = Playing;
    keepDisplayAwake();
    applySmoothMotion();

    if (Settings::instance().normalizeAudio()) applyAudioNormalization();

    m_videoListModel.clear();
    m_lazyTracks.clear();
    if (m_videosToBeAdded.count() > 1) {
        m_videoListModel.append(m_videosToBeAdded[0].url,
                                m_videosToBeAdded[0].title,
                                m_videosToBeAdded[0].lang,
                                m_videosToBeAdded[0].height);
        for (int i = 1; i < m_videosToBeAdded.count(); i++)
            addVideo(m_videosToBeAdded[i], true);
    }
    m_videosToBeAdded.clear();
    m_audioListModel.clear();
    for (int i = 0; i < m_audiosToBeAdded.size(); ++i)
        addAudio(m_audiosToBeAdded[i], i == 0, i > 0);
    m_audiosToBeAdded.clear();

    m_subtitleListModel.clear();
    if (!m_externalSubIds.isEmpty()) { m_externalSubIds.clear(); emit externalSubsChanged(); }
    m_pendingSubPath.clear();
    if (m_primarySubId != 0)   { m_primarySubId = 0;   emit primarySubIdChanged(); }
    if (m_secondarySubId != 0) { m_secondarySubId = 0; emit secondarySubIdChanged(); }
    for (const auto &sub : std::as_const(m_subtitlesToBeAdded))
        addSubtitle(sub);
    m_subtitlesToBeAdded.clear();

    setLoading(false);
    emit fileLoaded();
    emit mpvStateChanged();

    ensureDanmaku();
    QTimer::singleShot(2000, this, [this, key = m_danmakuKey]() {
        if (key == m_danmakuKey) ensureDanmaku();
    });
}

void MpvPlayer::ensureDanmaku() {
    if (m_state == Stopped || m_danmakuKey.isEmpty() || !m_danmakuSource) return;
    if (!DanmakuOptions::current().enabled) return;
    if (danmakuTrackIndex() >= 0) return;
    if (m_danmakuWriter.isRunning()) return;
    if (m_danmaku.isEmpty()) { startDanmakuFetch(); return; }
    writeDanmaku();
}

void MpvPlayer::onEndFile(const mpv_event *event) {
    auto *ef = static_cast<mpv_event_end_file *>(event->data);
    if (m_openPending) return;
    const auto reason = static_cast<mpv_end_file_reason>(ef->reason);
    m_stallTimer.stop();
    setLoading(false);
    if (std::exchange(m_nextIn, -1) != -1) emit nextInChanged();
    if (reason == MPV_END_FILE_REASON_STOP) {
        m_time.store(0, std::memory_order_relaxed);
        m_duration.store(0, std::memory_order_relaxed);
        m_bufferedTime = 0;
        emit timeChanged();
        emit durationChanged();
        emit bufferedTimeChanged();
    }
    if (reason == MPV_END_FILE_REASON_ERROR)
        reportStreamFailure(QString::fromUtf8(mpv_error_string(ef->error)));
    else if (reason == MPV_END_FILE_REASON_EOF && !m_playNextEmitted) {
        m_playNextEmitted = true;
        if (endedEarly()) reportStreamFailure(tr("the stream ended early"));
        else emit playNext();
    }
}

void MpvPlayer::reportStreamFailure(const QString &reason) {
    if (std::exchange(m_streamFailed, true)) return;
    m_streamFailedAtMs = QDateTime::currentMSecsSinceEpoch();
    m_stallTimer.stop();
    const double at = preciseTime();
    logWarn() << "Player" << "Stream failed at" << QString::number(at, 'f', 1) + "s:" << reason;
    emit streamFailed(reason, at, m_openGeneration);
}

bool MpvPlayer::progressing() const {
    return m_state == Playing && !m_buffering
           && QDateTime::currentMSecsSinceEpoch() - m_progressAtMs < 4000;
}

// keep-open parks a dead stream on its last frame: far from the length that is a failure, at the same place twice the real end.
bool MpvPlayer::endedEarly() {
    const double duration = preciseDuration(), time = preciseTime();
    if (duration <= 60 || duration - time <= 15) return false;
    if (m_earlyEndAt >= 0 && std::abs(time - m_earlyEndAt) < 3) {
        m_earlyEndAt = -1;
        return false;
    }
    m_earlyEndAt = time;
    return true;
}

// A skipped HLS segment is a hole every mirror shares, so nothing to recover; a dead server stalls, errors or ends early.
void MpvPlayer::noteSkippedSegment(const QString &segment) {
    logWarn() << "Player" << "a segment is missing on the server, played past:" << segment;
    m_upscaler.restartWatch();
}

void MpvPlayer::onIdle() {
    m_state = Stopped;
    emit mpvStateChanged();
}

void MpvPlayer::onVideoReconfig() {
    int64_t width = 0, height = 0;
    if (!m_mpv.get_int64("dwidth", width) || !m_mpv.get_int64("dheight", height)) return;

    const int w = int(width);
    const int h = int(height);
    m_videoWidth.store(w, std::memory_order_relaxed);
    m_videoHeight.store(h, std::memory_order_relaxed);
    emit videoSizeChanged();
    m_composition.takeSwapchain();
    m_upscaler.apply();

    const QSizeF item = size();
    if (h > 0 && item.height() > 0)
        logInfo() << "Player" << QStringLiteral("%1x%2 (%3) into %4x%5 (%6)")
                              .arg(w).arg(h).arg(double(w) / h, 0, 'f', 3)
                              .arg(int(item.width())).arg(int(item.height()))
                              .arg(item.width() / item.height(), 0, 'f', 3);
}

static bool isDecoderNoise(const QString &text) {
    static const char *ignored[] = {
        "Reserved SBR extensions is not implemented",
        "upload a sample of this file",
        "ffmpeg-devel",
        "illegal icc",
        "illegal iid",
        "border_position non monotone",
        "may have been wrapped",
        "Failed to query swap chain's output information",
        "No key binding found for key",
        "CUDA hwdec only works with",
        "dst->params.blit_dst",
        "for texture: ../",
        "Backtrace:",
    };
    for (const char *needle : ignored)
        if (text.contains(QLatin1String(needle), Qt::CaseInsensitive)) return true;
    if (text.size() > 1 && text[0] == QLatin1Char('#') && text[1].isDigit()) return true;
    return false;
}

void MpvPlayer::onLogMessage(const mpv_event *event) {
    auto *msg = static_cast<mpv_event_log_message *>(event->data);
    if (m_fileLoaded && msg->text) {
        static const QRegularExpression skip(QStringLiteral(R"(Segment (\d+) of playlist (\d+) failed too many times)"));
        if (const auto match = skip.match(QString::fromUtf8(msg->text)); match.hasMatch())
            noteSkippedSegment(match.captured(2) + QLatin1Char(':') + match.captured(1));
    }
    if (msg->text && QByteArrayView(msg->text).contains("translating HLSL to DXBC"))
        MainWindow::instance().reportInfo(tr("Compiling the upscaler (first time only). The picture may stop for a "
                                           "minute or two."),
                                        tr("Upscaler"));
    if (msg->text && QByteArrayView(msg->text).contains("inference")) m_filterFault = QString::fromUtf8(msg->text).trimmed();
    if (msg->text && !m_filterFaultShown && QByteArrayView(msg->text).contains("Creating filter 'animejanai' failed")) {
        m_filterFaultShown = true;
        logWarn() << "Player" << QStringLiteral("AnimeJaNai filter failed:") << m_filterFault;
        MainWindow::instance().reportError(tr("AnimeJaNai could not start: %1\n\nPick the AnimeJaNai 3.6.x folder in "
                                            "Settings > Player > AnimeJaNai.")
                                             .arg(m_filterFault.isEmpty() ? tr("the filter could not load") : m_filterFault),
                                         tr("Upscaler"));
    }
    if (!Settings::instance().mpvLogEnabled()) return;
    static QString lastMsgText;
    auto msgText = QString::fromUtf8(msg->text).trimmed();
    if (msgText.isEmpty() || msgText == lastMsgText || isDecoderNoise(msgText)) return;
    lastMsgText = msgText;
    if (msgText.startsWith(QLatin1String("HTTP error")) || msgText.startsWith(QLatin1String("Failed to open "))
        || msgText.startsWith(QLatin1String("hls: Failed to open segment"))) {
        if (m_logRetries.contains(msgText)) return;
        m_logRetries.append(msgText);
        if (m_logRetries.size() > 6) m_logRetries.removeFirst();
    }
    logRaw() << "Player" << QStringLiteral("mpv:") << msgText;
}

void MpvPlayer::onPropertyChange(const mpv_event *event) {
    auto *prop = static_cast<mpv_event_property *>(event->data);
    if (prop->data == nullptr) return;

    const Mpv::Node &value = *static_cast<Mpv::Node *>(prop->data);
    if (value.type() == MPV_FORMAT_NONE) return;

    switch (static_cast<Observed>(event->reply_userdata)) {
    case Observed::PlaybackTime:
        onPlaybackTime(static_cast<double>(value));
        break;

    case Observed::Duration:
        m_duration.store(static_cast<double>(value), std::memory_order_relaxed);
        emit durationChanged();
        rebuildDanmakuHeat();
        break;

    case Observed::SubtitleDelay:
        // mpv's z/Z bindings move this too, and operator double() asserts.
        if (value.type() == MPV_FORMAT_DOUBLE) {
            const double delay = static_cast<double>(value);
            if (!qFuzzyCompare(m_subDelay + 1.0, delay + 1.0)) {
                m_subDelay = delay;
                emit subDelayChanged();
            }
        }
        break;

    case Observed::Pause:
        if (value && m_state == Playing) {
            m_state = Paused;
            allowDisplaySleep();
        } else if (!value && m_state == Paused) {
            m_state = Playing;
            keepDisplayAwake();
        }
        emit mpvStateChanged();
        break;

    case Observed::PausedForCache:
        m_buffering = bool(value);
        emit bufferingChanged();
        if (m_buffering && m_fileLoaded) {
            m_stallMark = m_bufferedTime;
            m_stallTimer.start();
        } else {
            m_stallTimer.stop();
        }
        break;

    case Observed::CacheEnd:
        m_bufferedTime = value.type() == MPV_FORMAT_DOUBLE ? static_cast<double>(value) : 0;
        emit bufferedTimeChanged();
        break;

    case Observed::EofReached:
        if (value && !m_playNextEmitted && !m_openPending) {
            m_playNextEmitted = true;
            if (endedEarly()) reportStreamFailure(tr("the stream ended early"));
            else emit playNext();
        }
        break;

    case Observed::Speed:
        m_upscaler.onSpeedChanged();
        break;

    case Observed::LoopA:
    case Observed::LoopB: {
        const double point = value.type() == MPV_FORMAT_DOUBLE ? static_cast<double>(value) : -1;
        (static_cast<Observed>(event->reply_userdata) == Observed::LoopA ? m_loopA : m_loopB) = point;
        emit abLoopChanged();
        break;
    }

    case Observed::BaseIdle:
        if (value && m_state == Playing) showText(tr("Pausing…"));
        break;

    case Observed::AudioId:
        if (value.type() == MPV_FORMAT_INT64)
            m_audioListModel.setCurrentId(static_cast<int64_t>(value));
        break;

    case Observed::VideoId:
        if (value.type() == MPV_FORMAT_INT64)
            m_videoListModel.setCurrentId(static_cast<int64_t>(value));
        break;

    case Observed::SubtitleId: {
        const qint64 id = value.type() == MPV_FORMAT_INT64 ? static_cast<int64_t>(value) : 0;
        if (id != 0) m_subtitleListModel.setCurrentId(id);
        else         m_subtitleListModel.setCurrentIndex(-1);
        if (m_primarySubId != id) { m_primarySubId = id; emit primarySubIdChanged(); applySubLayout(); applyAssOverride(); }
        break;
    }

    case Observed::SecondarySubtitleId: {
        const qint64 id = value.type() == MPV_FORMAT_INT64 ? static_cast<int64_t>(value) : 0;
        m_subtitleListModel.setSecondaryIndex(id != 0 ? m_subtitleListModel.indexForId(id) : -1);
        if (m_secondarySubId != id) { m_secondarySubId = id; emit secondarySubIdChanged(); applySubLayout(); }
        break;
    }

    case Observed::SecondarySubtitleText: {
        const QString text = value.type() == MPV_FORMAT_STRING ? QString(value) : QString();
        if (const int lines = text.isEmpty() ? 0 : text.count(QChar(0x0a)) + 1; lines != m_secondarySubLines) {
            m_secondarySubLines = lines;
            applySubLayout();
        }
        break;
    }

    case Observed::SubtitleScale:
        if (value.type() == MPV_FORMAT_DOUBLE) {
            m_subScale = double(value);
            applySubLayout();
        }
        break;

    case Observed::TrackList:
        parseTrackList(value);
        break;
    }
}

void MpvPlayer::onPlaybackTime(double time) {
    if (!std::isfinite(time)) return;
    if (time == m_time.load(std::memory_order_relaxed)) return;
    m_time.store(time, std::memory_order_relaxed);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    m_progressAtMs = now;
    if (m_streamFailed && now - m_streamFailedAtMs > 30000 && progressing()) m_streamFailed = false;
    emit timeChanged();
    applyAutoSkip(time);
}

void MpvPlayer::applyAutoSkip(double time) {
    const double duration = preciseDuration();
    const bool hasAniSkip = hasOP() || hasED();

    const int64_t edLength = hasAniSkip ? m_aniEDLength : m_EDLength;
    const bool hasOutro = edLength > 0 && edLength < duration;
    double endAt = duration;
    if (hasOutro && time > duration - edLength - 60
        && (hasED() ? Settings::instance().aniskipAuto() : m_skipED))
        endAt = duration - edLength;
    const int nextIn = duration > 0 && !m_playNextEmitted && endAt - time <= 60
                     ? int(std::ceil(std::max(0.0, endAt - time))) : -1;
    if (nextIn != m_nextIn) {
        m_nextIn = nextIn;
        emit nextInChanged();
    }
    if (!m_playNextEmitted && duration > 0 && time >= endAt) {
        m_playNextEmitted = true;
        emit playNext();
        return;
    }

    const int64_t opStart = hasAniSkip ? m_aniOPStart : m_OPStart;
    const int64_t opEnd   = opStart + (hasAniSkip ? m_aniOPLength : m_OPLength);
    if (opEnd > opStart && time >= opStart && time < opEnd && opEnd <= duration
        && (hasOP() ? Settings::instance().aniskipAuto() : m_skipOP))
        seek(opEnd, true);
}

static QUrl externalTrackUrl(const QString &raw) {
    const QUrl parsed(raw);
    if (parsed.scheme().size() > 1) return parsed;
    return QUrl::fromLocalFile(QDir::fromNativeSeparators(raw));
}

struct TrackInfo {
    QString type;
    int64_t id = -1;
    QString title, lang, codec;
    QUrl    url;
    int64_t w = 0, h = 0;
    int64_t channels = 0, bitrate = 0;
    double  fps = 0.0;
};

QString prettyCodec(const QString &codec) {
    if (codec.isEmpty()) return {};
    static const QMap<QString, QString> names{
        {"aac", "AAC"},   {"ac3", "AC3"},   {"eac3", "E-AC3"}, {"truehd", "TrueHD"},
        {"dts", "DTS"},   {"opus", "Opus"}, {"vorbis", "Vorbis"}, {"flac", "FLAC"},
        {"mp3", "MP3"},   {"subrip", "SRT"}, {"ass", "ASS"},   {"ssa", "SSA"},
        {"webvtt", "WebVTT"}, {"hdmv_pgs_subtitle", "PGS"}, {"dvd_subtitle", "VobSub"},
        {"mov_text", "Text"},
    };
    if (const QString name = names.value(codec.toLower()); !name.isEmpty()) return name;
    if (codec.startsWith(QLatin1String("pcm"), Qt::CaseInsensitive)) return QStringLiteral("PCM");
    return codec.toUpper();
}

QString channelLayout(int64_t channels) {
    switch (channels) {
    case 0:  return {};
    case 1:  return QStringLiteral("mono");
    case 2:  return QStringLiteral("stereo");
    case 6:  return QStringLiteral("5.1");
    case 8:  return QStringLiteral("7.1");
    default: return QString("%1 ch").arg(channels);
    }
}

QString externalBaseName(const QUrl &url) {
    if (url.isEmpty()) return {};
    return QFileInfo(url.isLocalFile() ? url.toLocalFile() : url.path()).completeBaseName();
}

QString trackLabel(const TrackInfo &t) {
    const bool isVideo = t.type == QLatin1String("video");
    const QString name = !t.title.isEmpty() ? t.title : externalBaseName(t.url);
    QString label;

    if (isVideo) {
        QString resolution;
        if (t.w > 0 && t.h > 0) {
            resolution = QString("%1x%2").arg(t.w).arg(t.h);
            if (t.fps > 0) resolution += QString(" %1FPS").arg(t.fps);
        }
        if (resolution.isEmpty()) resolution = prettyCodec(t.codec);
        label = (!name.isEmpty() && !resolution.isEmpty()) ? QString("%1 [%2]").arg(name, resolution)
              : name.isEmpty()                             ? resolution
                                                           : name;
        if (!t.lang.isEmpty())
            label = label.isEmpty() ? t.lang : QString("%1 (%2)").arg(label, t.lang);
        if (t.bitrate > 0) {
            if (!label.isEmpty()) label += " - ";
            label += QString("%1 kbps").arg(t.bitrate / 1000);
        }
        return label.isEmpty() ? QString("Video %1").arg(t.id) : label;
    }

    if (!name.isEmpty()) {
        label = (t.lang.isEmpty() || name.compare(t.lang, Qt::CaseInsensitive) == 0)
                    ? name : QString("%1 [%2]").arg(name, t.lang);
    } else {
        QStringList parts;
        if (const QString codec = prettyCodec(t.codec); !codec.isEmpty()) parts << codec;
        if (t.type == QLatin1String("audio"))
            if (const QString channels = channelLayout(t.channels); !channels.isEmpty()) parts << channels;
        const QString description = parts.join(QChar(' '));
        label = t.lang.isEmpty()       ? description
              : description.isEmpty()  ? t.lang
                                       : QString("%1 - %2").arg(t.lang, description);
    }

    if (t.type == QLatin1String("audio") && t.bitrate > 0) {
        if (!label.isEmpty()) label += " - ";
        label += QString("%1 kbps").arg(t.bitrate / 1000);
    }
    if (!label.isEmpty()) return label;
    return t.type == QLatin1String("audio") ? QString("Audio %1").arg(t.id)
                                            : QString("Subtitle %1").arg(t.id);
}

void MpvPlayer::parseTrackList(const Mpv::Node &trackList) {
    for (const Mpv::Node &track : trackList) {
        try {
            QString trackType = static_cast<const char *>(track["type"]);
            TrackListModel *listModel = nullptr;

            if (trackType == "sub")        listModel = &m_subtitleListModel;
            else if (trackType == "audio") listModel = &m_audioListModel;
            else if (trackType == "video") listModel = &m_videoListModel;
            else continue;

            int64_t id = -1;
            QString title, lang, codec;
            QUrl url;
            bool hlsVariant = false;
            double fps = 0.0;
            int64_t bitrate = 0, w = 0, h = 0, channels = 0;

            auto map = track.list();
            for (int i = 0; i < map->num; i++) {
                const char *key = map->keys[i];
                mpv_node &v = map->values[i];

                if (strcmp(key, "id") == 0 && v.format == MPV_FORMAT_INT64)
                    id = v.u.int64;
                else if (strcmp(key, "title") == 0 && v.format == MPV_FORMAT_STRING)
                    title = QString::fromUtf8(v.u.string);
                else if (strcmp(key, "lang") == 0 && v.format == MPV_FORMAT_STRING)
                    lang = QString::fromUtf8(v.u.string);
                else if (strcmp(key, "external-filename") == 0 && v.format == MPV_FORMAT_STRING)
                    url = externalTrackUrl(QString::fromUtf8(v.u.string));
                else if (strcmp(key, "demux-fps") == 0 && v.format == MPV_FORMAT_DOUBLE)
                    fps = v.u.double_;
                else if ((strcmp(key, "demux-bitrate") == 0 || strcmp(key, "hls-bitrate") == 0) && v.format == MPV_FORMAT_INT64) {
                    if (bitrate == 0) bitrate = v.u.int64;
                    if (key[0] == 'h') hlsVariant = true;
                }
                else if (strcmp(key, "demux-w") == 0 && v.format == MPV_FORMAT_INT64)
                    w = v.u.int64;
                else if (strcmp(key, "demux-h") == 0 && v.format == MPV_FORMAT_INT64)
                    h = v.u.int64;
                else if (strcmp(key, "codec") == 0 && v.format == MPV_FORMAT_STRING)
                    codec = QString::fromUtf8(v.u.string);
                else if (strcmp(key, "demux-channel-count") == 0 && v.format == MPV_FORMAT_INT64)
                    channels = v.u.int64;
            }

            if (id <= 0) continue;

            if (listModel == &m_subtitleListModel && url.isLocalFile()) {
                const QString path = QDir::cleanPath(url.toLocalFile());
                if (auto it = m_externalSubIds.find(path); it != m_externalSubIds.end()) {
                    if (*it != id) {
                        *it = id;
                        emit externalSubsChanged();
                        if (m_pendingSubPath == path) {
                            setSubSlot(!m_pendingSubSecondary, id);
                            m_pendingSubPath.clear();
                        }
                    }
                    continue;
                }
            }

            const QUrl rowUrl = url.isEmpty() && listModel == &m_videoListModel && !hlsVariant ? m_mainVideoUrl : url;
            if (!rowUrl.isEmpty())
                listModel->setId(rowUrl, id);

            if (!url.isEmpty() && !title.isEmpty()) {
                const QString suffix =
                    QFileInfo(url.isLocalFile() ? url.toLocalFile() : url.path()).suffix();
                if (!suffix.isEmpty()) {
                    if (title.compare(suffix, Qt::CaseInsensitive) == 0)
                        title.clear();
                    else if (title.endsWith(QLatin1Char('.') + suffix, Qt::CaseInsensitive))
                        title.chop(suffix.size() + 1);
                }
            }

            if (listModel == &m_subtitleListModel && url.isEmpty() && title.startsWith(QLatin1String("Danmaku"))) {
                m_bakedSubIds.insert(id);
                if (danmakuTrackIndex() >= 0) continue;
            }

            const TrackInfo info{trackType, id, title, lang, codec, url, w, h, channels, bitrate, fps};
            const bool derived = title.isEmpty();
            const QString label = trackLabel(info);

            if (listModel->indexForId(id) < 0)
                listModel->append(id, label, lang, derived);
            else if (!listModel->hasFinalTitle(id))
                listModel->updateById(id, label, derived);

            listModel->setStats(id, static_cast<int>(h), fps, static_cast<int>(bitrate));

        } catch (const std::exception &e) {
            logRaw() << "Player" << QStringLiteral("mpv:") << e.what();
        }
    }
    m_videoListModel.sortByQuality(true);
    m_audioListModel.sortByQuality(false);

    if (danmakuTrackIndex() >= 0)
        for (const int64_t id : std::as_const(m_bakedSubIds)) m_subtitleListModel.removeById(id);

    // mpv can report vid/aid/sid before the tracks exist, and the pending id is lost on clear.
    auto syncSelection = [this](const char *prop, TrackListModel &model) {
        if (int64_t id = 0; m_mpv.get_int64(prop, id)) model.setCurrentId(id);
    };
    syncSelection("vid", m_videoListModel);
    syncSelection("aid", m_audioListModel);
    syncSelection("sid", m_subtitleListModel);

    if (!m_danmakuPlaced && Settings::instance().danmakuEnabled()) {
        if (const int index = danmakuTrackIndex(); index >= 0 && m_subtitleListModel.hasRealId(index)) {
            m_danmakuPlaced = true;
            const qint64 danmaku = m_subtitleListModel.idForIndex(index);
            const qint64 previous = m_primarySubId;
            const auto baked = [this](qint64 id) { return m_bakedSubIds.contains(id); };
            if (previous != danmaku) {
                setPrimarySub(danmaku);
                if (previous > 0 && m_secondarySubId == 0 && !baked(previous)) setSecondarySub(previous);
            }
            if (baked(m_secondarySubId)) setSecondarySub(0);
            if (!m_subVisible) setSubVisible(true);
        }
    }

    restoreTrackPrefs();
}

void MpvPlayer::setMpvProperty(const QString &name, const QVariant &value) {
    QByteArray nameData = name.toLatin1();
    switch (value.typeId()) {
    case QMetaType::Bool:
        m_mpv.set_property_async(nameData.constData(), value.toBool());
        break;
    case QMetaType::Int:
    case QMetaType::Long:
    case QMetaType::LongLong:
        m_mpv.set_property_async(nameData.constData(), value.toLongLong());
        break;
    case QMetaType::Float:
    case QMetaType::Double:
        m_mpv.set_property_async(nameData.constData(), value.toDouble());
        break;
    case QMetaType::QByteArray: {
        QByteArray v = value.toByteArray();
        m_mpv.set_property_async(nameData.constData(), v.constData());
        break;
    }
    case QMetaType::QString: {
        QByteArray v = value.toString().toUtf8();
        m_mpv.set_property_async(nameData.constData(), v.constData());
        break;
    }
    }
}

void MpvPlayer::showText(const QString &text) {
    QByteArray data = text.toUtf8();
    const char *args[] = {"show-text", data.constData(), nullptr};
    m_mpv.command_async(args);
}

void MpvPlayer::setRadius(qreal radius) {
    if (qFuzzyCompare(m_radius, radius)) return;
    m_radius = radius;
    emit radiusChanged();
    update();
}

void MpvPlayer::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) {
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) m_composition.itemResized();
    update();
}

void MpvPlayer::itemChange(ItemChange change, const ItemChangeData &value) {
    QQuickItem::itemChange(change, value);
    if (change == ItemVisibleHasChanged) update();
}

QSGNode *MpvPlayer::updatePaintNode(QSGNode *old, UpdatePaintNodeData *) {
    const QRectF rect = boundingRect();
    if (rect.isEmpty()) {
        delete old;
        return nullptr;
    }
    if (!m_composition.shown()) {
        auto *node = dynamic_cast<QSGSimpleRectNode *>(old);
        if (!node) {
            delete old;
            node = new QSGSimpleRectNode(rect, Qt::black);
        }
        node->setRect(rect);
        return node;
    }
    auto *node = dynamic_cast<HoleNode *>(old);
    if (!node) {
        delete old;
        node = new HoleNode;
    }
    node->update(rect, m_radius);
    return node;
}

void MpvPlayer::setHeaders(const QMap<QString, QString> &headers) {
    m_playbackHeaders = headers;
    m_mpv.set_property("referrer", "");
    m_mpv.set_property("user-agent", "");
    m_mpv.set_property("http-header-fields", "");
    m_mpv.set_property("stream-lavf-o", "");
    Mpv::applyHttpHeaders(m_mpv.raw(), headers);
}

void MpvPlayer::setSkipOP(bool skip) {
    m_skipOP = skip;
    emit skipOPChanged();
}

void MpvPlayer::setSkipED(bool skip) {
    m_skipED = skip;
    emit skipEDChanged();
}

void MpvPlayer::setOPStart(qint64 start) {
    m_OPStart = start;
    emit skipOPStartChanged();
}

void MpvPlayer::setOPLength(qint64 length) {
    m_OPLength = length;
    emit skipOPLengthChanged();
}

void MpvPlayer::setEDLength(qint64 length) {
    m_EDLength = length;
    emit skipEDLengthChanged();
}

void MpvPlayer::setAudioIndex(int index) {
    if (index < 0 || index >= m_audioListModel.count()) return;
    if (const Track *track = m_audioListModel.at(index); track && m_lazyTracks.remove(track->url)) {
        const QByteArray url = Mpv::target(track->url);
        const QByteArray title = track->title.toUtf8();
        const char *args[] = {"audio-add", url.constData(), "select", title.constData(), "", nullptr};
        m_mpv.command_async(args);
        m_audioListModel.setCurrentIndex(index);
        if (!m_applyingTrackPrefs) { m_audioPrefApplied = true; saveTrackPrefs(); }
        return;
    }
    // mpv has not reported this track yet, and "aid -3" would clear the audio outright.
    if (!m_audioListModel.hasRealId(index)) { m_audioListModel.setCurrentIndex(index); return; }
    m_mpv.set_property_async("aid", m_audioListModel.idForIndex(index));
    m_audioListModel.setCurrentIndex(index);
    if (!m_applyingTrackPrefs) { m_audioPrefApplied = true; saveTrackPrefs(); }
}

void MpvPlayer::setSubSlot(bool primary, qint64 id) {
    if (id < 0) return;
    qint64 &slot = primary ? m_primarySubId : m_secondarySubId;
    qint64 &other = primary ? m_secondarySubId : m_primarySubId;
    if (slot == id) return;
    const qint64 previous = slot;

    if (id != 0 && other == id) setSubSlot(!primary, 0);

    slot = id;
    const char *property = primary ? "sid" : "secondary-sid";
    if (id == 0) m_mpv.set_property_async(property, "no");
    else         m_mpv.set_property_async(property, int64_t(id));

    if (primary) {
        m_subtitleListModel.setCurrentIndex(m_subtitleListModel.indexForId(id));
        emit primarySubIdChanged();
        applyAssOverride();
    } else {
        m_subtitleListModel.setSecondaryIndex(m_subtitleListModel.indexForId(id));
        emit secondarySubIdChanged();
    }

    if (id != 0 && !m_subVisible) setSubVisible(true);
    else if (m_primarySubId == 0 && m_secondarySubId == 0 && m_subVisible) setSubVisible(false);

    applySubLayout();
    if (m_applyingTrackPrefs) return;

    const QString path = pathForSubId(id);
    if (!path.isEmpty()) rememberEpisodeSub(path);
    if (primary) {
        if (id != 0) m_subRestored = true;
        else if (!pathForSubId(previous).isEmpty()) rememberEpisodeSub({});
    }
    saveTrackPrefs();
}

void MpvPlayer::setPrimarySub(qint64 id)   { setSubSlot(true, id); }
void MpvPlayer::setSecondarySub(qint64 id) { setSubSlot(false, id); }

qint64 MpvPlayer::externalSubId(const QString &path) const {
    return m_externalSubIds.value(QDir::cleanPath(path), 0);
}

QString MpvPlayer::subNameForId(qint64 id) const {
    if (const int row = m_subtitleListModel.indexForId(id); row >= 0)
        if (const Track *track = m_subtitleListModel.at(row))
            return track->title.isEmpty() ? track->lang : track->title;
    const QString path = pathForSubId(id);
    return path.isEmpty() ? QString() : QFileInfo(path).completeBaseName();
}

void MpvPlayer::setSubIndex(int index, bool secondary) {
    if (index < 0 || index >= m_subtitleListModel.count()) return;
    if (!m_subtitleListModel.hasRealId(index)) return;
    setSubSlot(!secondary, m_subtitleListModel.idForIndex(index));
}

void MpvPlayer::useExternalSubtitle(const QString &path, const QString &title,
                                    const QString &lang, bool secondary) {
    if (path.isEmpty() || m_state == Stopped) return;
    const QString key = QDir::cleanPath(path);
    if (QFileInfo(key).size() <= 0) return;
    if (const qint64 known = m_externalSubIds.value(key, 0); known != 0) {
        setSubSlot(!secondary, known);
        return;
    }
    m_externalSubIds.insert(key, 0);
    m_pendingSubPath = key;
    m_pendingSubSecondary = secondary;
    emit externalSubsChanged();

    const QByteArray file = QDir::toNativeSeparators(key).toUtf8();
    const QByteArray t = title.toUtf8(), l = lang.toUtf8();
    const char *args[] = {"sub-add", file.constData(), "auto", t.constData(), l.constData(), nullptr};
    m_mpv.command_async(args);
}

QString MpvPlayer::pathForSubId(qint64 id) const {
    if (id == 0) return {};
    for (auto it = m_externalSubIds.constBegin(); it != m_externalSubIds.constEnd(); ++it)
        if (it.value() == id) return it.key();
    return {};
}

void MpvPlayer::rememberEpisodeSub(const QString &path) const {
    if (m_episodeKey.isEmpty()) return;
    Settings::instance().setValue(Config::episodeSub(m_episodeKey), path);
}

void MpvPlayer::applySubLayout() {
    int gap = 0;
    if (m_primarySubId != 0 && m_secondarySubId != 0) {
        const int lines = qMax(1, m_secondarySubLines);
        gap = int(std::ceil(m_subScale * 5.3 * lines));
    }
    m_mpv.set_property_async("sub-pos", int64_t(qMax(0, m_subPos - gap)));
    m_mpv.set_property_async("secondary-sub-pos", int64_t(m_subPos));
}

void MpvPlayer::setSubPos(int pos) {
    m_subPos = pos;
    applySubLayout();
}

void MpvPlayer::saveTrackPrefs() {
    if (m_applyingTrackPrefs || m_showKey.isEmpty()) return;
    const QString key = QStringLiteral("tracks/") + m_showKey;
    const TrackPrefs stored = TrackPrefs::parse(Settings::instance().value(key).toString());

    auto subtitleTitle = [&](const QString &keptWhenExternal, int index, qint64 id) {
        if (!pathForSubId(id).isEmpty()) return keptWhenExternal;
        const Track *track = m_subtitleListModel.at(index);
        if (!track) return QString();
        return m_subtitleListModel.isDerivedTitle(m_subtitleListModel.idForIndex(index))
                   ? QString() : track->title;
    };

    const int audioIndex = m_audioListModel.currentIndex();
    const Track *audio = m_audioListModel.at(audioIndex);
    const bool audioTitled = audio && !m_audioListModel.isDerivedTitle(m_audioListModel.idForIndex(audioIndex));

    const QList<int> heights = m_videoListModel.heights();
    const int videoIndex = m_videoListModel.currentIndex();
    const int videoHeight = (videoIndex >= 0 && videoIndex < heights.size()) ? heights[videoIndex] : -1;
    int videoWithinHeight = 0;
    for (int i = 0; i < videoIndex && i < heights.size(); ++i)
        if (heights[i] == videoHeight) videoWithinHeight++;

    // Storing it would restore the danmaku track as the saved subtitle with danmaku off.
    const int danmakuIndex = activeDanmakuIndex();
    const int subtitleIndex = m_subtitleListModel.currentIndex() == danmakuIndex
                                  ? stored.subtitleIndex : m_subtitleListModel.currentIndex();

    TrackPrefs prefs;
    prefs.subtitleTitle = m_subtitleListModel.currentIndex() == danmakuIndex
                              ? stored.subtitleTitle
                              : subtitleTitle(stored.subtitleTitle, subtitleIndex, m_primarySubId);
    prefs.audioTitle = audioTitled ? audio->title : QString();
    prefs.subtitlesVisible = m_subVisible;
    prefs.videoHeight = videoHeight;
    prefs.videoWithinHeight = videoWithinHeight;
    prefs.audioIndex = audioIndex;
    prefs.secondarySubtitleTitle =
        subtitleTitle(stored.secondarySubtitleTitle, m_subtitleListModel.secondaryIndex(), m_secondarySubId);
    prefs.subtitleIndex = subtitleIndex;
    Settings::instance().setValue(key, prefs.toString());
}

void MpvPlayer::restoreTrackPrefs() {
    if (!m_episodeKey.isEmpty() && !m_subRestored) {
        const QString saved = Settings::instance().value(Config::episodeSub(m_episodeKey)).toString();
        if (!saved.isEmpty() && QFileInfo(saved).size() > 0 && !m_externalSubIds.contains(saved)) {
            m_subRestored = true;
            const bool danmakuFirst = Settings::instance().danmakuEnabled() && !m_danmakuKey.isEmpty();
            useExternalSubtitle(saved, QFileInfo(saved).completeBaseName(), {}, danmakuFirst);
        }
    }

    if (m_showKey.isEmpty() || (m_subRestored && m_videoPrefApplied && m_audioPrefApplied)) return;
    const QString stored = Settings::instance().value("tracks/" + m_showKey).toString();
    if (stored.isEmpty()) { m_subRestored = m_videoPrefApplied = m_audioPrefApplied = true; return; }
    const TrackPrefs prefs = TrackPrefs::parse(stored);

    if (!m_videoPrefApplied) {
        const int target = TrackPrefs::pickVideo(m_videoListModel.heights(), prefs.videoHeight,
                                                 prefs.videoWithinHeight);
        if (target < 0 || m_videoListModel.currentIndex() == target) {
            m_videoPrefApplied = true;
        } else if (const int64_t id = m_videoListModel.idForIndex(target); id > 0) {
            m_mpv.set_property_async("vid", id);
        } else if (const Track *track = m_videoListModel.at(target); track && m_lazyTracks.contains(track->url)) {
            m_applyingTrackPrefs = true;
            setVideoIndex(target);
            m_applyingTrackPrefs = false;
            m_videoPrefApplied = true;
        }
    }

    if (!m_audioPrefApplied) {
        QStringList titles;
        for (int i = 0; i < m_audioListModel.count(); ++i) titles << m_audioListModel.at(i)->title;
        const int target = TrackPrefs::pickAudio(titles, prefs.audioTitle, prefs.audioIndex);
        if (target < 0 || m_audioListModel.currentIndex() == target) {
            m_audioPrefApplied = true;
        } else if (const int64_t id = m_audioListModel.idForIndex(target); id > 0) {
            m_mpv.set_property_async("aid", id);
        } else if (const Track *track = m_audioListModel.at(target); track && m_lazyTracks.contains(track->url)) {
            m_applyingTrackPrefs = true;
            setAudioIndex(target);
            m_applyingTrackPrefs = false;
            m_audioPrefApplied = true;
        }
    }

    if (m_subRestored) return;
    const bool danmakuFirst = Settings::instance().danmakuEnabled() && activeDanmakuIndex() >= 0;

    m_applyingTrackPrefs = true;
    bool found = true;
    if (danmakuFirst) {
        if (prefs.secondarySubtitleTitle.isEmpty() && !prefs.subtitleTitle.isEmpty()) {
            found = false;
            for (int i = 0; i < m_subtitleListModel.count(); ++i)
                if (m_subtitleListModel.at(i)->title == prefs.subtitleTitle) { setSubIndex(i, true); found = true; break; }
        }
    } else if (!prefs.subtitleTitle.isEmpty()) {
        found = false;
        for (int i = 0; i < m_subtitleListModel.count(); ++i)
            if (m_subtitleListModel.at(i)->title == prefs.subtitleTitle) { setSubIndex(i); found = true; break; }
    } else if (prefs.subtitleIndex >= 0 && prefs.subtitleIndex < m_subtitleListModel.count()) {
        setSubIndex(prefs.subtitleIndex);
    }
    if (!prefs.secondarySubtitleTitle.isEmpty())
        for (int i = 0; i < m_subtitleListModel.count(); ++i)
            if (m_subtitleListModel.at(i)->title == prefs.secondarySubtitleTitle) {
                if (m_subtitleListModel.hasRealId(i))
                    setSecondarySub(m_subtitleListModel.idForIndex(i));
                break;
            }
    setSubVisible(prefs.subtitlesVisible);
    m_applyingTrackPrefs = false;
    if (found) m_subRestored = true;
}

void MpvPlayer::setVideoIndex(int index) {
    if (index < 0 || index >= m_videoListModel.count()) return;
    if (const Track *track = m_videoListModel.at(index); track && m_lazyTracks.remove(track->url)) {
        const QByteArray url = Mpv::target(track->url);
        const char *args[] = {"video-add", url.constData(), "select", "", nullptr};
        m_mpv.command_async(args);
        m_videoListModel.setCurrentIndex(index);
        m_currentVideoUrl = track->url;
        if (!m_applyingTrackPrefs) { m_videoPrefApplied = true; saveTrackPrefs(); }
        return;
    }
    if (!m_videoListModel.hasRealId(index)) { m_videoListModel.setCurrentIndex(index); return; }
    m_mpv.set_property_async("vid", m_videoListModel.idForIndex(index));
    m_videoListModel.setCurrentIndex(index);
    auto *track = m_videoListModel.at(index);
    if (track && !track->url.isEmpty())
        m_currentVideoUrl = track->url;
    if (!m_applyingTrackPrefs) { m_videoPrefApplied = true; saveTrackPrefs(); }
}

void MpvPlayer::setMuted(bool muted) {
    if (m_muted == muted) return;
    if (muted) {
        m_lastVolume = m_volume;
        setVolume(0);
    } else {
        setVolume(m_lastVolume);
    }
    m_muted = muted;
    emit mutedChanged();
}
