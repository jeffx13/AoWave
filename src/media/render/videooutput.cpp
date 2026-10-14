#include "media/render/videooutput.h"
#include "app/logger.h"
#include "app/settings.h"
#include "media/render/upscaler.h"
#include "system/platform.h"
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRunnable>
#include <QtGui/private/qrhid3d11_p.h>
#include <cmath>
#include <d3d11_1.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <dxgi1_6.h>
#include <rhi/qrhi.h>
#include <windows.h>
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QProcess>
#include <QScopeGuard>
#include <QScreen>
#include <QtEndian>

QByteArray VideoOutput::highPerformanceAdapter() {
    IDXGIFactory6 *factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return {};
    QByteArray name;
    IDXGIAdapter1 *adapter = nullptr;
    if (SUCCEEDED(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)))) {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            name = QString::fromWCharArray(desc.Description).toUtf8();
        adapter->Release();
    }
    factory->Release();
    return name;
}

QString VideoOutput::shaderCompilerPath() {
    const QString bundled = Settings::toolPath(QStringLiteral("d3dcompiler_47.dll"));
    if (QFileInfo::exists(bundled)) return bundled;
    wchar_t system[MAX_PATH] = {};
    GetSystemDirectoryW(system, MAX_PATH);
    return QString::fromWCharArray(system) + QStringLiteral("\\d3dcompiler_47.dll");
}

void VideoOutput::loadShaderCompiler() {
    if (const QString compiler = Settings::toolPath(QStringLiteral("d3dcompiler_47.dll")); QFileInfo::exists(compiler))
        LoadLibraryW(reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(compiler).utf16()));
}

QByteArray VideoOutput::setOptions(const Mpv::Handle &mpv) {
    const QDir mpvDir(Settings::dataDir() + QStringLiteral("/mpv"));
    if (mpvDir.exists()) {
        mpv.set_option("config-dir", mpvDir.absolutePath().toUtf8().constData());
        mpv.set_option("config", "yes");
    }
    mpv.set_option("gpu-shader-cache-dir", (Settings::tempDir() + QStringLiteral("/mpv")).toUtf8().constData());
    mpv.set_option("vo", "gpu-next");
    mpv.set_option("gpu-api", "d3d11");
    mpv.set_option("gpu-context", "d3d11");
    mpv.set_option("d3d11-output-mode", "composition");
    mpv.set_option("force-window", "yes");
    // A hybrid laptop's default adapter is its integrated GPU, which has no RTX Video.
    const QByteArray gpu = highPerformanceAdapter();
    if (!gpu.isEmpty()) mpv.set_option("d3d11-adapter", gpu.constData());
    return gpu;
}

VideoComposition::VideoComposition(QQuickItem &item, Mpv::Handle &mpv, std::function<QSize()> shape)
    : m_item(item), m_mpv(mpv), m_shape(std::move(shape)) {
    m_resize.setSingleShot(true);
    m_resize.setInterval(250);
    connect(&m_resize, &QTimer::timeout, this, &VideoComposition::resize);
    m_kick.setInterval(16);
    connect(&m_kick, &QTimer::timeout, this, [this] {
        const qint64 since = m_kickSince.elapsed();
        if ((since > 150 && !m_stale) || since > 3000 || !m_item.window()) {
            m_kick.stop();
            return;
        }
        m_item.window()->update();
    });
}

VideoComposition::~VideoComposition() {
    release();
}

// The underlay belongs to the render thread; the scene graph usually released it already. No frame starts on
// it after the disconnect, and the lock waits out one under way.
void VideoComposition::release() {
    QQuickWindow *win = m_item.window();
    if (win) win->disconnect(this);
    QMutexLocker lock(&m_mutex);
    if (win && m_underlay) {
        VideoUnderlay *underlay = m_underlay.release();
        win->scheduleRenderJob(QRunnable::create([underlay] { delete underlay; }), QQuickWindow::NoStage);
    }
    if (m_swapchain) m_swapchain->Release();
    m_swapchain = nullptr;
}

