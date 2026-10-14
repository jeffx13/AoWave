#include "media/render/upscaler.h"
#include "app/logger.h"
#include "app/settings.h"
#include "system/platform.h"
#ifdef AOWAVE_ANIMEJANAI
#include "media/render/animejanai.h"
#endif
#include <algorithm>
#include <cmath>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>

LoadWatch::LoadWatch(UpscalerHost &host, Kind kind, std::function<bool()> counts, std::function<double()> frameFactor,
                     QObject *parent)
    : QObject(parent), m_host(host), m_kind(kind), m_counts(std::move(counts)), m_factor(std::move(frameFactor)) {
    m_timer.setInterval(kTickMs);
    connect(&m_timer, &QTimer::timeout, this, &LoadWatch::tick);
}

void LoadWatch::restart() {
    m_started = false;
    m_settle = kSettleTicks;
    m_hot = m_slow = m_late = m_room = 0;
    std::fill(std::begin(m_added), std::end(m_added), 0);
}

void LoadWatch::passTimes(const QByteArray &json, double &fresh, double &freshPeak, double &redraw) {
    fresh = freshPeak = redraw = 0;
    if (json.isEmpty()) return;
    const QJsonObject passes = QJsonDocument::fromJson(json).object();
    for (const QJsonValue &pass : passes.value(QLatin1String("fresh")).toArray()) {
        fresh += pass.toObject().value(QLatin1String("avg")).toDouble();
        freshPeak = std::max(freshPeak, pass.toObject().value(QLatin1String("peak")).toDouble());
    }
    for (const QJsonValue &pass : passes.value(QLatin1String("redraw")).toArray())
        redraw += pass.toObject().value(QLatin1String("avg")).toDouble();
    fresh /= 1e6;
    freshPeak /= 1e6;
    redraw /= 1e6;
}

LoadSample LoadWatch::take(double speed, int64_t counters) const {
    LoadSample sample;
    passTimes(m_host.jsonProperty("vo-passes"), sample.fresh, sample.freshPeak, sample.redraw);
    double fps = m_host.doubleProperty("container-fps").value_or(0);
    if (fps <= 0) fps = 24;
    sample.fpsEff = fps * speed * (m_factor ? m_factor() : 1.0);
    sample.utilization = sample.fresh * sample.fpsEff / 1000.0;
    if (m_host.jsonProperty("display-sync-active") == "yes")
        sample.utilization += sample.redraw * std::max(0.0, m_host.doubleProperty("display-fps").value_or(0) - sample.fpsEff) / 1000.0;
    sample.vfFps = m_host.doubleProperty("estimated-vf-fps").value_or(0);
    sample.counters = counters;
    return sample;
}

void LoadWatch::tick() {
    const std::optional<int64_t> drops = m_host.intProperty("frame-drop-count");
    const std::optional<double> speed = m_host.doubleProperty("speed");
    if (!m_host.steady() || !m_counts() || !speed || !drops) {
        restart();
        return;
    }
    ++m_age;
    const int64_t delayed = m_host.intProperty("vo-delayed-frame-count").value_or(0);
    const int64_t mistimed = m_host.intProperty("mistimed-frame-count").value_or(0);
    if (*drops < m_drops || delayed < m_delayed || mistimed < m_mistimed) restart();
    const int64_t counters = *drops + delayed + mistimed;
    const int64_t gained = m_started ? counters - m_counters : 0;
    m_started = true;
    m_drops = *drops;
    m_delayed = delayed;
    m_mistimed = mistimed;
    m_counters = counters;

    LoadSample sample = take(*speed, counters);
    const bool settling = m_settle > 0;
    if (settling) --m_settle;
    sample.settled = !settling;
    m_last = sample;
    emit sampled(sample);
    if (settling) return;

    std::move(std::begin(m_added) + 1, std::end(m_added), std::begin(m_added));
    m_added[3] = int(gained);
    const int rose = m_added[0] + m_added[1] + m_added[2] + m_added[3];
    const double late = m_host.doubleProperty("avsync").value_or(0);
    const bool shaders = m_kind == Kind::Shaders;
    m_hot = shaders && sample.utilization >= 0.90 ? m_hot + 1 : 0;
    m_slow = !shaders && sample.vfFps > 0 && sample.vfFps < 0.95 * sample.fpsEff ? m_slow + 1 : 0;
    m_late = std::abs(late) > 0.2 ? m_late + 1 : 0;
    if (m_hot >= 3 || m_slow >= 3 || rose >= 2 || m_late >= 4) {
        restart();
        emit overloaded();
        return;
    }
    const bool room = rose == 0 && m_late == 0
                      && (shaders ? sample.fresh > 0 && sample.utilization < 0.55 : sample.vfFps >= 0.98 * sample.fpsEff);
    m_room = room ? m_room + 1 : 0;
    if (m_room >= kRoomTicks) {
        m_room = 0;
        emit roomToGrow();
    }
}

