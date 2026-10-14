#pragma once
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
#include <QJsonObject>
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
    virtual QByteArray jsonProperty(const char *name) const = 0;
    virtual QSize screenSize() const = 0;
    virtual bool paused() const = 0;
    virtual bool steady() const = 0;
    virtual void showText(const QString &text) = 0;
};

struct VideoFacts {
    QSize source;
    double fps = 0;
    double speed = 1;
    QSize display;
    QSize screen;

    double scale() const {
        if (source.isEmpty() || display.isEmpty()) return 0;
        return std::min(display.width() / double(source.width()), display.height() / double(source.height()));
    }
    double rate() const { return fps * speed; }
};

struct VideoFilter {
    QByteArray spec;
    int slot = -1;
    int revision = 0;
    double frames = 1.0;

    bool isEmpty() const { return spec.isEmpty(); }
    QByteArray label() const { return spec.left(spec.indexOf(':')); }
    QByteArray full() const { return slot < 0 ? spec : spec + ":slot=" + QByteArray::number(slot); }
    bool operator==(const VideoFilter &) const = default;
};

struct UpscalerPlan {
    QString shaders;
    QByteArray hwdec;
    QByteArray framedrop;
    VideoFilter filter;

    bool operator==(const UpscalerPlan &) const = default;
};

class UpscalerCosts;

class UpscalerBackend : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;

    virtual QString id() const = 0;
    virtual QString name() const = 0;
    virtual bool offered() const = 0;
    virtual bool ownsFilter() const { return false; }
    virtual QString stepName() const { return {}; }
    virtual QString stepLabel() const { return {}; }
    virtual QString status(const VideoFacts &video) const { Q_UNUSED(video); return {}; }
    virtual QVariantMap describe() const { return {{QStringLiteral("id"), id()}, {QStringLiteral("name"), name()}}; }
    virtual UpscalerPlan plan(const VideoFacts &video, const UpscalerPlan &current) = 0;
    virtual std::optional<UpscalerPlan> idle(const UpscalerPlan &current) { Q_UNUSED(current); return std::nullopt; }
    virtual QString notice(const VideoFacts &video) const;
    virtual void applied(const UpscalerPlan &before, const UpscalerPlan &after) { Q_UNUSED(before); Q_UNUSED(after); }
    virtual void picked(const VideoFacts &video) { Q_UNUSED(video); }
    virtual void fileStarted(const UpscalerPlan &actual) { Q_UNUSED(actual); }
    virtual void restartWatch() {}
    virtual void useCosts(UpscalerCosts *costs) { Q_UNUSED(costs); }

signals:
    void replan();
    void note(const QString &text);
    void statusChanged();
};

struct LoadSample {
    double fresh = 0;
    double freshPeak = 0;
    double redraw = 0;
    double fpsEff = 24;
    double utilization = 0;
    double vfFps = 0;
    int64_t counters = 0;
    bool settled = false;
};
Q_DECLARE_METATYPE(LoadSample)

// overloaded: 3 ticks over 90% load, 2 drops in 4, or 4 ticks 0.2 s late
class LoadWatch : public QObject {
    Q_OBJECT
public:
    enum class Kind { Shaders, Filter };

    LoadWatch(UpscalerHost &host, Kind kind, std::function<bool()> counts, std::function<double()> frameFactor = {},
              QObject *parent = nullptr);

    static constexpr int kTickMs = 500;
    static constexpr int kSettleTicks = 4;
    static constexpr int kRoomTicks = 120;

    void start() { m_timer.start(); }
    void stop() { m_timer.stop(); }
    void restart();
    void tick();
    int age() const { return m_age; }
    void resetAge() { m_age = 0; }
    double utilization() const { return m_last.utilization; }
    const LoadSample &last() const { return m_last; }

    // vo-passes JSON in ns, the result in ms
    static void passTimes(const QByteArray &json, double &fresh, double &freshPeak, double &redraw);

signals:
    void overloaded();
    void roomToGrow();
    void sampled(const LoadSample &sample);

private:
    LoadSample take(double speed, int64_t counters) const;

    UpscalerHost &m_host;
    Kind m_kind;
    std::function<bool()> m_counts;
    std::function<double()> m_factor;
    QTimer m_timer;
    bool m_started = false;
    int m_settle = kSettleTicks;
    int m_age = 0;
    int64_t m_drops = 0, m_delayed = 0, m_mistimed = 0, m_counters = 0;
    int m_added[4] = {0, 0, 0, 0};
    int m_hot = 0, m_slow = 0, m_late = 0, m_room = 0;
    LoadSample m_last;
};

class UpscalerCosts : public QObject {
    Q_OBJECT
public:
    static constexpr int kMaxEntries = 500;
    static constexpr double kBlend = 0.3;

    UpscalerCosts(const QString &path, const QByteArray &adapter, QObject *parent = nullptr);
    ~UpscalerCosts() override { save(); }

    void record(const QString &id, const QString &step, QSize source, QSize screen, double ms);
    std::optional<double> predict(const QString &id, const QString &step, QSize source, QSize screen) const;
    double costScale() const { return m_costScale; }
    void setCostScale(double scale);
    int size() const { return int(m_entries.size()); }
    bool save();

private:
    struct Entry {
        double ms = 0;
        quint64 used = 0;
    };
    void load();
    void changed();
    static QString keyOf(const QString &id, const QString &step, QSize source, QSize screen);

    QString m_path;
    QString m_adapter;
    QHash<QString, Entry> m_entries;
    QJsonObject m_others;
    double m_costScale = 1.0;
    quint64 m_clock = 0;
    bool m_dirty = false;
    QTimer m_saveLater;
};

