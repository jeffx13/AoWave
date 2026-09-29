#pragma once
#include <QImage>
#include <QQuickPaintedItem>
#include <QTimer>
#include <QUrl>
#include <mpv/client.h>
#include <mpv/render.h>

class MpvPlayer;

// A second libmpv handle on the same file, rendered small through the software API.
class SeekPreview : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(MpvPlayer *player READ player WRITE setPlayer NOTIFY playerChanged)
    Q_PROPERTY(double time READ time WRITE setTime NOTIFY timeChanged)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(bool hasFrame READ hasFrame NOTIFY stateChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY stateChanged)
    Q_PROPERTY(bool available READ available NOTIFY stateChanged)
    Q_PROPERTY(double frameAspect READ frameAspect NOTIFY stateChanged)
public:
    explicit SeekPreview(QQuickItem *parent = nullptr);
    ~SeekPreview() override;

    MpvPlayer *player() const { return m_player; }
    void setPlayer(MpvPlayer *player);
    double time() const { return m_time; }
    void setTime(double seconds);
    bool active() const { return m_active; }
    void setActive(bool active);
    bool hasFrame() const { return m_hasFrame; }
    bool loading() const { return m_active && m_available && m_mpv && (!m_loaded || m_seekPending || m_seeking); }
    bool available() const { return m_available; }
    double frameAspect() const { return m_frameAspect; }

    void paint(QPainter *painter) override;

    // Called from libmpv's threads through queued invocations.
    void drainEvents();
    void renderFrame();

signals:
    void playerChanged();
    void timeChanged();
    void activeChanged();
    void stateChanged();

private:
    void open();
    void close();
    void reset();
    void seekNow();

    MpvPlayer *m_player = nullptr;
    mpv_handle *m_mpv = nullptr;
    mpv_render_context *m_render = nullptr;
    QUrl m_failedUrl;
    QImage m_frame;
    QTimer m_debounce;
    double m_time = -1;
    double m_shownTime = -1;
    double m_frameAspect = 16.0 / 9.0;
    bool m_active = false;
    bool m_available = true;
    bool m_loaded = false;
    bool m_seeking = false;
    bool m_seekPending = false;
    bool m_hasFrame = false;
};