void VideoComposition::attach(QQuickWindow *window) {
    if (!window) return;
    window->setPersistentGraphics(true);
    window->setPersistentSceneGraph(true);
    // Read while the GUI thread waits: an item that moves with its parent gets no updatePaintNode.
    connect(window, &QQuickWindow::beforeSynchronizing, this, [this, window] {
        const qreal dpr = window->effectiveDevicePixelRatio();
        const QRectF rect = m_item.mapRectToScene(m_item.boundingRect());
        m_rect = QRect(qRound(rect.x() * dpr), qRound(rect.y() * dpr),
                       qRound(rect.width() * dpr), qRound(rect.height() * dpr));
    }, Qt::DirectConnection);
    connect(window, &QQuickWindow::beforeRendering, this, [this, window] {
        QMutexLocker lock(&m_mutex);
        if (!m_underlay) m_underlay = std::make_unique<VideoUnderlay>();
        const bool shown = m_underlay->sync(window, m_swapchain, m_rect, m_shape()) && m_swapchain;
        m_stale = m_underlay->stale();
        if (shown != m_shown.exchange(shown))
            QMetaObject::invokeMethod(&m_item, [this] { m_item.update(); }, Qt::QueuedConnection);
    }, Qt::DirectConnection);
    connect(window, &QQuickWindow::afterFrameEnd, this, [this] {
        QMutexLocker lock(&m_mutex);
        if (m_underlay) m_underlay->commit();
    }, Qt::DirectConnection);
    connect(window, &QQuickWindow::sceneGraphInvalidated, this, [this] {
        QMutexLocker lock(&m_mutex);
        if (m_underlay) m_underlay->release();
        m_shown = false;
    }, Qt::DirectConnection);
    connect(window, &QWindow::screenChanged, this, [this] { m_resize.start(); });
    m_resize.start();
}

void VideoComposition::takeSwapchain() {
    int64_t address = 0;
    m_mpv.get_int64("display-swapchain", address);
    IDXGISwapChain1 *swapchain = nullptr;
    if (auto *any = reinterpret_cast<IDXGISwapChain *>(address))
        any->QueryInterface(IID_PPV_ARGS(&swapchain));
    {
        QMutexLocker lock(&m_mutex);
        if (swapchain == m_swapchain) {
            if (swapchain) swapchain->Release();
            return;
        }
        if (m_swapchain) m_swapchain->Release();
        m_swapchain = swapchain;
    }
    if (m_item.window()) m_item.window()->update();
}

void VideoComposition::resize() {
    QQuickWindow *window = m_item.window();
    if (!window || QGuiApplication::platformName() == QLatin1String("offscreen")) return;
    const QSize size = (QSizeF(m_item.width(), m_item.height()) * window->effectiveDevicePixelRatio()).toSize()
                           .expandedTo(QSize(16, 16));
    if (size == m_size) return;
    m_size = size;
    const QByteArray box = QByteArray::number(size.width()) + 'x' + QByteArray::number(size.height());
    m_mpv.set_property_async("d3d11-composition-size", box.constData());
    m_kickSince.start();
    m_kick.start();
    emit resized();
}

namespace {
// QRhiResource keeps its QRhiImplementation protected; a pointer to member reaches it legally.
struct RhiOf : QRhiResource {
    static constexpr QRhiImplementation *QRhiResource::*member() { return &RhiOf::m_rhi; }
};
}

