#pragma once
#include "media/render/animejanai.h"
#include <QByteArray>
#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QSize>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>
#include <QDateTime>
#include <QHash>
#include <initializer_list>
#include <span>

class UpscalerHost {
public:
    virtual ~UpscalerHost() = default;

    virtual bool command(const QList<QByteArray> &args) = 0;
    virtual void commandAsync(const QList<QByteArray> &args) = 0;
    virtual void setPropertyAsync(const char *name, const QByteArray &value) = 0;
    virtual std::optional<int64_t> intProperty(const char *name) const = 0;
    virtual std::optional<double> doubleProperty(const char *name) const = 0;

    virtual QSize displaySize() const = 0;
    virtual bool paused() const = 0;
    // Playing in a shown window with frames arriving: drops mean something.
    virtual bool steady() const = 0;
    virtual void showText(const QString &text) = 0;
};

struct VideoFacts {
    QSize source;
    double fps = 0;
    double speed = 1;
    QSize display;

    double scale() const {
        if (source.isEmpty() || display.isEmpty()) return 0;
        return std::min(display.width() / double(source.width()), display.height() / double(source.height()));
    }
    double rate() const { return fps * speed; }
};

// A video filter: "@label:name=options", and a slot that can change in place (-1: it has none).
struct VideoFilter {
    QByteArray spec;
    int slot = -1;

    bool isEmpty() const { return spec.isEmpty(); }
    QByteArray label() const { return spec.left(spec.indexOf(':')); }
    QByteArray full() const { return slot < 0 ? spec : spec + ":slot=" + QByteArray::number(slot); }
    bool operator==(const VideoFilter &) const = default;
};

struct UpscalerPlan {
    QString shaders;
    QByteArray hwdec;
    QByteArray framedrop;  // empty: mpv's own (vo, which drops after the filters have done their work)
    VideoFilter filter;

    bool operator==(const UpscalerPlan &) const = default;
};

class UpscalerBackend : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;

    virtual QString id() const = 0;
    virtual QString name() const = 0;
    // Present on this PC: shader files there, the right graphics card, the package unpacked.
    virtual bool offered() const = 0;
    virtual bool ownsFilter() const { return false; }
    virtual QVariantMap describe() const { return {{QStringLiteral("id"), id()}, {QStringLiteral("name"), name()}}; }
    virtual UpscalerPlan plan(const VideoFacts &video, const UpscalerPlan &current) = 0;
    // When another pick (or none) is made after this one, its filter may stay in the chain, passing frames.
    virtual std::optional<UpscalerPlan> idle(const UpscalerPlan &current) { Q_UNUSED(current); return std::nullopt; }
    virtual QString notice(const VideoFacts &video) const;
    virtual void applied(const UpscalerPlan &before, const UpscalerPlan &after) { Q_UNUSED(before); Q_UNUSED(after); }
    virtual void picked(const VideoFacts &video) { Q_UNUSED(video); }
    // mpv starts a file with the filter as it was first added; `actual` says how that is.
    virtual void fileStarted(const UpscalerPlan &actual) { Q_UNUSED(actual); }
    // A seek, restart, stall or the like: what is measured next says nothing.
    virtual void restartWatch() {}

signals:
    void replan();
    void note(const QString &text);
    void statusChanged();
};

// Watches whether an upscaler keeps up: two 4 s windows in a row each losing 2+ frames and over 1% of the frames due,
// or the picture over 0.2 s behind the sound for 4 s (a filter that cannot keep up makes the video late, not drop frames).
// Not counted: the first window after a start or restart, a seek, cache stall, resize, hidden window or speed change.
class DropWatch : public QObject {
    Q_OBJECT
public:
    DropWatch(UpscalerHost &host, std::function<bool()> counts, QObject *parent = nullptr);

    static constexpr int kTickMs = 2000;

    void start() { m_timer.start(); }
    void stop() { m_timer.stop(); }
    void restart();
    void tick();

signals:
    void overloaded();

private:
    UpscalerHost &m_host;
    std::function<bool()> m_counts;
    QTimer m_timer;
    bool m_started = false;
    int m_ticks = 0;
    int64_t m_dropsAt = 0;
    int m_badWindows = 0;
    int m_lateTicks = 0;
    bool m_settling = true;
};

class Upscaler : public QObject {
    Q_OBJECT
public:
    explicit Upscaler(UpscalerHost &host);

    void init(const QByteArray &adapter);
    QVariantList upscalers() const;
    void apply();
    void pick(const QString &id);
    void onPictureChanged();
    void onStartFile();
    void onResized();
    void onSpeedChanged();
    void restartWatch();
    double frameFactor() const;

signals:
    void upscalersChanged();
    void frameRateChanged();

private:
    VideoFacts facts() const;
    static double factorOf(const UpscalerPlan &plan);
    UpscalerBackend *backend(const QString &id) const;
    void refreshOffers();
    void setShaders(const QString &shaders, const QString &id);
    void setFilter(const VideoFilter &filter);

