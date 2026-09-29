#include "media/seekpreview.h"
#include "media/mpvplayer.h"
#include <QPainter>
#include <QPainterPath>
#include <QQuickWindow>
#include <cmath>

namespace {

void onWakeup(void *item) {
    QMetaObject::invokeMethod(static_cast<SeekPreview *>(item), &SeekPreview::drainEvents, Qt::QueuedConnection);
}

void onRenderUpdate(void *item) {
    QMetaObject::invokeMethod(static_cast<SeekPreview *>(item), &SeekPreview::renderFrame, Qt::QueuedConnection);
}

}

SeekPreview::SeekPreview(QQuickItem *parent) : QQuickPaintedItem(parent) {
    setAntialiasing(true);
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(90);
    connect(&m_debounce, &QTimer::timeout, this, &SeekPreview::seekNow);
}

SeekPreview::~SeekPreview() {
    close();
}

void SeekPreview::setPlayer(MpvPlayer *player) {
    if (m_player == player) return;
    if (m_player) disconnect(m_player, nullptr, this, nullptr);
    m_player = player;
    if (m_player) {
        // Reopened on the next hover.
        connect(m_player, &MpvPlayer::fileLoaded, this, &SeekPreview::reset);
        connect(m_player, &MpvPlayer::mpvStateChanged, this, [this] {
            if (m_player->state() == MpvPlayer::Stopped) reset();
        });
    }
    reset();
    emit playerChanged();
}

void SeekPreview::setActive(bool active) {
    if (m_active == active) return;
    m_active = active;
    if (!m_active) {
        m_debounce.stop();
        m_seekPending = false;
    } else if (m_time >= 0) {
        setTime(m_time);
    }
    emit activeChanged();
    emit stateChanged();
}

void SeekPreview::setTime(double seconds) {
    if (!std::isfinite(seconds)) return;
    if (m_time != seconds) {
        m_time = seconds;
        emit timeChanged();
    }
    if (!m_active || !m_player) return;
    if (!m_mpv) {
        if (!m_available && m_player->currentVideoUrl() == m_failedUrl) return;
        open();
        if (!m_mpv) return;
    }
    m_debounce.start();
}

void SeekPreview::open() {
    close();
    const QUrl url = m_player->currentVideoUrl();
    if (url.isEmpty() || m_player->state() == MpvPlayer::Stopped) return;
    m_available = true;
    m_failedUrl.clear();

    m_mpv = mpv_create();
    if (!m_mpv) { m_available = false; emit stateChanged(); return; }
    // Video only, never buffering ahead.
    for (const auto &[name, value] : {
             std::pair{"vo", "libmpv"}, {"hwdec", "no"}, {"audio", "no"}, {"sid", "no"},
             {"sub-auto", "no"}, {"audio-file-auto", "no"}, {"pause", "yes"}, {"keep-open", "yes"},
             {"idle", "yes"}, {"hr-seek", "no"}, {"ytdl", "no"}, {"load-scripts", "no"}, {"osc", "no"},
             {"input-default-bindings", "no"}, {"terminal", "no"}, {"msg-level", "all=no"},
             {"config", "no"}, {"demuxer-max-bytes", "4MiB"}, {"demuxer-max-back-bytes", "1MiB"},
             {"demuxer-readahead-secs", "0"}, {"cache-pause", "no"}, {"vd-lavc-skiploopfilter", "all"},
             {"vd-lavc-fast", "yes"}, {"vd-lavc-threads", "2"}, {"network-timeout", "10"},
             {"tls-verify", "no"}, {"sws-fast", "yes"}, {"sws-scaler", "fast-bilinear"},
             {"cookies", "no"}})
        mpv_set_option_string(m_mpv, name, value);
    // Land on the hovered position rather than frame zero.
    m_shownTime = qMax(0.0, m_time);
    mpv_set_option_string(m_mpv, "start", QByteArray::number(m_shownTime, 'f', 3).constData());

    QStringList fields;
    const QMap<QString, QString> headers = m_player->playbackHeaders();
    for (auto it = headers.constBegin(); it != headers.constEnd(); ++it) {
        const QString key = it.key().toLower();
        if (key == "user-agent")
            mpv_set_option_string(m_mpv, "user-agent", it.value().toUtf8().constData());
        else if (key == "referrer" || key == "referer")
            mpv_set_option_string(m_mpv, "referrer", it.value().toUtf8().constData());
        fields << QStringLiteral("%1: %2").arg(it.key(), it.value());
    }
    if (!fields.isEmpty())
        mpv_set_option_string(m_mpv, "http-header-fields", fields.join(',').toUtf8().constData());

    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_SW)},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    if (mpv_initialize(m_mpv) < 0 || mpv_render_context_create(&m_render, m_mpv, params) < 0) {
        m_render = nullptr;
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
        m_available = false;
        m_failedUrl = url;
        emit stateChanged();
        return;
    }
    mpv_render_context_set_update_callback(m_render, onRenderUpdate, this);
    mpv_set_wakeup_callback(m_mpv, onWakeup, this);

    const QByteArray target = (url.isLocalFile() ? url.toLocalFile() : url.toString()).toUtf8();
    const char *load[] = {"loadfile", target.constData(), nullptr};
    mpv_command_async(m_mpv, 0, load);
    emit stateChanged();
}