bool VideoUnderlay::sync(QQuickWindow *window, IDXGISwapChain1 *video, const QRect &deviceRect, QSize display) {
    QRhiSwapChain *swapChain = window ? window->swapChain() : nullptr;
    if (!swapChain || !window->rhi() || window->rhi()->backend() != QRhi::D3D11) return false;
    auto *qtSwapChain = static_cast<QD3D11SwapChain *>(swapChain);
    auto *rhi = static_cast<QRhiD3D11 *>(swapChain->*RhiOf::member());
    IDCompositionDevice *device = rhi->dcompDevice;
    if (!device || !qtSwapChain->dcompTarget || !qtSwapChain->dcompVisual) return false;

    if (device != m_device || qtSwapChain->dcompTarget != m_target || qtSwapChain->dcompVisual != m_qtVisual) {
        release();
        m_device = device;
        m_target = qtSwapChain->dcompTarget;
        m_qtVisual = qtSwapChain->dcompVisual;
        const bool built = SUCCEEDED(device->CreateVisual(&m_root)) && SUCCEEDED(device->CreateVisual(&m_clip))
                           && SUCCEEDED(device->CreateVisual(&m_black)) && SUCCEEDED(device->CreateVisual(&m_video))
                           && makeBlack(rhi->dev, rhi->context)
                           && SUCCEEDED(m_target->SetRoot(nullptr))
                           && SUCCEEDED(m_clip->AddVisual(m_black, FALSE, nullptr))
                           && SUCCEEDED(m_clip->AddVisual(m_video, TRUE, m_black))
                           && SUCCEEDED(m_root->AddVisual(m_clip, FALSE, nullptr))
                           && SUCCEEDED(m_root->AddVisual(m_qtVisual, TRUE, m_clip))
                           && SUCCEEDED(m_target->SetRoot(m_root));
        m_failed = !built;
        m_dirty = true;
        if (m_failed) {
            logWarn() << "Player" << "could not place the video under the window; it shows no video";
            if (m_root) m_root->RemoveAllVisuals();
            m_target->SetRoot(m_qtVisual);
        }
    }
    if (m_failed) return false;

    if (video != m_content) {
        m_dirty = true;
        m_video->SetContent(video);
        m_content = video;
        m_buffer = m_shown = m_placed = {};
    }
    // mpv resizes on its own thread: until it shows the new size, the frame on screen is the old one.
    DXGI_SWAP_CHAIN_DESC1 desc{};
    const QSize buffer = video && SUCCEEDED(video->GetDesc1(&desc)) ? QSize(int(desc.Width), int(desc.Height)) : QSize();
    UINT presents = 0;
    if (video) video->GetLastPresentCount(&presents);
    if (buffer != m_buffer) {
        m_buffer = buffer;
        m_resizePresents = presents;
        m_resizedAt.start();
    }
    if (m_shown != m_buffer && (m_shown.isEmpty() || presents != m_resizePresents || m_resizedAt.elapsed() > 250))
        m_shown = m_buffer;

    if (deviceRect == m_rect && m_shown == m_placed && display == m_display) return true;
    m_rect = deviceRect;
    m_placed = m_shown;
    m_display = display;
    const QSizeF frame = m_shown.isEmpty() ? QSizeF(deviceRect.size()) : QSizeF(m_shown);
    const QSizeF shape = display.isEmpty() ? frame : QSizeF(display);
    double scale = 1;
    if (frame.toSize() != deviceRect.size() && frame.width() > 0 && frame.height() > 0) {
        const double inFrame = std::min(frame.width() / shape.width(), frame.height() / shape.height());
        const double inItem = std::min(deviceRect.width() / shape.width(), deviceRect.height() / shape.height());
        scale = inFrame > 0 ? inItem / inFrame : 1;
    }
    D2D_MATRIX_3X2_F place{};
    place._11 = place._22 = float(scale);
    place._31 = float(std::round(deviceRect.x() + deviceRect.width() / 2.0 - scale * frame.width() / 2.0));
    place._32 = float(std::round(deviceRect.y() + deviceRect.height() / 2.0 - scale * frame.height() / 2.0));
    m_video->SetOffsetX(0.f);
    m_video->SetOffsetY(0.f);
    m_video->SetTransform(place);
    D2D_MATRIX_3X2_F fill{};
    fill._11 = float(deviceRect.width()) / 8.f;
    fill._22 = float(deviceRect.height()) / 8.f;
    fill._31 = float(deviceRect.x());
    fill._32 = float(deviceRect.y());
    m_black->SetTransform(fill);
    m_clip->SetClip(D2D_RECT_F{float(deviceRect.left()), float(deviceRect.top()),
                               float(deviceRect.right() + 1), float(deviceRect.bottom() + 1)});
    m_dirty = true;
    return true;
}

