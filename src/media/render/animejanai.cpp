#include "media/render/animejanai.h"
#include "app/logger.h"
#include "app/settings.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <algorithm>
#include <cmath>
#include <utility>

namespace {
constexpr const char *kSdModel = "2x_AnimeJaNai_SD_V1beta34_Compact_1x3xHxW_dyn-HW_strong_fp16_op21_dynamo";
constexpr const char *kHdModels[] = {"2x_AnimeJaNai_HD_V3.1_Balanced_SPANF3_b8f64_unshuffle_fp16",
                                     "2x_AnimeJaNai_HD_V3.1_Performance_SPANF3_b5f48_unshuffle_fp16"};
constexpr double kPerformanceRatio = 0.6;
constexpr const char *kRifeModel = "rife_v4.25";
constexpr double kRifeMaxFps = 31;

constexpr const char *kEngineSettings =
    "--minShapes=input:1x3x8x8 --optShapes=input:1x3x1080x1920 --maxShapes=input:1x3x1216x2048";
constexpr int kMaxWidth = 2048, kMaxHeight = 1216;

// aji matches by pixels; rates 26, 31, 61 cover 24-25, 30, 50-60
struct Size { int width, height; };
constexpr Size kSizes[] = {{640, 480}, {720, 576}, {1024, 600}, {1280, 720}, {1920, 1080}, {2584, 1080}, {2560, 1440},
                           {3840, 2160}, {4096, 2160}};
constexpr double kRates[] = {26, 31, 61};
constexpr int kSdPixels = 1024 * 600;
constexpr QSize kDefaultScreen{1920, 1080};

constexpr double kUsed = 0.75;
constexpr double kLighter = 0.85;
constexpr double kSupersample = 0.8;
constexpr int kFloorPercent = 35;

using Stages = QList<AnimeJaNai::Stage>;

double stagesMs(const AnimeJaNai::Cost &cost, const Stages &stages, double width, double height, bool rife = false) {
    double ms = cost.fixed + (rife ? cost.rife * width * height / 1e6 : 0.0);
    for (const AnimeJaNai::Stage &stage : stages) {
        width *= stage.percent / 100.0;
        height *= stage.percent / 100.0;
        const double fit = std::min({1.0, kMaxWidth / width, kMaxHeight / height});
        width *= fit;
        height *= fit;
        ms += (stage.sd ? cost.sd : cost.hd * (stage.tier > 0 ? kPerformanceRatio : 1.0)) * width * height / 1e6;
        width *= 2;
        height *= 2;
    }
    return ms + cost.out * width * height / 1e6;
}

int floor5(double percent) { return int(std::floor(percent / 5.0 + 1e-9)) * 5; }

struct Ladder {
    Stages stages;
    bool raised = false;
};

QList<Ladder> ladderFor(Size k, QSize screen, bool tiers) {
    QList<Ladder> list;
    const bool sd = int64_t(k.width) * k.height <= kSdPixels;
    const double fitH = std::min(screen.width() / double(k.width), screen.height() / double(k.height)) * k.height;
    const int tierCount = tiers ? 2 : 1;
    if (sd) {
        if (fitH <= 2.0 * k.height) return list << Ladder{{{true, 100}}, false};
        const int top = std::clamp(floor5(fitH / (4.0 * k.height) * 100), 50, 100);
        for (int p = 100; p > top; p -= 5) list << Ladder{{{true, 100}, {false, p}}, true};
        for (int p = top; p >= 50; p -= 5)
            for (int tier = 0; tier < tierCount; ++tier) list << Ladder{{{true, 100}, {false, p, tier}}, false};
        return list << Ladder{{{true, 100}}, false};
    }
    const int cap = std::min(100, floor5(100.0 * std::min(kMaxWidth / double(k.width), kMaxHeight / double(k.height))));
    const int top = std::clamp(floor5(fitH / (2.0 * k.height) * 100), 5, cap);
    for (int p = cap; p > top; p -= 5) list << Ladder{{{false, p}}, true};
    for (int p = top; p >= std::min(kFloorPercent, top); p -= 5)
        for (int tier = 0; tier < tierCount; ++tier) list << Ladder{{{false, p, tier}}, false};
    return list;
}

bool sameStages(const Stages &a, const Stages &b) {
    return a.size() == b.size()
           && std::equal(a.begin(), a.end(), b.begin(), [](const AnimeJaNai::Stage &x, const AnimeJaNai::Stage &y) {
                  return x.sd == y.sd && x.percent == y.percent && x.tier == y.tier;
              });
}

QString section(int slot, const QList<AnimeJaNai::Chain> &chains) {
    QString text = QStringLiteral("[slot_%1]\nprofile_name=AnimeJaNai\n").arg(slot);
    int n = 0;
    for (const AnimeJaNai::Chain &chain : chains) {
        ++n;
        text += QStringLiteral("chain_%1_min_resolution=0x0\nchain_%1_max_resolution=%2x%3\n"
                               "chain_%1_min_fps=0\nchain_%1_max_fps=%4\n")
                    .arg(n).arg(chain.width).arg(chain.height).arg(chain.fps);
        int m = 0;
        for (const AnimeJaNai::Stage &stage : chain.stages) {
            ++m;
            text += QStringLiteral("chain_%1_model_%2_name=%3\n"
                                   "chain_%1_model_%2_resize_factor_before_upscale=%4\n"
                                   "chain_%1_model_%2_resize_height_before_upscale=0\n")
                        .arg(n).arg(m).arg(QLatin1String(stage.sd ? kSdModel : kHdModels[std::clamp(stage.tier, 0, 1)])).arg(stage.percent);
        }
        if (chain.rife)
            text += QStringLiteral("chain_%1_rife=yes\nchain_%1_rife_factor_numerator=2\nchain_%1_rife_factor_denominator=1\n"
                                   "chain_%1_rife_model=%2\nchain_%1_rife_ensemble=no\nchain_%1_rife_scene_detect_threshold=0.15\n"
                                   "chain_%1_rife_before_upscale=yes\n").arg(n).arg(QLatin1String(kRifeModel));
        else
            text += QStringLiteral("chain_%1_rife=no\n").arg(n);
    }
    return text;
}
}

