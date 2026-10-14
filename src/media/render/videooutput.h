#pragma once
#include "media/player/mpv.h"
#include <QByteArray>
#include <QElapsedTimer>
#include <QMutex>
#include <QObject>
#include <QRect>
#include <QSize>
#include <QString>
#include <QTimer>
#include <atomic>
#include <functional>
#include <memory>
#include <QStringList>

// one d3dcompiler everywhere: libplacebo keys shaders by its version
namespace VideoOutput {

QByteArray highPerformanceAdapter();

QString shaderCompilerPath();

void loadShaderCompiler();

QByteArray setOptions(const Mpv::Handle &mpv);

void syncConfig(const QString &shippedDir, const QString &userDir);

}

class QQuickItem;
class QQuickWindow;
class VideoUnderlay;
struct IDXGISwapChain1;

class VideoComposition : public QObject {
    Q_OBJECT
public:
    VideoComposition(QQuickItem &item, Mpv::Handle &mpv, std::function<QSize()> shape);
    ~VideoComposition() override;

    void attach(QQuickWindow *window);
    void takeSwapchain();
    void itemResized() { m_resize.start(); }
    QSize size() const { return m_size; }
    bool shown() const { return m_shown.load(std::memory_order_relaxed); }
    bool stale() const { return m_stale.load(std::memory_order_relaxed); }
    void release();

signals:
    void resized();

private:
    void resize();

    QQuickItem &m_item;
    Mpv::Handle &m_mpv;
    std::function<QSize()> m_shape;

    // m_underlay and m_rect: render thread only; swapchain goes via m_mutex
    std::unique_ptr<VideoUnderlay> m_underlay;
    QMutex m_mutex;
    IDXGISwapChain1 *m_swapchain = nullptr;
    QRect m_rect;
    std::atomic<bool> m_shown{false};
    std::atomic<bool> m_stale{false};
    QTimer m_kick;
    QElapsedTimer m_kickSince;
    QSize m_size;
    QTimer m_resize;
};

class QQuickWindow;
struct IDXGISwapChain1;
struct IDCompositionDevice;
struct IDCompositionTarget;
struct IDCompositionVisual;
struct ID3D11Device;
struct ID3D11DeviceContext1;

class VideoUnderlay {
public:
    ~VideoUnderlay() { release(); }

    bool sync(QQuickWindow *window, IDXGISwapChain1 *video, const QRect &deviceRect, QSize display);
    void release();
    bool stale() const { return m_shown != m_buffer; }
    // DirectComposition needs a commit; Qt commits only its own
    void commit();

private:
    // Qt's, never AddRef'd, or Qt cannot re-create its target
    IDCompositionDevice *m_device = nullptr;
    IDCompositionTarget *m_target = nullptr;
    IDCompositionVisual *m_qtVisual = nullptr;
    bool makeBlack(ID3D11Device *dev, ID3D11DeviceContext1 *context);

    IDCompositionVisual *m_root = nullptr;
    IDCompositionVisual *m_clip = nullptr;
    IDCompositionVisual *m_black = nullptr;
    IDCompositionVisual *m_video = nullptr;
    IDXGISwapChain1 *m_blackChain = nullptr;
    IDXGISwapChain1 *m_content = nullptr;
    QRect m_rect;
    QSize m_buffer;
    QSize m_shown;
    QSize m_placed, m_display;
    unsigned m_resizePresents = 0;
    QElapsedTimer m_resizedAt;
    bool m_failed = false;
    bool m_dirty = false;
};

class QQuickWindow;

namespace ShaderCache {
void seed();
QString fingerprint(const QString &configDir);
bool startWarmup(const QQuickWindow *window, bool playing);
int warm(const QStringList &args);
}