UpscalerCosts::UpscalerCosts(const QString &path, const QByteArray &adapter, QObject *parent)
    : QObject(parent), m_path(path), m_adapter(QString::fromUtf8(adapter)) {
    m_saveLater.setSingleShot(true);
    m_saveLater.setInterval(15000);
    connect(&m_saveLater, &QTimer::timeout, this, &UpscalerCosts::save);
    load();
}

QString UpscalerCosts::keyOf(const QString &id, const QString &step, QSize source, QSize screen) {
    return QStringLiteral("%1|%2|%3x%4|%5x%6").arg(id, step).arg(source.width()).arg(source.height()).arg(screen.width()).arg(screen.height());
}

void UpscalerCosts::load() {
    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly)) return;
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject() || doc.object().value(QLatin1String("version")).toInt() != 1) {
        logWarn() << "Player" << "upscaler-costs.json could not be read; starting it afresh";
        m_dirty = true;
        return;
    }
    m_others = doc.object().value(QLatin1String("byAdapter")).toObject();
    const QJsonObject mine = m_others.take(m_adapter).toObject();
    const QJsonObject shaders = mine.value(QLatin1String("shaders")).toObject();
    for (auto it = shaders.constBegin(); it != shaders.constEnd(); ++it)
        if (const double ms = it.value().toDouble(); ms > 0 && std::isfinite(ms) && m_entries.size() < kMaxEntries)
            m_entries.insert(it.key(), {ms, ++m_clock});
    m_costScale = std::clamp(mine.value(QLatin1String("aji")).toObject().value(QLatin1String("costScale")).toDouble(1.0), 0.6, 3.0);
}

bool UpscalerCosts::save() {
    m_saveLater.stop();
    if (!m_dirty) return true;
    QJsonObject shaders;
    for (auto it = m_entries.constBegin(); it != m_entries.constEnd(); ++it) shaders.insert(it.key(), it.value().ms);
    QJsonObject byAdapter = m_others;
    byAdapter.insert(m_adapter, QJsonObject{{QStringLiteral("shaders"), shaders},
                                            {QStringLiteral("aji"), QJsonObject{{QStringLiteral("costScale"), m_costScale}}}});
    QDir().mkpath(QFileInfo(m_path).absolutePath());
    QSaveFile out(m_path);
    if (!out.open(QIODevice::WriteOnly)) return false;
    out.write(QJsonDocument(QJsonObject{{QStringLiteral("version"), 1}, {QStringLiteral("byAdapter"), byAdapter}}).toJson());
    if (!out.commit()) return false;
    m_dirty = false;
    return true;
}

void UpscalerCosts::changed() {
    m_dirty = true;
    if (!m_saveLater.isActive()) m_saveLater.start();
}

void UpscalerCosts::record(const QString &id, const QString &step, QSize source, QSize screen, double ms) {
    if (!(ms > 0) || !std::isfinite(ms) || source.isEmpty()) return;
    const QString key = keyOf(id, step, source, screen);
    Entry &entry = m_entries[key];
    entry.ms = entry.used == 0 ? ms : entry.ms + kBlend * (ms - entry.ms);
    entry.ms = std::round(entry.ms * 1000) / 1000;
    entry.used = ++m_clock;
    while (m_entries.size() > kMaxEntries) {
        auto oldest = m_entries.begin();
        for (auto it = m_entries.begin(); it != m_entries.end(); ++it)
            if (it->used < oldest->used) oldest = it;
        m_entries.erase(oldest);
    }
    changed();
}

std::optional<double> UpscalerCosts::predict(const QString &id, const QString &step, QSize source, QSize screen) const {
    if (source.isEmpty()) return std::nullopt;
    if (const auto it = m_entries.constFind(keyOf(id, step, source, screen)); it != m_entries.constEnd()) return it->ms;
    const QString prefix = QStringLiteral("%1|%2|").arg(id, step);
    const QString onScreen = QStringLiteral("|%1x%2").arg(screen.width()).arg(screen.height());
    const double area = double(source.width()) * source.height();
    std::optional<double> best;
    double nearest = 0;
    for (auto it = m_entries.constBegin(); it != m_entries.constEnd(); ++it) {
        if (!it.key().startsWith(prefix) || !it.key().endsWith(onScreen)) continue;
        const QStringList size = it.key().section(QLatin1Char('|'), 2, 2).split(QLatin1Char('x'));
        const double other = size.value(0).toDouble() * size.value(1).toDouble();
        if (other <= 0) continue;
        const double distance = std::abs(std::log(area / other));
        if (!best || distance < nearest) {
            best = it->ms * area / other;
            nearest = distance;
        }
    }
    return best;
}

void UpscalerCosts::setCostScale(double scale) {
    scale = std::clamp(scale, 0.6, 3.0);
    if (scale == m_costScale) return;
    m_costScale = scale;
    changed();
}

namespace {
std::optional<bool> g_batteryForTest;
}