void VideoUnderlay::commit() {
    if (m_dirty && m_device && !m_failed) m_device->Commit();
    m_dirty = false;
}

// Own swapchain, cleared once: Qt's composition device has no rendering device to make drawing surfaces.
bool VideoUnderlay::makeBlack(ID3D11Device *dev, ID3D11DeviceContext1 *context) {
    if (!dev || !context) return false;
    IDXGIDevice *dxgi = nullptr;
    IDXGIAdapter *adapter = nullptr;
    IDXGIFactory2 *factory = nullptr;
    bool made = SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dxgi))) && SUCCEEDED(dxgi->GetAdapter(&adapter))
                && SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory)));
    if (made) {
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = desc.Height = 8;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        made = SUCCEEDED(factory->CreateSwapChainForComposition(dev, &desc, nullptr, &m_blackChain));
    }
    if (factory) factory->Release();
    if (adapter) adapter->Release();
    if (dxgi) dxgi->Release();
    ID3D11Texture2D *buffer = nullptr;
    ID3D11RenderTargetView *view = nullptr;
    made = made && SUCCEEDED(m_blackChain->GetBuffer(0, IID_PPV_ARGS(&buffer)))
           && SUCCEEDED(dev->CreateRenderTargetView(buffer, nullptr, &view));
    if (made) {
        const float black[4] = {0.f, 0.f, 0.f, 1.f};
        context->ClearRenderTargetView(view, black);
        made = SUCCEEDED(m_blackChain->Present(0, 0));
    }
    if (view) view->Release();
    if (buffer) buffer->Release();
    return made && SUCCEEDED(m_black->SetContent(m_blackChain));
}

void VideoUnderlay::release() {
    if (m_root) {
        m_root->RemoveAllVisuals();
        m_root->Release();
    }
    if (m_clip) {
        m_clip->RemoveAllVisuals();
        m_clip->Release();
    }
    if (m_black) m_black->Release();
    if (m_video) m_video->Release();
    if (m_blackChain) m_blackChain->Release();
    m_root = m_clip = m_black = m_video = nullptr;
    m_blackChain = nullptr;
    m_device = nullptr;
    m_target = nullptr;
    m_qtVisual = nullptr;
    m_content = nullptr;
    m_rect = {};
    m_buffer = m_shown = m_placed = m_display = {};
    m_failed = false;
}

namespace {
QString hashOf(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QString::fromLatin1(QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha1).toHex());
}

bool copyOver(const QString &from, const QString &to) {
    QDir().mkpath(QFileInfo(to).path());
    QFile::remove(to);
    return QFile::copy(from, to);
}
}