AnimeJaNai::Cost AnimeJaNai::defaultCost() {
    return {2.0, 20.8, 9.2, 0.93, 40.0};
}

AnimeJaNai::AnimeJaNai(const Cost &cost) : m_cost(cost) { rebuild(); }

void AnimeJaNai::rebuild() {
    const double scale = m_scale;
    const Cost base = m_tensorRt ? m_cost : Cost{m_cost.fixed, m_cost.sd * 2, m_cost.hd * 2, m_cost.out, m_cost.rife};
    m_active = {base.fixed * scale, base.sd * scale, base.hd * scale, base.out * scale, base.rife * scale};
    m_full = worked(m_active, screen(), m_tiers, Kind::Full);
    m_light = worked(m_active, screen(), m_tiers, Kind::Light);
    m_rife = m_tensorRt ? worked(m_active, screen(), m_tiers, Kind::Rife) : QList<Chain>();
}

QSize AnimeJaNai::screen() const {
    return m_screen.isEmpty() ? kDefaultScreen : m_screen;
}

QList<AnimeJaNai::Chain> AnimeJaNai::worked(const Cost &cost, QSize screen, bool tiers, Kind kind) {
    QList<Chain> chains;
    for (const Size size : kSizes) {
        const QList<Ladder> ladder = ladderFor(size, screen, tiers);
        QList<Chain> forSize;
        for (const double rate : kRates) {
            if (kind == Kind::Rife && rate > kRifeMaxFps) continue;
            const double budget = 1000.0 / rate * kUsed;
            const bool rife = kind == Kind::Rife;
            const auto ms = [&](const Ladder &plan) { return stagesMs(cost, plan.stages, size.width, size.height, rife); };
            const auto fits = [&](const Ladder &plan, double limit) {
                return ms(plan) <= (plan.raised ? std::min(limit, kSupersample * budget) : limit);
            };
            auto best = std::find_if(ladder.begin(), ladder.end(), [&](const Ladder &plan) { return fits(plan, budget); });
            if (best == ladder.end()) continue;
            if (kind == Kind::Light) {
                const double limit = std::min(budget, kLighter * ms(*best));
                best = std::find_if(best + 1, ladder.end(), [&](const Ladder &plan) { return fits(plan, limit); });
                if (best == ladder.end()) continue;
            }
            Chain chain{size.width, size.height, rate, best->stages, rife};
            if (!forSize.isEmpty() && sameStages(forSize.last().stages, chain.stages)) forSize.removeLast();
            forSize.append(chain);
        }
        chains += forSize;
    }
    return chains;
}