SteppedBackend::SteppedBackend(UpscalerHost &host, LoadWatch::Kind kind, std::function<bool()> counts,
                               std::function<double()> frameFactor, QObject *parent)
    : UpscalerBackend(parent), m_host(host), m_watch(host, kind, std::move(counts), std::move(frameFactor), this) {
    connect(&m_watch, &LoadWatch::overloaded, this, &SteppedBackend::stepDown);
    connect(&m_watch, &LoadWatch::roomToGrow, this, &SteppedBackend::stepUp);
    connect(&m_watch, &LoadWatch::sampled, this, &SteppedBackend::onSampled);
}

double SteppedBackend::headroom() {
    return (g_batteryForTest ? *g_batteryForTest : Platform::onBattery()) ? 0.85 : 1.0;
}

void SteppedBackend::forceBattery(std::optional<bool> on) { g_batteryForTest = on; }

VideoFacts SteppedBackend::playing() const {
    VideoFacts video;
    const std::optional<int64_t> w = m_host.intProperty("video-dec-params/w"), h = m_host.intProperty("video-dec-params/h");
    if (w && h && *w > 0 && *h > 0) video.source = QSize(int(*w), int(*h));
    video.fps = m_host.doubleProperty("container-fps").value_or(0);
    video.speed = m_host.doubleProperty("speed").value_or(1);
    video.display = m_host.displaySize();
    video.screen = m_host.screenSize();
    return video;
}

const SteppedBackend::Remembered *SteppedBackend::recall(const QString &kind) const {
    if (kind.isEmpty()) return nullptr;
    auto it = m_remembered.constFind(kind);
    if (it == m_remembered.constEnd())
        if (const qsizetype bar = kind.indexOf(QLatin1Char('|')); bar > 0) it = m_remembered.constFind(kind.left(bar));
    return it == m_remembered.constEnd() ? nullptr : &*it;
}

void SteppedBackend::remember(const QString &kind, Remembered what) {
    if (kind.isEmpty()) return;
    m_remembered.insert(kind, what);
    remembered();
}

int SteppedBackend::initial(const QList<int> &rungs, const VideoFacts &video) const {
    for (const int step : rungs) {
        if (step == 0) break;
        const double expected = predicted(step, video);
        if (expected < 0 || expected <= kUpLimit * headroom()) return step;
    }
    return rungs.first();
}

int SteppedBackend::levelFor(const VideoFacts &video) const {
    const QList<int> steps = rungs(video);
    int cap = m_cap;
    if (cap == kNone)
        if (const Remembered *known = recall(kindOf(video))) cap = known->step;
    if (cap == kNone) return initial(steps, video);
    for (const int step : steps)
        if (step <= cap) return step;
    return 0;
}

void SteppedBackend::picked(const VideoFacts &video) {
    m_cap = m_planned = kNone;
    m_up = false;
    const QString kind = kindOf(video);
    bool changed = m_remembered.remove(kind);
    if (const qsizetype bar = kind.indexOf(QLatin1Char('|')); bar > 0) changed = m_remembered.remove(kind.left(bar)) || changed;
    if (changed) remembered();
}

void SteppedBackend::fileStarted(const UpscalerPlan &) {
    m_cap = m_planned = kNone;
    m_up = false;
    m_watch.resetAge();
    m_watch.restart();
}

QString SteppedBackend::status(const VideoFacts &video) const {
    const LoadSample &sample = m_watch.last();
    const QString step = stepLabel();
    if (step.isEmpty() || !sample.settled || video.source.isEmpty() || video.display.isEmpty() || video.fps <= 0) return {};
    const double fit = std::min(video.display.width() / double(video.source.width()), video.display.height() / double(video.source.height()));
    const double budget = 1000.0 / (video.fps * std::max(video.speed, 0.1));
    return tr("%1 · %2 · %3×%4 to %5×%6 · %7 of %8 ms")
        .arg(name(), step).arg(video.source.width()).arg(video.source.height())
        .arg(qRound(video.source.width() * fit)).arg(qRound(video.source.height() * fit))
        .arg(frameMs(video, sample), 0, 'f', 1).arg(budget, 0, 'f', 1);
}

void SteppedBackend::stepDown() {
    const VideoFacts video = playing();
    const QList<int> steps = rungs(video);
    const int now = m_planned != kNone ? m_planned : levelFor(video);
    const qsizetype at = steps.indexOf(now);
    const int next = steps.at(at >= 0 ? std::min<qsizetype>(at + 1, steps.size() - 1)
                                     : std::max<qsizetype>(steps.size() - 2, 0));
    const bool undone = m_up && m_watch.age() - m_upAt < kUndoTicks;
    m_up = false;
    m_cap = next;
    const QString kind = kindOf(video);
    Remembered known;
    if (const Remembered *r = recall(kind)) known = *r;
    known.step = next;
    known.hard = known.hard || undone;
    remember(kind, known);
    moved(now, next, undone ? Move::Undone : Move::Down, video);
    emit replan();
}