void VideoOutput::syncConfig(const QString &shippedDir, const QString &userDir) {
    // Without the shipped folder (a broken package, a build being copied) nothing is known to have been dropped.
    if (!QDir(shippedDir).exists()) return;
    const QString manifestPath = userDir + QStringLiteral("/.shipped.json");
    QFile manifestFile(manifestPath);
    QJsonObject copied;
    if (manifestFile.open(QIODevice::ReadOnly)) copied = QJsonDocument::fromJson(manifestFile.readAll()).object();
    QJsonObject now;

    QDirIterator files(shippedDir, QDir::Files, QDirIterator::Subdirectories);
    const QDir shipped(shippedDir);
    while (files.hasNext()) {
        const QString source = files.next();
        const QString name = shipped.relativeFilePath(source);
        const QString user = userDir + QLatin1Char('/') + name;
        const QString shippedHash = hashOf(source);
        const QString lastCopied = copied.value(name).toString();
        const bool data = name.startsWith(QLatin1String("shaders/"));
        if (!QFileInfo::exists(user) || data) {
            if (hashOf(user) != shippedHash && !copyOver(source, user)) logWarn() << "Player" << "could not copy" << name << "to the mpv folder";
            now.insert(name, shippedHash);
        } else if (hashOf(user) == shippedHash) {
            now.insert(name, shippedHash);
            QFile::remove(user + QStringLiteral(".new"));
        } else if (!lastCopied.isEmpty() && hashOf(user) == lastCopied) {
            if (copyOver(source, user)) now.insert(name, shippedHash);
            else now.insert(name, lastCopied);
            logInfo() << "Player" << QStringLiteral("mpv folder: %1 updated").arg(name);
        } else {
            // Edited by the user: theirs stays, and the new default is beside it if it changed.
            now.insert(name, lastCopied);
            if (lastCopied != shippedHash && hashOf(user + QStringLiteral(".new")) != shippedHash && copyOver(source, user + QStringLiteral(".new")))
                logInfo() << "Player" << QStringLiteral("mpv folder: %1 has your changes; the new default is %1.new").arg(name);
        }
    }
    // What was shipped and is not any more goes, if it was left as it came.
    for (auto it = copied.constBegin(); it != copied.constEnd(); ++it) {
        if (now.contains(it.key())) continue;
        const QString user = userDir + QLatin1Char('/') + it.key();
        if (hashOf(user) == it.value().toString()) QFile::remove(user);
    }
    QDir().mkpath(userDir);
    QSaveFile out(manifestPath);
    if (out.open(QIODevice::WriteOnly)) {
        out.write(QJsonDocument(now).toJson(QJsonDocument::Compact));
        out.commit();
    }
}

namespace {

QString buildStamp(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return QStringLiteral("none");
    const QByteArray head = file.read(4096);
    const quint32 pe = head.size() >= 0x40 ? qFromLittleEndian<quint32>(head.constData() + 0x3C) : 0;
    if (pe == 0 || pe + 12 > quint32(head.size()) || !head.mid(pe, 4).startsWith("PE"))
        return QString::number(file.size());
    return QString::number(qFromLittleEndian<quint32>(head.constData() + pe + 8), 16) + QLatin1Char('/')
           + QString::number(file.size());
}

// What the slow shaders depend on, so a warm-up reruns only when one changes: libmpv, its compiler, the
// shader text and the GPU. Not video or window size: measured, those add a second or two.
QString shaderFingerprint(const QString &configDir) {
    QCryptographicHash shaders(QCryptographicHash::Sha1);
    for (const ShaderChain &chain : ShaderChains::all())
        for (const char *file : chain.files) {
            QFile glsl(configDir + QStringLiteral("/shaders/") + QLatin1String(file));
            if (glsl.open(QIODevice::ReadOnly)) shaders.addData(glsl.readAll());
        }
    const QStringList parts{buildStamp(Platform::moduleDir() + QStringLiteral("/libmpv-2.dll")),
                            buildStamp(VideoOutput::shaderCompilerPath()), QString::fromLatin1(shaders.result().toHex()),
                            QString::fromUtf8(VideoOutput::highPerformanceAdapter())};
    return QString::fromLatin1(QCryptographicHash::hash(parts.join(QLatin1Char('|')).toUtf8(),
                                                        QCryptographicHash::Sha1).toHex());
}

QByteArray readSmallFile(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.read(256) : QByteArray();
}

const QStringList kWarmSources{QStringLiteral("640x480"), QStringLiteral("720x480"), QStringLiteral("1024x576"),
                                      QStringLiteral("1280x720"), QStringLiteral("1920x1080"),
                                      QStringLiteral("1920x1080p10")};

}