void AnimeJaNai::setTensorRt(bool on) {
    if (on == m_tensorRt) return;
    m_tensorRt = on;
    rebuild();
}

void AnimeJaNai::setCostScale(double scale) {
    scale = std::clamp(scale, 0.6, 3.0);
    if (scale == m_scale) return;
    m_scale = scale;
    rebuild();
}

bool AnimeJaNai::setScreen(QSize screen) {
    if (screen == m_screen) return false;
    const QByteArray before = conf();
    m_screen = screen;
    rebuild();
    return conf() != before;
}

bool AnimeJaNai::setTiers(bool performance) {
    if (performance == m_tiers) return false;
    const QByteArray before = conf();
    m_tiers = performance;
    rebuild();
    return conf() != before;
}

bool AnimeJaNai::usable() const {
    return QFile::exists(root() + QStringLiteral("/inference/aji.dll"));
}

bool AnimeJaNai::hasPerformanceModel() const {
    return QFile::exists(root() + QStringLiteral("/onnx/") + QLatin1String(kHdModels[1]) + QStringLiteral(".onnx"));
}

bool AnimeJaNai::hasRife() const {
    return m_tensorRt && QFile::exists(root() + QStringLiteral("/rife/") + QLatin1String(kRifeModel) + QStringLiteral(".onnx"));
}

QString AnimeJaNai::root() const {
    const QString dir = Settings::instance().animeJaNaiDir();
    return dir.isEmpty() ? QString() : QDir::fromNativeSeparators(dir) + QStringLiteral("/animejanai");
}

QByteArray AnimeJaNai::conf() const {
    return (QStringLiteral("[global]\nbackend=%1\n").arg(m_tensorRt ? "TensorRT" : "DirectML")
            + (m_tensorRt ? QStringLiteral("trt_engine_settings=%1\n").arg(QLatin1String(kEngineSettings)) : QString())
            + section(Slot, m_full) + section(LightSlot, m_light) + (m_tensorRt ? section(RifeSlot, m_rife) : QString())).toUtf8();
}

QString AnimeJaNai::confPath() const {
    const QString path = Settings::dataDir() + QStringLiteral("/animejanai.conf");
    const QByteArray wanted = conf();
    if (wanted == m_written) return path;
    QFile current(path);
    if (!(current.open(QIODevice::ReadOnly) && current.readAll() == wanted)) {
        current.close();
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) return path;
        file.write(wanted);
        if (!file.commit()) return path;
    }
    m_written = wanted;
    return path;
}

const AnimeJaNai::Chain *AnimeJaNai::chainFor(int slot, int64_t width, int64_t height, double fps) const {
    const QList<Chain> &chains = slot == Slot ? m_full : slot == LightSlot ? m_light : slot == RifeSlot ? m_rife : QList<Chain>();
    const auto it = std::find_if(chains.begin(), chains.end(), [&](const Chain &chain) {
        return width * height <= int64_t(chain.width) * chain.height && fps <= chain.fps;
    });
    return it == chains.end() ? nullptr : &*it;
}

double AnimeJaNai::frameMs(const Chain &chain, int64_t width, int64_t height) const {
    return stagesMs(m_active, chain.stages, double(width), double(height), chain.rife);
}

AnimeJaNai::Verdict AnimeJaNai::verdict(int64_t width, int64_t height, double fps, double speed, double headroom) const {
    Verdict verdict;
    verdict.budgetMs = 1000.0 / (std::max(fps, 1.0) * std::max(speed, 0.1)) * kUsed * headroom;
    for (const int slot : {Slot, LightSlot}) {
        const Chain *chain = chainFor(slot, width, height, fps);
        if (!chain) continue;
        verdict.ms = frameMs(*chain, width, height);
        if (verdict.ms <= verdict.budgetMs) {
            verdict.slot = slot;
            return verdict;
        }
    }
    verdict.limited = true;
    return verdict;
}