void SteppedBackend::stepUp() {
    const VideoFacts video = playing();
    const QList<int> steps = rungs(video);
    const int now = m_planned != kNone ? m_planned : levelFor(video);
    const qsizetype at = steps.indexOf(now);
    if (now == 0 || at <= 0) return;
    const int higher = steps.at(at - 1);
    if (const Remembered *known = recall(kindOf(video)); known && known->hard && higher > known->step) return;
    double expected = predicted(higher, video);
    if (expected < 0) expected = scaledUp(now, higher, m_watch.utilization());
    if (expected < 0 || expected > kUpLimit * headroom()) return;
    m_cap = higher;
    m_up = true;
    m_upAt = m_watch.age();
    moved(now, higher, Move::Up, video);
    emit replan();
}

void SteppedBackend::onSampled(const LoadSample &sample) {
    onLoad(sample);
    if (!m_up || !sample.settled || m_watch.age() - m_upAt < kUndoTicks) return;
    m_up = false;
    const QString kind = kindOf(playing());
    Remembered known;
    if (const Remembered *r = recall(kind)) known = *r;
    known.step = m_cap;
    remember(kind, known);
}

QString UpscalerBackend::notice(const VideoFacts &) const {
    return tr("Upscaler: %1").arg(name());
}

Upscaler::Upscaler(UpscalerHost &host) : m_host(host) {}

void Upscaler::init(const QByteArray &adapter) {
    const QByteArray vsrMode = adapter.startsWith("NVIDIA") ? "nvidia" : adapter.startsWith("Intel") ? "intel" : "";
    m_costs = new UpscalerCosts(Settings::dataDir() + QStringLiteral("/upscaler-costs.json"), adapter, this);
    for (const ShaderChain &chain : ShaderChains::all()) m_backends.push_back(new ShaderBackend(m_host, chain, this));
    m_backends.push_back(new VsrBackend(vsrMode, this));
#ifdef AOWAVE_ANIMEJANAI
    bool hasFilter = false;
    if (m_host.command({"vf", "add", "@aji-probe:animejanai"})) {
        hasFilter = true;
        m_host.command({"vf", "remove", "@aji-probe"});
    }
    m_backends.push_back(new AnimeJaNaiBackend(m_host, hasFilter, vsrMode == "nvidia", this));
#endif
    for (UpscalerBackend *b : m_backends) {
        b->useCosts(m_costs);
        connect(b, &UpscalerBackend::replan, this, &Upscaler::apply);
        connect(b, &UpscalerBackend::note, this, [this](const QString &text) { m_host.showText(text); });
    }
    refreshOffers();

    m_refresh.setSingleShot(true);
    m_refresh.setInterval(300);
    connect(&m_refresh, &QTimer::timeout, this, &Upscaler::apply);

    m_load.setInterval(10000);
    connect(&m_load, &QTimer::timeout, this, &Upscaler::logLoad);
    m_load.start();

    m_statusTimer.setInterval(1000);
    connect(&m_statusTimer, &QTimer::timeout, this, &Upscaler::refreshStatus);
    m_statusTimer.start();
}

void Upscaler::refreshStatus() {
    const UpscalerBackend *chosen = m_id.isEmpty() ? nullptr : backend(m_id);
    const VideoFacts video = facts();
    const QString subject = chosen ? QStringLiteral("%1|%2x%3|%4").arg(m_id).arg(video.source.width()).arg(video.source.height()).arg(video.fps) : QString();
    const bool same = subject == m_statusSubject;
    m_statusSubject = subject;
    if (chosen && same && !m_host.steady()) return;
    const QString text = chosen && m_host.steady() ? chosen->status(video) : QString();
    if (text == m_status) return;
    m_status = text;
    emit statusChanged();
}

void Upscaler::logLoad() {
    if ((m_current.shaders.isEmpty() && m_current.filter.isEmpty()) || !m_host.steady()) return;
    double fresh = 0, peak = 0;
    const QJsonObject passes = QJsonDocument::fromJson(m_host.jsonProperty("vo-passes")).object();
    for (const QJsonValue &pass : passes.value(QLatin1String("fresh")).toArray()) {
        fresh += pass.toObject().value(QLatin1String("avg")).toDouble();
        peak += pass.toObject().value(QLatin1String("peak")).toDouble();
    }
    const auto since = [this](const char *name, int64_t &before) {
        const int64_t now = m_host.intProperty(name).value_or(0);
        const int64_t added = now >= before ? now - before : now;
        before = now;
        return added;
    };
    const int64_t drops = since("frame-drop-count", m_loadDrops);
    const int64_t delayed = since("vo-delayed-frame-count", m_loadDelayed);
    const int64_t mistimed = since("mistimed-frame-count", m_loadMistimed);
    const VideoFacts video = facts();
    const QSize screen = m_host.screenSize();
    const UpscalerBackend *chosen = backend(m_id);
    logInfo() << "Player" << QString::asprintf(
        "upscaler %s %s: %dx%d@%.3f on %dx%d, fresh %.1f ms (peak %.1f), vf %.1f fps, drops +%lld, delayed +%lld, "
        "mistimed +%lld, avsync %.0f ms",
        qPrintable(m_id), qPrintable(chosen ? chosen->stepName() : QString()), video.source.width(), video.source.height(),
        video.fps, screen.width(), screen.height(), fresh / 1e6, peak / 1e6,
        m_host.doubleProperty("estimated-vf-fps").value_or(0), static_cast<long long>(drops),
        static_cast<long long>(delayed), static_cast<long long>(mistimed),
        m_host.doubleProperty("avsync").value_or(0) * 1000);
}