void ShaderCache::seed() {
    const QString seed = Settings::toolPath(QStringLiteral("shader-cache"));
    const QByteArray made = readSmallFile(seed + QStringLiteral("/warmed.txt"));
    if (made.isEmpty()) return;
    const QString cache = Settings::tempDir() + QStringLiteral("/mpv");
    if (readSmallFile(cache + QStringLiteral("/seeded.txt")) == made) return;
    QDir().mkpath(cache);
    for (const QFileInfo &file : QDir(seed).entryInfoList({QStringLiteral("shader_*")}, QDir::Files))
        if (!QFileInfo::exists(cache + QLatin1Char('/') + file.fileName()))
            QFile::copy(file.filePath(), cache + QLatin1Char('/') + file.fileName());
    QFile seeded(cache + QStringLiteral("/seeded.txt"));
    if (seeded.open(QIODevice::WriteOnly)) seeded.write(made);
}

bool ShaderCache::startWarmup(const QQuickWindow *shown, bool playing) {
    // It runs the app's own exe again; a test program would start its whole run instead.
    if (QFileInfo(QCoreApplication::applicationFilePath()).completeBaseName() != QLatin1String(APP_NAME)) return true;
    const QString cache = Settings::tempDir() + QStringLiteral("/mpv");
    const QString config = Settings::dataDir() + QStringLiteral("/mpv");
    if (readSmallFile(cache + QStringLiteral("/warmed.txt")) == shaderFingerprint(config).toLatin1()) return true;
    const QFileInfo lock(cache + QStringLiteral("/warming.lock"));
    if (lock.exists() && lock.lastModified().secsTo(QDateTime::currentDateTime()) < 1800) return true;
    if (playing || Platform::onBattery()) return false;
    const QScreen *screen = shown ? shown->screen() : QGuiApplication::primaryScreen();
    if (!screen) return true;
    const qreal dpr = screen->devicePixelRatio();
    QStringList sizes{QStringLiteral("%1x%2").arg(qRound(screen->size().width() * dpr)).arg(qRound(screen->size().height() * dpr))};
    if (shown && shown->visibility() == QWindow::Windowed)
        sizes << QStringLiteral("%1x%2").arg(qRound(shown->width() * dpr)).arg(qRound(shown->height() * dpr));
    logInfo() << "Player" << "compiling the upscalers' shaders in the background for" << sizes.join(QLatin1Char(' '));
    QProcess::startDetached(QCoreApplication::applicationFilePath(),
                            QStringList{QStringLiteral("--warm-shaders"), cache, config, kWarmSources.join(QLatin1Char(','))}
                                + sizes);
    return true;
}