AnimeJaNai::Verdict AnimeJaNai::rifeVerdict(int64_t width, int64_t height, double fps, double speed, double headroom) const {
    Verdict verdict;
    verdict.budgetMs = 1000.0 / (std::max(fps, 1.0) * std::max(speed, 0.1)) * kUsed * headroom;
    if (!m_tensorRt || fps * speed > kRifeMaxFps) { verdict.limited = true; return verdict; }
    if (const Chain *chain = chainFor(RifeSlot, width, height, fps)) {
        verdict.ms = frameMs(*chain, width, height);
        if (verdict.ms <= verdict.budgetMs) {
            verdict.slot = RifeSlot;
            return verdict;
        }
    }
    verdict.limited = true;
    return verdict;
}

namespace {
// mpv's length prefix for ':' (drive letter) or ','
QByteArray filterValue(const QString &value) {
    const QByteArray utf8 = value.toUtf8();
    return '%' + QByteArray::number(utf8.size()) + '%' + utf8;
}
}

AnimeJaNaiBackend::AnimeJaNaiBackend(UpscalerHost &host, bool hasFilter, bool tensorRt, QObject *parent)
    : SteppedBackend(host, LoadWatch::Kind::Filter, [this] { return m_slot != 0; },
                     [this] { return framesOf(m_slot); }, parent),
      m_hasFilter(hasFilter) {
    m_aji.setTensorRt(tensorRt);
    for (const QString &entry : Settings::instance().get(Config::AnimeJaNaiCeilings)) {
        const qsizetype at = entry.lastIndexOf(QLatin1Char('='));
        if (at <= 0) continue;
        QString slot = entry.mid(at + 1);
        const bool hard = slot.endsWith(QLatin1Char('!'));
        if (hard) slot.chop(1);
        m_remembered.insert(entry.left(at), {stepOf(slot.toInt()), hard});
    }
    m_statsPath = QDir::fromNativeSeparators(Settings::tempDir()) + QStringLiteral("/animejanai-status.txt");
    m_statusTimer.setInterval(2000);
    connect(&m_statusTimer, &QTimer::timeout, this, &AnimeJaNaiBackend::pollStatus);
}

double AnimeJaNaiBackend::framesOf(int slot) {
    return slot == AnimeJaNai::RifeSlot ? 2.0 : 1.0;
}

int AnimeJaNaiBackend::slotOf(int step) {
    switch (step) {
    case 3: return AnimeJaNai::RifeSlot;
    case 2: return AnimeJaNai::Slot;
    case 1: return AnimeJaNai::LightSlot;
    default: return 0;
    }
}

int AnimeJaNaiBackend::stepOf(int slot) {
    switch (slot) {
    case AnimeJaNai::RifeSlot: return 3;
    case AnimeJaNai::Slot: return 2;
    case AnimeJaNai::LightSlot: return 1;
    default: return 0;
    }
}

UpscalerPlan AnimeJaNaiBackend::planFor(int slot) const {
    UpscalerPlan plan;
    plan.hwdec = m_aji.tensorRt() ? "nvdec" : "d3d11va";
    const QString root = m_aji.root();
    QByteArray spec = "@aji:animejanai=lib=" + filterValue(root + QStringLiteral("/inference/aji.dll"))
                      + ":conf=" + filterValue(m_aji.confPath())
                      + ":model-dir=" + filterValue(root + QStringLiteral("/onnx"))
                      + ":trtexec=" + filterValue(root + QStringLiteral("/inference/trtexec.exe"));
    if (QFileInfo(root + QStringLiteral("/rife")).isDir())
        spec += ":rife-model-dir=" + filterValue(root + QStringLiteral("/rife"));
    // NV12: mpv's default 16-bit 4:4:4 is 50 MB per 4K frame
    plan.filter.spec = spec + ":stats=" + filterValue(m_statsPath) + ":output-444=no";
    plan.filter.slot = slot;
    plan.filter.revision = m_revision;
    plan.filter.frames = framesOf(slot);
    return plan;
}