UpscalerBackend *Upscaler::backend(const QString &id) const {
    const auto it = std::find_if(m_backends.begin(), m_backends.end(), [&id](const UpscalerBackend *b) { return b->id() == id; });
    return it == m_backends.end() ? nullptr : *it;
}

void Upscaler::refreshOffers() {
    QList<bool> offered;
    for (const UpscalerBackend *b : m_backends) offered.append(b->offered());
    if (offered == m_offered) return;
    const bool first = m_offered.isEmpty();
    m_offered = offered;
    if (!first) emit upscalersChanged();
}

QVariantList Upscaler::upscalers() const {
    QVariantList list{QVariantMap{{QStringLiteral("id"), QString()}, {QStringLiteral("name"), tr("Off")}}};
    for (size_t i = 0; i < m_backends.size(); ++i)
        if (i < size_t(m_offered.size()) && m_offered[i]) list << m_backends[i]->describe();
    return list;
}

VideoFacts Upscaler::facts() const {
    VideoFacts video;
    const std::optional<int64_t> w = m_host.intProperty("video-dec-params/w"), h = m_host.intProperty("video-dec-params/h");
    if (w && h && *w > 0 && *h > 0) video.source = QSize(int(*w), int(*h));
    video.fps = m_host.doubleProperty("container-fps").value_or(0);
    video.speed = m_host.doubleProperty("speed").value_or(1);
    video.display = m_host.displaySize();
    video.screen = m_host.screenSize();
    return video;
}

void Upscaler::onPictureChanged() {
    refreshOffers();
    apply();
}

void Upscaler::onStartFile() {
    if (m_current.filter.slot >= 0) m_current.filter.slot = m_addedSlot;
    for (UpscalerBackend *b : m_backends) b->fileStarted(m_current);
}

void Upscaler::onResized() {
    restartWatch();
    if (m_current.filter.label() == "@vsr"
        || Settings::instance().picture().value(QStringLiteral("upscaler")) == QLatin1String("vsr"))
        m_refresh.start();
}

void Upscaler::onSpeedChanged() {
    restartWatch();
    m_refresh.start();
}

void Upscaler::restartWatch() {
    for (UpscalerBackend *b : m_backends) b->restartWatch();
}

void Upscaler::apply() {
    QString id = Settings::instance().picture().value(QStringLiteral("upscaler")).toString();
    UpscalerBackend *chosen = backend(id);
    const size_t index = std::find(m_backends.begin(), m_backends.end(), chosen) - m_backends.begin();
    if (!chosen || index >= size_t(m_offered.size()) || !m_offered[index]) {
        chosen = nullptr;
        id.clear();
    }
    const VideoFacts video = facts();
    if (chosen && m_id != id) chosen->picked(video);
    m_id = id;

    UpscalerPlan next;
    if (chosen) next = chosen->plan(video, m_current);
    if (!chosen || !chosen->ownsFilter()) {
        for (UpscalerBackend *b : m_backends)
            if (b != chosen && next.filter.isEmpty())
                if (const std::optional<UpscalerPlan> idle = b->idle(m_current)) {
                    next.filter = idle->filter;
                    next.hwdec = idle->hwdec;
                }
    }

    const UpscalerPlan before = m_current;
    setShaders(next.shaders, id);
    if (next.hwdec != m_current.hwdec) {
        m_host.setPropertyAsync("hwdec", next.hwdec.isEmpty() ? QByteArray("auto-safe") : next.hwdec);
        m_current.hwdec = next.hwdec;
    }
    if (next.framedrop != m_current.framedrop) {
        m_host.setPropertyAsync("framedrop", next.framedrop.isEmpty() ? QByteArray("vo") : next.framedrop);
        m_current.framedrop = next.framedrop;
    }
    if (next.filter.full() != m_current.filter.full() || next.filter.revision != m_current.filter.revision) {
        const QByteArray full = next.filter.full();
        logInfo() << "Player" << QStringLiteral("upscaler %1: hwdec %2, filter %3")
                                     .arg(id.isEmpty() ? QStringLiteral("off") : id,
                                          QString::fromLatin1(next.hwdec.isEmpty() ? QByteArray("auto-safe") : next.hwdec),
                                          QString::fromUtf8(full.left(full.indexOf('=')) + full.mid(full.lastIndexOf(':'))));
    }
    setFilter(next.filter);
    if (before != m_current)
        for (UpscalerBackend *b : m_backends) b->applied(before, m_current);
    if (factorOf(before) != factorOf(m_current)) emit frameRateChanged();
}

