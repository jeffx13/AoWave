#include "media/render/animejanai.h"
#include "app/settings.h"
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <algorithm>

namespace {
constexpr const char *kSdModel = "2x_AnimeJaNai_SD_V1beta34_Compact_1x3xHxW_dyn-HW_strong_fp16_op21_dynamo";
constexpr const char *kHdModel = "2x_AnimeJaNai_HD_V3.1_Balanced_SPANF3_b8f64_unshuffle_fp16";
// RIFE's model: v4.25 is its general-purpose one; the lite v4.22 saves 2 ms of 22 at 576p, the v4.26 costs 3 more.
constexpr const char *kRifeModel = "rife_v4.25";
constexpr double kRifeMaxFps = 31;   // RIFE doubles 24, 25 and 30 fps to 48, 50 and 60; no more than that

// One dynamic-shape engine per model is as fast as one per size.
constexpr const char *kEngineSettings =
    "--minShapes=input:1x3x8x8 --optShapes=input:1x3x1080x1920 --maxShapes=input:1x3x1216x2048";
constexpr int kMaxWidth = 2048, kMaxHeight = 1216;

// aji matches by pixel count; rate 26 holds 24 and 25 fps, 31 holds 30, 61 holds 50 and 60.
struct Size { int width, height; };
constexpr Size kSizes[] = {{640, 480}, {720, 576}, {1024, 600}, {1280, 720}, {1920, 1080}, {2584, 1080}, {2560, 1440}};
constexpr double kRates[] = {26, 31, 61};
constexpr int kSdPixels = 1024 * 600;

// A frame may take this much of the time there is; the rest is decode, the copy back from CUDA, mpv's passes and a
// margin. A chain at 76% (SD+HD60 at 30 fps) hitched in two of six real-time runs; chains at 70% and below never did.
constexpr double kUsed = 0.75;
constexpr double kLighter = 0.85;

using Stages = QList<AnimeJaNai::Stage>;
const QList<Stages> kSdFull = {{{true, 100}, {false, 100}}, {{true, 100}, {false, 75}}, {{true, 100}, {false, 60}}, {{true, 100}, {false, 50}}};
const QList<Stages> kSdLight = {{{true, 100}, {false, 60}}, {{true, 100}}};
const QList<Stages> kHdFull = {{{false, 100}}, {{false, 85}}, {{false, 75}}, {{false, 60}}};
const QList<Stages> kHdLight = {{{false, 70}}, {{false, 60}}};

// Each model doubles both sides; aji scales a picture larger than the engine's top shape down to it.
double stagesMs(const AnimeJaNai::Cost &cost, const Stages &stages, double width, double height, bool rife = false) {
    double ms = cost.fixed + (rife ? cost.rife * width * height / 1e6 : 0.0);
    for (const AnimeJaNai::Stage &stage : stages) {
        width *= stage.percent / 100.0;
        height *= stage.percent / 100.0;
        const double fit = std::min({1.0, kMaxWidth / width, kMaxHeight / height});
        width *= fit;
        height *= fit;
        ms += (stage.sd ? cost.sd : cost.hd) * width * height / 1e6;
        width *= 2;
        height *= 2;
    }
    return ms + cost.out * width * height / 1e6;
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
                        .arg(n).arg(m).arg(QLatin1String(stage.sd ? kSdModel : kHdModel)).arg(stage.percent);
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

// Least squares over 20 chains (480p to 2584x1080, RTX 5080 Laptop at 65 W), within 9% of each, as much as the GPU varies.
AnimeJaNai::Cost AnimeJaNai::defaultCost() {
    return {2.0, 20.8, 9.2, 0.93, 40.0};
}

AnimeJaNai::AnimeJaNai(const Cost &cost)
    : m_cost(cost), m_active(cost), m_full(worked(cost, false)), m_light(worked(cost, true)), m_rife(workedWithRife(cost)) {}

QList<AnimeJaNai::Chain> AnimeJaNai::workedWithRife(const Cost &cost) {
    QList<Chain> chains;
    for (const Size size : kSizes) {
        const bool sd = int64_t(size.width) * size.height <= kSdPixels;
        // With RIFE's share taken out the SD model alone is the most that fits at 576p.
        const QList<Stages> candidates = sd ? kSdFull + QList<Stages>{{{true, 100}}} : kHdFull;
        QList<Chain> forSize;
        for (const double rate : kRates) {
            if (rate > kRifeMaxFps) continue;
            const double budget = 1000.0 / rate * kUsed;
            const auto best = std::find_if(candidates.begin(), candidates.end(),
                                           [&](const Stages &s) { return stagesMs(cost, s, size.width, size.height, true) <= budget; });
            if (best == candidates.end()) continue;
            Chain chain{size.width, size.height, rate, *best, true};
            if (!forSize.isEmpty() && forSize.last().stages.size() == chain.stages.size()
                && std::equal(chain.stages.begin(), chain.stages.end(), forSize.last().stages.begin(),
                              [](const Stage &a, const Stage &b) { return a.sd == b.sd && a.percent == b.percent; }))
                forSize.removeLast();
            forSize.append(chain);
        }
        chains += forSize;
    }
    return chains;
}

QList<AnimeJaNai::Chain> AnimeJaNai::worked(const Cost &cost, bool light) {
    QList<Chain> chains;
    for (const Size size : kSizes) {
        const bool sd = int64_t(size.width) * size.height <= kSdPixels;
        const QList<Stages> &candidates = light ? (sd ? kSdLight : kHdLight) : (sd ? kSdFull : kHdFull);
        QList<Chain> forSize;
        for (const double rate : kRates) {
            const double budget = 1000.0 / rate * kUsed;
            double ceiling = budget;
            if (light) {
                const QList<Stages> &full = sd ? kSdFull : kHdFull;
                const auto fits = std::find_if(full.begin(), full.end(), [&](const Stages &s) { return stagesMs(cost, s, size.width, size.height) <= budget; });
                if (fits != full.end()) ceiling = std::min(budget, kLighter * stagesMs(cost, *fits, size.width, size.height));
            }
            const auto best = std::find_if(candidates.begin(), candidates.end(),
                                           [&](const Stages &s) { return stagesMs(cost, s, size.width, size.height) <= ceiling; });
            if (best == candidates.end()) continue;
            Chain chain{size.width, size.height, rate, *best};
            // The same chain a rate lower is the same chain a rate higher: keep the higher.
            if (!forSize.isEmpty() && forSize.last().stages.size() == chain.stages.size()
                && std::equal(chain.stages.begin(), chain.stages.end(), forSize.last().stages.begin(),
                              [](const Stage &a, const Stage &b) { return a.sd == b.sd && a.percent == b.percent; }))
                forSize.removeLast();
            forSize.append(chain);
        }
        chains += forSize;
    }
    return chains;
}

void AnimeJaNai::setTensorRt(bool on) {
    if (on == m_tensorRt) return;
    m_tensorRt = on;
    m_active = on ? m_cost : Cost{m_cost.fixed, m_cost.sd * 2, m_cost.hd * 2, m_cost.out, m_cost.rife};
    m_full = worked(m_active, false);
    m_light = worked(m_active, true);
    m_rife = workedWithRife(m_active);
}

bool AnimeJaNai::usable() const {
    return QFile::exists(root() + QStringLiteral("/inference/aji.dll"));
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

// Written when it differs from what was last written: every apply asks for the path.
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