void AnimeJaNaiBackend::fitScreen(const VideoFacts &video) {
    bool changed = m_aji.setScreen(video.screen);
    changed = m_aji.setTiers(m_aji.hasPerformanceModel()) || changed;
    if (changed) ++m_revision;
}

void AnimeJaNaiBackend::picked(const VideoFacts &video) {
    fitScreen(video);
    SteppedBackend::picked(video);
}

UpscalerPlan AnimeJaNaiBackend::plan(const VideoFacts &video, const UpscalerPlan &) {
    fitScreen(video);
    UpscalerPlan plan = planFor(slotOf(choose(video)));
    if (plan.filter.slot != 0 && video.speed > 1.05) plan.framedrop = "decoder+vo";
    return plan;
}

std::optional<UpscalerPlan> AnimeJaNaiBackend::idle(const UpscalerPlan &current) {
    if (current.filter.label() != "@aji") return std::nullopt;
    return planFor(0);
}

QString AnimeJaNaiBackend::kindOf(const VideoFacts &video) const {
    if (video.source.isEmpty() || video.fps <= 0) return {};
    return QStringLiteral("%1x%2@%3%4%5").arg(video.source.width()).arg(video.source.height()).arg(qRound(video.fps))
        .arg(video.speed > 1.05 ? QStringLiteral(" fast") : QString())
        .arg(video.screen.isEmpty() ? QString() : QStringLiteral("|%1p").arg(video.screen.height()));
}

QString AnimeJaNaiBackend::stepLabel() const {
    switch (m_slot) {
    case AnimeJaNai::Slot: return tr("Full");
    case AnimeJaNai::LightSlot: return tr("Lighter");
    case AnimeJaNai::RifeSlot: return tr("Frame interpolation");
    default: return {};
    }
}

double AnimeJaNaiBackend::frameMs(const VideoFacts &video, const LoadSample &sample) const {
    const AnimeJaNai::Chain *chain = video.source.isEmpty() ? nullptr
        : m_aji.chainFor(m_slot, video.source.width(), video.source.height(), video.fps);
    return chain ? m_aji.frameMs(*chain, video.source.width(), video.source.height()) : SteppedBackend::frameMs(video, sample);
}

QString AnimeJaNaiBackend::enhanceNotice(const VideoFacts &video) const {
    if (video.source.isEmpty() || video.screen.isEmpty() || video.fps <= 0) return {};
    const double fit = std::min(video.screen.width() / double(video.source.width()), video.screen.height() / double(video.source.height()));
    const AnimeJaNai::Chain *chain = m_aji.chainFor(AnimeJaNai::Slot, video.source.width(), video.source.height(), video.fps);
    if (fit > 1.0 || !chain || chain->stages.size() != 1 || chain->stages.first().sd || chain->stages.first().percent >= 100) return {};
    return tr("Upscaler: %1 (not smaller than the screen: shrunk to %2% first, then enhanced)").arg(name()).arg(chain->stages.first().percent);
}

QList<int> AnimeJaNaiBackend::rungs(const VideoFacts &video) const {
    QList<int> steps;
    int slot = AnimeJaNai::Slot;
    if (!video.source.isEmpty() && video.fps > 0) {
        const int w = video.source.width(), h = video.source.height();
        slot = m_aji.verdict(w, h, video.fps, video.speed, headroom()).slot;
        if (slot != 0 && m_aji.hasRife() && Settings::instance().frameInterpolation()
            && m_aji.rifeVerdict(w, h, video.fps, video.speed, headroom()).slot != 0)
            steps << stepOf(AnimeJaNai::RifeSlot);
    }
    if (slot == AnimeJaNai::Slot) steps << stepOf(AnimeJaNai::Slot) << stepOf(AnimeJaNai::LightSlot);
    else if (slot == AnimeJaNai::LightSlot) steps << stepOf(AnimeJaNai::LightSlot);
    return steps << 0;
}