double Upscaler::factorOf(const UpscalerPlan &plan) {
    return plan.filter.frames;
}

double Upscaler::frameFactor() const {
    return factorOf(m_current);
}

void Upscaler::setShaders(const QString &shaders, const QString &id) {
    if (shaders == m_current.shaders) return;
    if (!shaders.isEmpty() || !m_current.shaders.isEmpty()) {
        if (shaders.isEmpty()) m_host.commandAsync({"change-list", "glsl-shaders", "clr", ""});
        else m_host.commandAsync({"change-list", "glsl-shaders", "set", shaders.toUtf8()});
        logInfo() << "Player" << QStringLiteral("upscaler %1: shaders %2")
                                     .arg(id.isEmpty() ? QStringLiteral("off") : id,
                                          shaders.isEmpty() ? QStringLiteral("none") : QString(shaders).remove(QStringLiteral("~~/shaders/")));
    }
    m_current.shaders = shaders;
}

void Upscaler::setFilter(const VideoFilter &filter) {
    // another slot: no new filter, CUDA context or decoder
    if (filter.slot >= 0 && m_current.filter.slot >= 0 && filter.spec == m_current.filter.spec
        && filter.revision == m_current.filter.revision) {
        if (filter.slot != m_current.filter.slot) {
            m_host.commandAsync({"vf-command", filter.label().mid(1), "slot", QByteArray::number(filter.slot)});
            if (m_host.paused()) m_host.commandAsync({"seek", "0", "relative+exact"});
        }
        m_current.filter = filter;
        return;
    }
    if (filter == m_current.filter) return;
    if (!m_current.filter.isEmpty()) m_host.commandAsync({"vf", "remove", m_current.filter.label()});
    if (!filter.isEmpty()) {
        m_host.commandAsync({"vf", "add", filter.full()});
        m_addedSlot = filter.slot;
    }
    m_current.filter = filter;
}

void Upscaler::pick(const QString &id) {
    UpscalerBackend *chosen = backend(id);
    const size_t index = std::find(m_backends.begin(), m_backends.end(), chosen) - m_backends.begin();
    if (!id.isEmpty() && (!chosen || index >= size_t(m_offered.size()) || !m_offered[index])) {
        m_host.showText(tr("Not available: %1").arg(id));
        return;
    }
    if (chosen) chosen->picked(facts());
    Settings::instance().setPicture({{QStringLiteral("upscaler"), id}});
    m_host.showText(chosen ? chosen->notice(facts()) : tr("Upscaler: %1").arg(tr("Off")));
}

namespace {
const ShaderChain kShaderChains[] = {
    {"anime4k", "Anime4K A+A",
     {"Anime4K_Clamp_Highlights.glsl", "Anime4K_Restore_CNN_VL.glsl", "Anime4K_Upscale_CNN_x2_VL.glsl",
      "Anime4K_Restore_CNN_M.glsl", "Anime4K_AutoDownscalePre_x2.glsl", "Anime4K_AutoDownscalePre_x4.glsl",
      "Anime4K_Upscale_CNN_x2_S.glsl"},
     {"Anime4K_Clamp_Highlights.glsl", "Anime4K_Restore_CNN_M.glsl", "Anime4K_Upscale_CNN_x2_S.glsl"},
     {"Anime4K_Clamp_Highlights.glsl", "Anime4K_Restore_CNN_M.glsl"}},
    {"artcnn", "ArtCNN C4F32", {"ArtCNN_C4F32_DS.glsl", "CfL_Prediction.glsl"}, {"ArtCNN_C4F16_DS.glsl", "CfL_Prediction.glsl"}, {}},
    {"artcnn-fast", "ArtCNN C4F16", {"ArtCNN_C4F16_DS.glsl", "CfL_Prediction.glsl"}, {}, {}},
};

QString patchedPath(const QString &file, const QString &source) {
    QFile in(source);
    if (!in.open(QIODevice::ReadOnly)) return {};
    const QString text = QString::fromUtf8(in.readAll());
    const QString lowered = ShaderChains::lowerScaleGate(text);
    if (lowered.isEmpty()) return {};
    const QString dir = Settings::tempDir() + QStringLiteral("/patched-shaders/")
                        + QString::fromLatin1(QCryptographicHash::hash(lowered.toUtf8(), QCryptographicHash::Sha1).toHex().left(16));
    const QString path = dir + QLatin1Char('/') + file;
    if (QFile::exists(path)) return path;
    QDir().mkpath(dir);
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly)) return {};
    out.write(lowered.toUtf8());
    return out.commit() ? path : QString();
}
}

std::span<const ShaderChain> ShaderChains::all() { return kShaderChains; }

