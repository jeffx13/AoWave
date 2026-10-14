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

// How the video gets to the screen: Direct3D 11 through mpv's composition swapchain, on the strongest graphics
// card, compiling shaders with one d3dcompiler on every PC (libplacebo keys each compiled shader by its version).
namespace VideoOutput {

QByteArray highPerformanceAdapter();

QString shaderCompilerPath();

// Loaded by full path before Qt's renderer or libplacebo asks for it by name.
void loadShaderCompiler();

QByteArray setOptions(const Mpv::Handle &mpv);

void syncConfig(const QString &shippedDir, const QString &userDir);

}

class QQuickItem;
class QQuickWindow;
class VideoUnderlay;
struct IDXGISwapChain1;

// The render thread places the video (VideoUnderlay); everything else is the GUI thread's.
class VideoComposition : public QObject {
    Q_OBJECT
public:
    VideoComposition(QQuickItem &item, Mpv::Handle &mpv, std::function<QSize()> shape);
    ~VideoComposition() override;

    void attach(QQuickWindow *window);
    // After a video reconfig: mpv may have made a new swapchain.
    void takeSwapchain();
    void itemResized() { m_resize.start(); }
    QSize size() const { return m_size; }
    // The swapchain is under the window, so the item is a hole in the scene; else it is black.
    bool shown() const { return m_shown.load(std::memory_order_relaxed); }
    bool stale() const { return m_stale.load(std::memory_order_relaxed); }
    // Before the item and window go: the render thread's side is released there.
    void release();

signals:
    void resized();

private:
    void resize();

    QQuickItem &m_item;
    Mpv::Handle &m_mpv;
    std::function<QSize()> m_shape;

    // m_underlay and m_rect are the render thread's; the swapchain comes over via m_mutex.
    std::unique_ptr<VideoUnderlay> m_underlay;
    QMutex m_mutex;
    IDXGISwapChain1 *m_swapchain = nullptr;   // AddRef'd
    QRect m_rect;
    std::atomic<bool> m_shown{false};
    std::atomic<bool> m_stale{false};
    // Qt draws only on change; after a resize Qt is asked to draw until mpv has shown the new size.
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
    // DirectComposition shows property changes only once committed, and Qt commits only its own.
    void commit();

private:
    // Qt's, never AddRef'd: holding them would keep Qt from re-creating its target.
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
// False when it should be tried again later: on the battery, or while a video plays, it would only slow the playback.
bool startWarmup(const QQuickWindow *window, bool playing);
int warm(const QStringList &args);
}