    UpscalerHost &m_host;
    std::vector<UpscalerBackend *> m_backends;
    QList<bool> m_offered;
    QTimer m_refresh;
    QString m_id;
    UpscalerPlan m_current;
    int m_addedSlot = -1;
};

// C4F16 looks nearly as good as C4F32 in a sixth of the time (4 ms a 1080p frame to 23, RTX 5080 Laptop).
struct ShaderChain {
    const char *id;
    const char *name;
    std::initializer_list<const char *> files;
    std::initializer_list<const char *> lighter;
};

namespace ShaderChains {
std::span<const ShaderChain> all();

// The upstream shaders start their upscaling passes at 1.2x (Anime4K) or 1.3x (ArtCNN) of the video and idle below that.
// The copy made of one starts them at 0.99x, so the pick works at the video's own size too. Empty when there is no gate.
QString lowerScaleGate(const QString &glsl);
}

class ShaderBackend : public UpscalerBackend {
    Q_OBJECT
public:
    ShaderBackend(UpscalerHost &host, const ShaderChain &chain, QObject *parent = nullptr);

    QString id() const override { return QLatin1String(m_chain.id); }
    QString name() const override { return QLatin1String(m_chain.name); }
    bool offered() const override;
    UpscalerPlan plan(const VideoFacts &video, const UpscalerPlan &current) override;
    QString notice(const VideoFacts &video) const override;
    void applied(const UpscalerPlan &before, const UpscalerPlan &after) override;
    void picked(const VideoFacts &video) override;
    void fileStarted(const UpscalerPlan &) override;
    void restartWatch() override { m_watch.restart(); }

private:
    QString shaderList(std::initializer_list<const char *> files) const;
    bool all(std::initializer_list<const char *> files) const;
    static QString kindOf(const VideoFacts &video);
    void stepDown();

    UpscalerHost &m_host;
    const ShaderChain &m_chain;
    QString m_shaders;
    int m_step = 0;                    // 0 full, 1 lighter, 2 off: stepped down for the video playing
    QHash<QString, int> m_stepFor;
    DropWatch m_watch;
};

class VsrBackend : public UpscalerBackend {
    Q_OBJECT
public:
    explicit VsrBackend(const QByteArray &mode, QObject *parent = nullptr);

    static double scale(double fit, int64_t w, int64_t h);

    QString id() const override { return QStringLiteral("vsr"); }
    QString name() const override;
    bool offered() const override { return !m_mode.isEmpty(); }
    bool ownsFilter() const override { return true; }
    QVariantMap describe() const override;
    UpscalerPlan plan(const VideoFacts &video, const UpscalerPlan &current) override;

private:
    QByteArray m_mode;
};

class AnimeJaNaiBackend : public UpscalerBackend {
    Q_OBJECT
public:
    AnimeJaNaiBackend(UpscalerHost &host, bool hasFilter, bool tensorRt, QObject *parent = nullptr);

    QString id() const override { return QStringLiteral("animejanai"); }
    QString name() const override { return QStringLiteral("AnimeJaNai"); }
    bool offered() const override { return m_hasFilter && m_aji.usable(); }
    bool ownsFilter() const override { return true; }
    UpscalerPlan plan(const VideoFacts &video, const UpscalerPlan &current) override;
    std::optional<UpscalerPlan> idle(const UpscalerPlan &current) override;
    QString notice(const VideoFacts &video) const override;
    void applied(const UpscalerPlan &before, const UpscalerPlan &after) override;
    void picked(const VideoFacts &video) override;
    void fileStarted(const UpscalerPlan &actual) override;
    void restartWatch() override;
    void pollStatus();
    void stepDownForTest() { stepDown(); }
    QVariantMap describe() const override;

private:
    enum class Engines { Unknown, Building, Ready, Failed };
    void watchEngines();

    UpscalerPlan planFor(int slot) const;
    QList<int> ladder(const VideoFacts &video) const;
    int slotFor(const VideoFacts &video) const;
    QString steppedNotice(const VideoFacts &video) const;
    int ceilingOf(const VideoFacts &video) const;
    static int weight(int slot);
    void stepDown();
    static QString kindOf(const VideoFacts &video);
    static double headroom();
    void rememberCeilings() const;

    UpscalerHost &m_host;
    AnimeJaNai m_aji;
    bool m_hasFilter;

    static constexpr int kNoCeiling = -1;
    int m_slot = 0;
    // The heaviest slot the video playing may use, once it dropped frames with a heavier one; a slot, not a count
    // of steps, so it still means the same after Frame interpolation, the speed or the power source changes.
    int m_ceiling = kNoCeiling;
    QHash<QString, int> m_ceilingFor;
    DropWatch m_watch;

    QString m_statsPath;
    QTimer m_statusTimer;
    QDateTime m_since;                // what the filter wrote before this is from an earlier run
    Engines m_engines = Engines::Unknown;
    int m_polls = 0;
};