QString ShaderChains::lowerScaleGate(const QString &glsl) {
    static const QRegularExpression gate(QStringLiteral(R"(^(//!WHEN OUTPUT\.w (?:MAIN|LUMA)\.w / )(?:1\.200|1\.3)( > OUTPUT\.h (?:MAIN|LUMA)\.h / )(?:1\.200|1\.3)( >.*)$)"),
                                         QRegularExpression::MultilineOption);
    QString lowered;
    qsizetype last = 0;
    int changed = 0;
    for (auto it = gate.globalMatch(glsl); it.hasNext(); ++changed) {
        const QRegularExpressionMatch match = it.next();
        lowered += QStringView(glsl).mid(last, match.capturedStart() - last) + match.captured(1) + QLatin1String("0.99")
                   + match.captured(2) + QLatin1String("0.99") + match.captured(3);
        last = match.capturedEnd();
    }
    return changed > 0 ? lowered + QStringView(glsl).mid(last) : QString();
}

QString ShaderChains::list(std::initializer_list<const char *> files, const QString &configDir) {
    const bool upscalesFirst = std::none_of(files.begin(), files.end(),
                                            [](const char *f) { return QLatin1String(f).contains(QLatin1String("Upscale_CNN_x2_VL")); });
    QStringList list;
    for (const char *file : files) {
        const QString name = QLatin1String(file);
        const bool lowers = name.startsWith(QLatin1String("ArtCNN")) || name.contains(QLatin1String("Upscale_CNN_x2_VL"))
                            || (upscalesFirst && name.contains(QLatin1String("Upscale_CNN_x2_S")));
        const QString patched = lowers ? patchedPath(name, configDir + QStringLiteral("/shaders/") + name) : QString();
        list << (patched.isEmpty() ? QStringLiteral("~~/shaders/") + name : patched);
    }
    return list.join(QLatin1Char(';'));
}

QStringList ShaderChains::lists(const QString &configDir) {
    QStringList lists;
    for (const ShaderChain &chain : all())
        for (const auto &files : {chain.files, chain.lighter, chain.clean}) {
            if (files.size() == 0) continue;
            if (const QString one = list(files, configDir); !lists.contains(one)) lists << one;
        }
    return lists;
}

ShaderBackend::ShaderBackend(UpscalerHost &host, const ShaderChain &chain, QObject *parent)
    : SteppedBackend(host, LoadWatch::Kind::Shaders, [this] { return !m_shaders.isEmpty(); }, {}, parent), m_chain(chain) {}

bool ShaderBackend::all(std::initializer_list<const char *> files) const {
    const QString dir = Settings::dataDir() + QStringLiteral("/mpv/shaders/");
    return std::all_of(files.begin(), files.end(), [&dir](const char *file) { return QFile::exists(dir + QLatin1String(file)); });
}

bool ShaderBackend::offered() const { return all(m_chain.files); }

QList<ShaderBackend::Step> ShaderBackend::steps() const {
    QList<Step> list{{0, m_chain.files, "full"}};
    if (m_chain.lighter.size() > 0 && all(m_chain.lighter)) list.append({0, m_chain.lighter, "lighter"});
    if (m_chain.clean.size() > 0 && all(m_chain.clean)) list.append({0, m_chain.clean, "clean"});
    for (int i = 0; i < list.size(); ++i) list[i].weight = int(list.size()) - i;
    return list;
}

bool ShaderBackend::cleansUpOnly(const VideoFacts &video) const {
    if (m_chain.clean.size() == 0 || video.source.isEmpty() || video.screen.isEmpty() || video.source.height() < 1440) return false;
    const double fit = std::min(video.screen.width() / double(video.source.width()), video.screen.height() / double(video.source.height()));
    return fit <= 1.0 && all(m_chain.clean);
}

QList<int> ShaderBackend::rungs(const VideoFacts &video) const {
    QList<int> list;
    const QList<Step> ladder = steps();
    if (cleansUpOnly(video)) return list << ladder.last().weight << 0;
    for (const Step &step : ladder) list << step.weight;
    return list << 0;
}

QString ShaderBackend::stepName() const {
    for (const Step &step : steps())
        if (step.weight == m_level) return QLatin1String(step.name);
    return QStringLiteral("off");
}

QString ShaderBackend::stepLabel() const {
    if (m_shaders.isEmpty()) return {};
    const QString step = stepName();
    return step == QLatin1String("full") ? tr("Full") : step == QLatin1String("lighter") ? tr("Lighter") : tr("Clean-up");
}

QString ShaderBackend::kindOf(const VideoFacts &video) const {
    if (video.source.isEmpty() || video.fps <= 0) return {};
    return QStringLiteral("%1x%2@%3|%4p").arg(video.source.width()).arg(video.source.height()).arg(qRound(video.fps)).arg(video.screen.height());
}

UpscalerPlan ShaderBackend::plan(const VideoFacts &video, const UpscalerPlan &) {
    m_level = choose(video);
    UpscalerPlan plan;
    for (const Step &step : steps())
        if (step.weight == m_level) plan.shaders = ShaderChains::list(step.files, Settings::dataDir() + QStringLiteral("/mpv"));
    m_shaders = plan.shaders;
    return plan;
}