class SteppedBackend : public UpscalerBackend {
    Q_OBJECT
public:
    SteppedBackend(UpscalerHost &host, LoadWatch::Kind kind, std::function<bool()> counts,
                   std::function<double()> frameFactor = {}, QObject *parent = nullptr);

    void picked(const VideoFacts &video) override;
    void fileStarted(const UpscalerPlan &) override;
    void restartWatch() override { m_watch.restart(); }
    void useCosts(UpscalerCosts *costs) override { m_costs = costs; }
    QString status(const VideoFacts &video) const override;

    LoadWatch &watch() { return m_watch; }
    void stepDownForTest() { stepDown(); }
    static double headroom();
    static void forceBattery(std::optional<bool> on);

    static constexpr double kUpLimit = 0.70;
    static constexpr int kUndoTicks = 40;

protected:
    struct Remembered {
        int step = 0;
        bool hard = false;
    };

    virtual QList<int> rungs(const VideoFacts &video) const = 0;
    virtual QString kindOf(const VideoFacts &video) const = 0;
    virtual double predicted(int step, const VideoFacts &video) const { Q_UNUSED(step); Q_UNUSED(video); return -1; }
    virtual double frameMs(const VideoFacts &video, const LoadSample &sample) const {
        Q_UNUSED(video);
        return sample.utilization * 1000.0 / std::max(sample.fpsEff, 1.0);
    }
    virtual double scaledUp(int from, int to, double utilization) const { Q_UNUSED(from); Q_UNUSED(to); Q_UNUSED(utilization); return -1; }
    virtual int initial(const QList<int> &rungs, const VideoFacts &video) const;
    enum class Move { Down, Up, Undone };
    virtual void moved(int from, int to, Move why, const VideoFacts &video) = 0;
    virtual void remembered() {}
    virtual void onLoad(const LoadSample &) {}

    int levelFor(const VideoFacts &video) const;
    int choose(const VideoFacts &video) { return m_planned = levelFor(video); }
    VideoFacts playing() const;
    const Remembered *recall(const QString &kind) const;
    void remember(const QString &kind, Remembered what);

    UpscalerHost &m_host;
    LoadWatch m_watch;
    QHash<QString, Remembered> m_remembered;
    UpscalerCosts *m_costs = nullptr;

private:
    void stepDown();
    void stepUp();
    void onSampled(const LoadSample &sample);

    static constexpr int kNone = -1;
    int m_cap = kNone;
    int m_planned = kNone;
    bool m_up = false;
    int m_upAt = 0;
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
    void onScreenChanged() { m_refresh.start(); }
    void onSpeedChanged();
    void restartWatch();
    double frameFactor() const;
    QString status() const { return m_status; }

signals:
    void upscalersChanged();
    void frameRateChanged();
    void statusChanged();

private:
    VideoFacts facts() const;
    static double factorOf(const UpscalerPlan &plan);
    UpscalerBackend *backend(const QString &id) const;
    void refreshOffers();
    void setShaders(const QString &shaders, const QString &id);
    void setFilter(const VideoFilter &filter);
    void logLoad();
    void refreshStatus();

    UpscalerHost &m_host;
    std::vector<UpscalerBackend *> m_backends;
    UpscalerCosts *m_costs = nullptr;
    QList<bool> m_offered;
    QTimer m_refresh;
    QTimer m_load;
    QTimer m_statusTimer;
    QString m_status;
    QString m_statusSubject;
    int64_t m_loadDrops = 0, m_loadDelayed = 0, m_loadMistimed = 0;
    QString m_id;
    UpscalerPlan m_current;
    int m_addedSlot = -1;
};

struct ShaderChain {
    const char *id;
    const char *name;
    std::initializer_list<const char *> files;
    std::initializer_list<const char *> lighter;
    std::initializer_list<const char *> clean;
};

namespace ShaderChains {
std::span<const ShaderChain> all();

// upstream gates start at 1.2x (Anime4K) or 1.3x (ArtCNN); the copy at 0.99x
QString lowerScaleGate(const QString &glsl);

QString list(std::initializer_list<const char *> files, const QString &configDir);
QStringList lists(const QString &configDir);
}

class ShaderBackend : public SteppedBackend {
    Q_OBJECT
public:
    ShaderBackend(UpscalerHost &host, const ShaderChain &chain, QObject *parent = nullptr);

    QString id() const override { return QLatin1String(m_chain.id); }
    QString name() const override { return QLatin1String(m_chain.name); }
    bool offered() const override;
    QString stepName() const override;
    QString stepLabel() const override;
    UpscalerPlan plan(const VideoFacts &video, const UpscalerPlan &current) override;
    QString notice(const VideoFacts &video) const override;
    void applied(const UpscalerPlan &before, const UpscalerPlan &after) override;

protected:
    QList<int> rungs(const VideoFacts &video) const override;
    QString kindOf(const VideoFacts &video) const override;
    double predicted(int step, const VideoFacts &video) const override;
    double scaledUp(int from, int to, double utilization) const override;
    void moved(int from, int to, Move why, const VideoFacts &video) override;
    void onLoad(const LoadSample &sample) override;

private:
    struct Step {
        int weight;
        std::initializer_list<const char *> files;
        const char *name;
    };
    QList<Step> steps() const;
    bool all(std::initializer_list<const char *> files) const;
    QString noticeFor(int level) const;
    bool cleansUpOnly(const VideoFacts &video) const;

    const ShaderChain &m_chain;
    QString m_shaders;
    int m_level = 0;
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