double AnimeJaNaiBackend::predicted(int step, const VideoFacts &video) const {
    if (video.source.isEmpty() || video.fps <= 0) return -1;
    const AnimeJaNai::Chain *chain = m_aji.chainFor(slotOf(step), video.source.width(), video.source.height(), video.fps);
    if (!chain) return -1;
    return m_aji.frameMs(*chain, video.source.width(), video.source.height()) * video.fps * video.speed / 1000.0;
}

QString AnimeJaNaiBackend::steppedNotice(const VideoFacts &video) const {
    const QList<int> steps = rungs(video);
    const int now = levelFor(video);
    if (now == steps.first()) return {};
    if (now == 0) return tr("%1: off for this video, as it cannot keep up").arg(name());
    return steps.first() == stepOf(AnimeJaNai::RifeSlot) && now == stepOf(AnimeJaNai::Slot)
               ? tr("%1: no frame interpolation for this video, to keep up").arg(name())
               : tr("%1: lighter for this video, to keep up").arg(name());
}

void AnimeJaNaiBackend::remembered() {
    QStringList entries;
    for (auto it = m_remembered.constBegin(); it != m_remembered.constEnd(); ++it)
        entries << it.key() + QLatin1Char('=') + QString::number(slotOf(it.value().step))
                       + (it.value().hard ? QStringLiteral("!") : QString());
    entries.sort();
    Settings::instance().set(Config::AnimeJaNaiCeilings, entries);
}

QString AnimeJaNaiBackend::notice(const VideoFacts &video) const {
    if (!video.source.isEmpty() && !video.display.isEmpty() && video.fps > 0) {
        const AnimeJaNai::Verdict v = m_aji.verdict(video.source.width(), video.source.height(), video.fps, video.speed, headroom());
        if (v.slot == 0 && v.ms <= 0)
            return tr("Upscaler: %1 (off for this video: %2x%3 at %4 fps is not supported)")
                .arg(name()).arg(video.source.width()).arg(video.source.height()).arg(qRound(video.fps * video.speed));
        if (v.slot == 0)
            return tr("Upscaler: %1 (off for this video: %2x%3 at %4 fps needs about %5 ms a frame, %6 ms available)")
                .arg(name()).arg(video.source.width()).arg(video.source.height()).arg(qRound(video.fps * video.speed))
                .arg(v.ms, 0, 'f', 1).arg(v.budgetMs, 0, 'f', 1);
    }
    if (const QString stepped = steppedNotice(video); !stepped.isEmpty()) return stepped;
    if (Settings::instance().frameInterpolation() && m_aji.tensorRt() && !video.source.isEmpty() && video.fps > 0
        && video.fps * video.speed <= 31) {
        if (!m_aji.hasRife())
            return tr("%1: frame interpolation needs RIFE models, missing from this AnimeJaNai folder").arg(name());
        const AnimeJaNai::Verdict r = m_aji.rifeVerdict(video.source.width(), video.source.height(), video.fps, video.speed, headroom());
        if (r.slot == 0)
            return r.ms <= 0 ? tr("%1: no frame interpolation for this video, which is too large").arg(name())
                             : tr("%1: no frame interpolation for this video (needs about %2 ms a frame, %3 ms "
                                  "available)")
                                   .arg(name()).arg(r.ms, 0, 'f', 1).arg(r.budgetMs, 0, 'f', 1);
    }
    if (const QString enhance = enhanceNotice(video); !enhance.isEmpty()) return enhance;
    return UpscalerBackend::notice(video);
}

void AnimeJaNaiBackend::applied(const UpscalerPlan &before, const UpscalerPlan &after) {
    const bool was = before.filter.label() == "@aji", is = after.filter.label() == "@aji";
    if (is) m_slot = after.filter.slot;
    if (is && (!was || before.filter.spec != after.filter.spec)) {
        m_watch.restart();
        m_watch.start();
        watchEngines();
    } else if (is && before.filter.slot != after.filter.slot) {
        m_watch.restart();
        if (after.filter.slot == AnimeJaNai::RifeSlot) watchEngines();
    } else if (!is) {
        m_watch.stop();
        m_statusTimer.stop();
    }
}