QString ShaderBackend::noticeFor(int level) const {
    if (level == 0) return tr("%1: off for this video, as it cannot keep up").arg(name());
    for (const Step &step : steps())
        if (step.weight == level && QLatin1String(step.name) == QLatin1String("clean"))
            return tr("%1: cleaning up only for this video, to keep up").arg(name());
    return tr("%1: lighter for this video, to keep up").arg(name());
}

QString ShaderBackend::notice(const VideoFacts &video) const {
    if (const int level = levelFor(video); level < rungs(video).first()) return noticeFor(level);
    if (cleansUpOnly(video)) return tr("Upscaler: %1 (only cleaning up, as the video is not smaller than the screen)").arg(name());
    if (video.scale() > 0 && video.scale() < 0.99)
        return QLatin1String(m_chain.id) == QLatin1String("anime4k")
                   ? tr("Upscaler: %1 (only cleaning up while the picture is shown smaller than the video)").arg(name())
                   : tr("Upscaler: %1 (idle while the picture is shown smaller than the video)").arg(name());
    return UpscalerBackend::notice(video);
}

void ShaderBackend::applied(const UpscalerPlan &, const UpscalerPlan &after) {
    if (!after.shaders.isEmpty() && after.shaders == m_shaders) {
        m_watch.restart();
        m_watch.start();
    } else {
        m_watch.stop();
    }
}

double ShaderBackend::predicted(int step, const VideoFacts &video) const {
    if (!m_costs || video.fps <= 0) return -1;
    for (const Step &s : steps())
        if (s.weight == step) {
            const std::optional<double> ms = m_costs->predict(id(), QLatin1String(s.name), video.source, video.screen);
            return ms ? *ms * video.fps * video.speed / 1000.0 : -1;
        }
    return -1;
}

void ShaderBackend::onLoad(const LoadSample &sample) {
    if (!m_costs || !sample.settled || m_level == 0 || !(sample.fresh > 0)) return;
    const VideoFacts video = playing();
    m_costs->record(id(), stepName(), video.source, video.screen, sample.fresh);
}

double ShaderBackend::scaledUp(int from, int to, double utilization) const {
    const double perStep = QLatin1String(m_chain.id) == QLatin1String("anime4k") ? 3.0 : 5.0;
    return utilization * std::pow(perStep, to - from);
}

void ShaderBackend::moved(int from, int to, Move why, const VideoFacts &) {
    Q_UNUSED(from);
    const QString what = to == 0 ? QStringLiteral("off") : QStringLiteral("step %1").arg(to);
    switch (why) {
    case Move::Down:
        logInfo() << "Player" << QStringLiteral("%1 drops frames on this video: %2").arg(name(), what);
        emit note(noticeFor(to));
        break;
    case Move::Undone:
        logInfo() << "Player" << QStringLiteral("%1 could not carry the step above on this video: %2").arg(name(), what);
        emit note(noticeFor(to));
        break;
    case Move::Up:
        logInfo() << "Player" << QStringLiteral("%1 has room on this video: %2").arg(name(), what);
        emit note(tr("%1: stepped up, as there is room").arg(name()));
        break;
    }
}

VsrBackend::VsrBackend(const QByteArray &mode, QObject *parent) : UpscalerBackend(parent), m_mode(mode) {}

QString VsrBackend::name() const {
    return m_mode == "nvidia" ? tr("RTX Video Super Resolution") : tr("Video Super Resolution");
}

QVariantMap VsrBackend::describe() const {
    QVariantMap map = UpscalerBackend::describe();
    map.insert(QStringLiteral("nvidia"), m_mode == "nvidia");
    return map;
}

UpscalerPlan VsrBackend::plan(const VideoFacts &video, const UpscalerPlan &current) {
    UpscalerPlan plan;
    if (m_mode.isEmpty() || video.display.isEmpty()) return plan;
    // dropping the filter while the size is unknown rebuilds it every second
    if (video.source.isEmpty()) {
        if (current.filter.spec.startsWith("@vsr:")) plan.filter = current.filter;
        return plan;
    }
    const double scale = VsrBackend::scale(video.scale(), video.source.width(), video.source.height());
    if (scale > 0)
        plan.filter.spec = "@vsr:d3d11vpp=scale=" + QByteArray::number(scale, 'f', 2) + ":scaling-mode=" + m_mode;
    return plan;
}

double VsrBackend::scale(double fit, int64_t w, int64_t h) {
    double scale = std::min(4.0, std::floor(fit * 10.0) / 10.0);
    if (scale <= 1.0) return 0;
    // d3d11vpp rounds size and crop apart; back off a hundredth
    const auto clean = [w, h](float s) {
        for (const int64_t n : {w, h}) {
            const int side = int(s * float(n)) + (int(s * float(n)) & 1);
            if (std::lrintf(s * float(n)) > side || (n == h && side == 1088)) return false;
        }
        return true;
    };
    for (int step = 0; step < 9 && !clean(QByteArray::number(scale, 'f', 2).toFloat()); ++step) scale -= 0.01;
    return scale;
}