// `AoWave.exe --warm-shaders <cache> <config> <clips> <WxH>...`: a hidden low-priority player that
// fills the shader cache ahead of need, then writes the fingerprint.
int ShaderCache::warm(const QStringList &args) {
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    if (args.size() < 4) return 2;
    const QString cacheDir = args[0], configDir = args[1];
    QDir().mkpath(cacheDir);
    QFile lock(cacheDir + QStringLiteral("/warming.lock"));
    if (!lock.open(QIODevice::WriteOnly)) return 1;
    lock.close();
    const auto cleanup = qScopeGuard([&lock] { lock.remove(); });

    const QString dir = Settings::tempDir() + QStringLiteral("/warmup");
    QDir().mkpath(dir);
    const QStringList sources = args[2].split(QLatin1Char(','), Qt::SkipEmptyParts);
    QStringList clips;
    for (const QString &size : sources) {
        const QString clip = dir + QStringLiteral("/%1.mp4").arg(size);
        const bool deep = size.endsWith(QLatin1String("p10"));
        if (!QFile::exists(clip))
            QProcess::execute(Settings::toolPath(QStringLiteral("ffmpeg.exe")),
                              {"-loglevel", "error", "-y", "-f", "lavfi", "-i",
                               QStringLiteral("testsrc2=size=%1:rate=24:duration=4").arg(size.section(QLatin1Char('p'), 0, 0)),
                               "-c:v", deep ? "libx265" : "libx264", "-pix_fmt", deep ? "yuv420p10le" : "yuv420p", clip});
        if (QFile::exists(clip)) clips << clip;
    }

    Mpv::Handle mpv;
    VideoOutput::setOptions(mpv);
    mpv.set_option("config-dir", configDir.toUtf8().constData());
    mpv.set_option("config", "yes");
    mpv.set_option("gpu-shader-cache-dir", cacheDir.toUtf8().constData());
    mpv.set_option("d3d11-composition-size", args[3].toUtf8().constData());
    mpv.set_option("hwdec", "auto-safe");
    mpv.set_option("ao", "null");
    mpv.set_option("audio", "no");
    mpv.set_option("loop-file", "inf");
    mpv.set_option("osd-level", "0");
    mpv.set_option("ytdl", "no");
    if (mpv.initialize() < 0) return 1;
    const auto time = [&mpv] { return QString::fromStdString(mpv.get_string("playback-time")).toDouble(); };
    const auto playOn = [&](double seconds, int timeoutMs) {
        double last = time(), moved = 0;
        QElapsedTimer clock;
        clock.start();
        while (moved < seconds && clock.elapsed() < timeoutMs) {
            mpv.wait_event(0.05);
            const double now = time();
            moved += now >= last ? now - last : now;
            last = now;
        }
        return moved >= seconds;
    };
    bool complete = clips.size() == sources.size();
    // The clock runs on while the VO compiles (up to 17 s a pass), so it proves nothing: a window screenshot
    // returns only once the same shaders have rendered at this size; then a few real frames.
    const auto settle = [&] {
        const char *shot[] = {"screenshot-raw", "window", nullptr};
        mpv_node image;
        if (mpv.command_ret(shot, &image) >= 0) mpv_free_node_contents(&image);
        if (!playOn(0.3, 30000)) complete = false;
    };
    for (const QString &clip : std::as_const(clips)) {
        const QByteArray path = clip.toUtf8();
        const char *load[] = {"loadfile", path.constData(), nullptr};
        mpv.command(load);
        QElapsedTimer opening;
        opening.start();
        bool loaded = false, failed = false;
        while (!loaded && !failed && opening.elapsed() < 30000) {
            const mpv_event *event = mpv.wait_event(0.05);
            loaded = event->event_id == MPV_EVENT_FILE_LOADED;
            failed = event->event_id == MPV_EVENT_END_FILE
                     && static_cast<const mpv_event_end_file *>(event->data)->reason == MPV_END_FILE_REASON_ERROR;
        }
        if (!loaded) {
            QFile::remove(clip);
            complete = false;
            continue;
        }
        for (int i = 3; i < args.size(); ++i) {
            mpv.set_property("d3d11-composition-size", args[i].toUtf8().constData());
            playOn(0.3, 30000);
            const char *none[] = {"change-list", "glsl-shaders", "clr", "", nullptr};
            mpv.command(none);
            settle();
            for (const ShaderChain &chain : ShaderChains::all()) {
                QStringList files;
                for (const char *file : chain.files) files << QStringLiteral("~~/shaders/") + QLatin1String(file);
                const QByteArray list = files.join(QLatin1Char(';')).toUtf8();
                const char *set[] = {"change-list", "glsl-shaders", "set", list.constData(), nullptr};
                mpv.command(set);
                settle();
            }
        }
    }
    if (!complete) return 1;
    QFile done(cacheDir + QStringLiteral("/warmed.txt"));
    if (done.open(QIODevice::WriteOnly)) done.write(shaderFingerprint(configDir).toLatin1());
    return 0;
}