void SeekPreview::close() {
    m_debounce.stop();
    if (m_render) {
        mpv_render_context_set_update_callback(m_render, nullptr, nullptr);
        mpv_render_context_free(m_render);
        m_render = nullptr;
    }
    if (m_mpv) {
        mpv_set_wakeup_callback(m_mpv, nullptr, nullptr);
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
    }
    m_loaded = m_seeking = m_seekPending = false;
    m_shownTime = -1;
    if (m_hasFrame) {
        m_hasFrame = false;
        update();
    }
    emit stateChanged();
}

void SeekPreview::reset() {
    close();
    m_available = true;
    m_failedUrl.clear();
    emit stateChanged();
}

// Keyframe seeks only: exact ones would decode from the keyframe anyway.
void SeekPreview::seekNow() {
    if (!m_mpv || !m_active) return;
    if (!m_loaded || m_seeking) { m_seekPending = true; emit stateChanged(); return; }
    m_seekPending = false;
    if (std::fabs(m_time - m_shownTime) < 0.5) { emit stateChanged(); return; }
    m_shownTime = m_time;
    const QByteArray at = QByteArray::number(qMax(0.0, m_time), 'f', 3);
    const char *seek[] = {"seek", at.constData(), "absolute+keyframes", nullptr};
    if (mpv_command_async(m_mpv, 0, seek) >= 0) m_seeking = true;
    emit stateChanged();
}

void SeekPreview::drainEvents() {
    while (m_mpv) {
        const mpv_event *event = mpv_wait_event(m_mpv, 0);
        if (event->event_id == MPV_EVENT_NONE) break;
        switch (event->event_id) {
        case MPV_EVENT_FILE_LOADED: {
            m_loaded = true;
            int64_t w = 0, h = 0;
            mpv_get_property(m_mpv, "dwidth", MPV_FORMAT_INT64, &w);
            mpv_get_property(m_mpv, "dheight", MPV_FORMAT_INT64, &h);
            if (w > 0 && h > 0) m_frameAspect = double(w) / double(h);
            if (m_time >= 0) seekNow();
            emit stateChanged();
            break;
        }
        case MPV_EVENT_PLAYBACK_RESTART:
            m_seeking = false;
            if (m_seekPending) seekNow();
            emit stateChanged();
            break;
        case MPV_EVENT_END_FILE: {
            const auto *end = static_cast<const mpv_event_end_file *>(event->data);
            if (end && end->reason == MPV_END_FILE_REASON_ERROR) {
                m_failedUrl = m_player ? m_player->currentVideoUrl() : QUrl();
                m_available = false;
                close();
                return;
            }
            break;
        }
        default:
            break;
        }
    }
}

void SeekPreview::renderFrame() {
    if (!m_render) return;
    if (!(mpv_render_context_update(m_render) & MPV_RENDER_UPDATE_FRAME)) return;
    const qreal dpr = window() ? window()->effectiveDevicePixelRatio() : 1.0;
    const QSize px = (size() * dpr).toSize();
    if (px.isEmpty()) return;
    if (m_frame.size() != px) m_frame = QImage(px, QImage::Format_RGB32);
    int dims[2] = {px.width(), px.height()};
    size_t stride = size_t(m_frame.bytesPerLine());
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_SW_SIZE, dims},
        {MPV_RENDER_PARAM_SW_FORMAT, const_cast<char *>("bgr0")},
        {MPV_RENDER_PARAM_SW_STRIDE, &stride},
        {MPV_RENDER_PARAM_SW_POINTER, m_frame.bits()},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    if (mpv_render_context_render(m_render, params) < 0) return;
    const bool first = !m_hasFrame;
    m_hasFrame = true;
    update();
    if (first) emit stateChanged();
}

void SeekPreview::paint(QPainter *painter) {
    if (!m_hasFrame) return;
    painter->setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    QPainterPath clip;
    clip.addRoundedRect(boundingRect(), 6, 6);
    painter->setClipPath(clip);
    painter->drawImage(boundingRect(), m_frame);
}