void AnimeJaNaiBackend::useCosts(UpscalerCosts *costs) {
    SteppedBackend::useCosts(costs);
    if (!costs) return;
    m_aji.setCostScale(costs->costScale());
    m_nextScale = m_aji.costScale();
}

void AnimeJaNaiBackend::onLoad(const LoadSample &sample) {
    if (m_slot == AnimeJaNai::Slot && sample.settled && !m_overloaded && ++m_cleanTicks == kCleanTicks)
        m_nextScale = std::max(0.6, m_aji.costScale() * 0.97);
}

void AnimeJaNaiBackend::fileStarted(const UpscalerPlan &actual) {
    if (m_nextScale != m_aji.costScale()) {
        m_aji.setCostScale(m_nextScale);
        m_nextScale = m_aji.costScale();
        if (m_costs) m_costs->setCostScale(m_nextScale);
        m_aji.confPath();
    }
    m_overloaded = false;
    m_cleanTicks = 0;
    SteppedBackend::fileStarted(actual);
    if (actual.filter.label() == "@aji") {
        m_slot = actual.filter.slot;
        watchEngines();
    }
}

void AnimeJaNaiBackend::moved(int from, int to, Move why, const VideoFacts &video) {
    Q_UNUSED(from);
    if (why != Move::Up && !std::exchange(m_overloaded, true)) m_nextScale = std::min(3.0, m_aji.costScale() * 1.15);
    if (why == Move::Up) {
        logInfo() << "Player" << QStringLiteral("AnimeJaNai has room on this video: slot %1 now").arg(slotOf(to));
        emit note(tr("%1: stepped up, as there is room").arg(name()));
        return;
    }
    logInfo() << "Player" << (why == Move::Undone ? QStringLiteral("AnimeJaNai could not carry the step above on this video: slot %1 now")
                                                  : QStringLiteral("AnimeJaNai drops frames on this video: slot %1 now"))
                                 .arg(slotOf(to));
    emit note(steppedNotice(video));
}

QVariantMap AnimeJaNaiBackend::describe() const {
    QVariantMap map = UpscalerBackend::describe();
    map.insert(QStringLiteral("interpolation"), m_aji.tensorRt());
    return map;
}

void AnimeJaNaiBackend::watchEngines() {
    m_since = QDateTime::currentDateTime().addSecs(-1);
    m_engines = Engines::Unknown;
    m_polls = 0;
    m_statusTimer.start();
}

// status lines: Building TensorRT engine, Applied Model, FAILED
void AnimeJaNaiBackend::pollStatus() {
    QFile file(m_statsPath);
    const bool fresh = !m_since.isValid() || QFileInfo(file).lastModified() >= m_since;
    if (!fresh || !file.open(QIODevice::ReadOnly)) {
        if (++m_polls > 150) m_statusTimer.stop();
        return;
    }
    const QString text = QString::fromUtf8(file.readAll());
    Engines now = Engines::Unknown;
    QString failure;
    for (const QString &line : text.split(QLatin1Char('\n'))) {
        if (line.contains(QLatin1String("FAILED"))) { now = Engines::Failed; failure = line.trimmed(); }
        else if (now != Engines::Failed && line.contains(QLatin1String("Building TensorRT engine"))) now = Engines::Building;
        else if (now == Engines::Unknown && line.contains(QLatin1String("Applied Model"))) now = Engines::Ready;
    }
    if (now == Engines::Unknown || now == m_engines) return;
    const Engines was = std::exchange(m_engines, now);
    if (now == Engines::Building) {
        logInfo() << "Player" << QStringLiteral("AnimeJaNai is building its engines");
        emit note(tr("%1: building engines (first time only, about a minute). The video plays unchanged until then.").arg(name()));
    } else if (now == Engines::Failed) {
        logWarn() << "Player" << QStringLiteral("AnimeJaNai could not build its engines:") << failure;
        emit note(tr("%1: could not build engines (see the log)").arg(name()));
        m_statusTimer.stop();
    } else {
        if (was == Engines::Building) {
            logInfo() << "Player" << QStringLiteral("AnimeJaNai engines are ready");
            emit note(tr("%1: ready").arg(name()));
        }
        m_statusTimer.stop();
    }
}
