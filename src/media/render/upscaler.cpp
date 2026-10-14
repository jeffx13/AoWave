#include "media/render/upscaler.h"
#include "app/logger.h"
#include "app/settings.h"
#include "system/platform.h"
#include <algorithm>
#include <cmath>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>

DropWatch::DropWatch(UpscalerHost &host, std::function<bool()> counts, QObject *parent)
    : QObject(parent), m_host(host), m_counts(std::move(counts)) {
    m_timer.setInterval(kTickMs);
    connect(&m_timer, &QTimer::timeout, this, &DropWatch::tick);
}

void DropWatch::restart() {
    m_started = false;
    m_ticks = 0;
    m_badWindows = 0;
    m_lateTicks = 0;
    m_settling = true;
}

void DropWatch::tick() {
    const std::optional<int64_t> drops = m_host.intProperty("frame-drop-count");
    const std::optional<double> speed = m_host.doubleProperty("speed");
    if (!m_host.steady() || !m_counts() || !speed || !drops) {
        restart();
        return;
    }
    // The first seconds after a start or a change say nothing; after that the sound is the clock.
    const std::optional<double> late = m_host.doubleProperty("avsync");
    m_lateTicks = !m_settling && late && std::abs(*late) > 0.2 ? m_lateTicks + 1 : 0;
    if (m_lateTicks >= 2) {
        restart();
        emit overloaded();
        return;
    }
    // mpv restarts its counter when the filter or the output is rebuilt: that is not a drop.
    if (!m_started || *drops < m_dropsAt) {
        m_started = true;
        m_ticks = 0;
        m_dropsAt = *drops;
        return;
    }
    if (++m_ticks < 2) return;
    double fps = m_host.doubleProperty("container-fps").value_or(0);
    if (fps <= 0) fps = 24;
    const double due = fps * *speed * 2.0 * kTickMs / 1000.0;
    const double lost = double(*drops - m_dropsAt);
    const bool dropping = lost >= 2 && lost > due * 0.01;
    const bool settling = std::exchange(m_settling, false);
    m_badWindows = dropping && !settling ? m_badWindows + 1 : 0;
    m_ticks = 0;
    m_dropsAt = *drops;
    if (m_badWindows < 2) return;
    m_badWindows = 0;
    emit overloaded();
}

QString UpscalerBackend::notice(const VideoFacts &) const {
    return tr("Upscaler: %1").arg(name());
}

Upscaler::Upscaler(UpscalerHost &host) : m_host(host) {}

void Upscaler::init(const QByteArray &adapter) {
    const QByteArray vsrMode = adapter.startsWith("NVIDIA") ? "nvidia" : adapter.startsWith("Intel") ? "intel" : "";
    bool hasFilter = false;
    if (m_host.command({"vf", "add", "@aji-probe:animejanai"})) {
        hasFilter = true;
        m_host.command({"vf", "remove", "@aji-probe"});
    }
    for (const ShaderChain &chain : ShaderChains::all()) m_backends.push_back(new ShaderBackend(m_host, chain, this));
    m_backends.push_back(new VsrBackend(vsrMode, this));
    m_backends.push_back(new AnimeJaNaiBackend(m_host, hasFilter, vsrMode == "nvidia", this));
    for (UpscalerBackend *b : m_backends) {
        connect(b, &UpscalerBackend::replan, this, &Upscaler::apply);
        connect(b, &UpscalerBackend::note, this, [this](const QString &text) { m_host.showText(text); });
    }
    refreshOffers();

    m_refresh.setSingleShot(true);
    m_refresh.setInterval(300);
    connect(&m_refresh, &QTimer::timeout, this, &Upscaler::apply);
}

UpscalerBackend *Upscaler::backend(const QString &id) const {
    const auto it = std::find_if(m_backends.begin(), m_backends.end(), [&id](const UpscalerBackend *b) { return b->id() == id; });
    return it == m_backends.end() ? nullptr : *it;
}

// Looking for shader files and the package is file access; it is done when the settings change.
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
    if (next.filter.full() != m_current.filter.full()) {
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
    return plan.filter.label() == "@aji" && plan.filter.slot == AnimeJaNai::RifeSlot ? 2.0 : 1.0;
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
    // Another slot is a command to the filter there: no new filter, CUDA context or decoder.
    if (filter.slot >= 0 && m_current.filter.slot >= 0 && filter.spec == m_current.filter.spec) {
        if (filter.slot != m_current.filter.slot) {
            m_host.commandAsync({"vf-command", filter.label().mid(1), "slot", QByteArray::number(filter.slot)});
            // The filter takes a slot with the next frame: paused, this re-sends the current one.
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
     {"Anime4K_Clamp_Highlights.glsl", "Anime4K_Restore_CNN_M.glsl", "Anime4K_Upscale_CNN_x2_S.glsl"}},
    {"artcnn", "ArtCNN C4F32", {"ArtCNN_C4F32_DS.glsl", "CfL_Prediction.glsl"}, {"ArtCNN_C4F16_DS.glsl", "CfL_Prediction.glsl"}},
    {"artcnn-fast", "ArtCNN C4F16", {"ArtCNN_C4F16_DS.glsl", "CfL_Prediction.glsl"}, {}},
};

// Copies with lowered gates, in a folder named by the hash of their text: an updated shader makes a new one.
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

// Gates look like `//!WHEN OUTPUT.w MAIN.w / 1.200 > OUTPUT.h MAIN.h / 1.200 > *`; ArtCNN's use `LUMA.w / 1.3`.
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

ShaderBackend::ShaderBackend(UpscalerHost &host, const ShaderChain &chain, QObject *parent)
    : UpscalerBackend(parent), m_host(host), m_chain(chain),
      m_watch(host, [this] { return !m_shaders.isEmpty(); }, this) {
    connect(&m_watch, &DropWatch::overloaded, this, &ShaderBackend::stepDown);
}

bool ShaderBackend::all(std::initializer_list<const char *> files) const {
    const QString dir = Settings::dataDir() + QStringLiteral("/mpv/shaders/");
    return std::all_of(files.begin(), files.end(), [&dir](const char *file) { return QFile::exists(dir + QLatin1String(file)); });
}

bool ShaderBackend::offered() const { return all(m_chain.files); }

QString ShaderBackend::kindOf(const VideoFacts &video) {
    if (video.source.isEmpty() || video.fps <= 0) return {};
    return QStringLiteral("%1x%2@%3").arg(video.source.width()).arg(video.source.height()).arg(qRound(video.fps));
}

QString ShaderBackend::shaderList(std::initializer_list<const char *> files) const {
    const bool upscalesFirst = std::none_of(files.begin(), files.end(),
                                            [](const char *f) { return QLatin1String(f).contains(QLatin1String("Upscale_CNN_x2_VL")); });
    QStringList list;
    for (const char *file : files) {
        const QString name = QLatin1String(file);
        // The first upscaler of the chain starts at the video's own size; a second one stays where it is.
        const bool lowers = name.startsWith(QLatin1String("ArtCNN")) || name.contains(QLatin1String("Upscale_CNN_x2_VL"))
                            || (upscalesFirst && name.contains(QLatin1String("Upscale_CNN_x2_S")));
        const QString patched = lowers ? patchedPath(name, Settings::dataDir() + QStringLiteral("/mpv/shaders/") + name) : QString();
        list << (patched.isEmpty() ? QStringLiteral("~~/shaders/") + name : patched);
    }
    return list.join(QLatin1Char(';'));
}

UpscalerPlan ShaderBackend::plan(const VideoFacts &video, const UpscalerPlan &) {
    m_step = std::max(m_step, m_stepFor.value(kindOf(video)));
    if (m_step == 1 && (m_chain.lighter.size() == 0 || !all(m_chain.lighter))) m_step = 2;
    UpscalerPlan plan;
    if (m_step == 0) plan.shaders = shaderList(m_chain.files);
    else if (m_step == 1) plan.shaders = shaderList(m_chain.lighter);
    m_shaders = plan.shaders;
    return plan;
}

QString ShaderBackend::notice(const VideoFacts &video) const {
    if (const int step = std::max(m_step, m_stepFor.value(kindOf(video))); step > 0)
        return step == 1 ? tr("%1: lighter for this video, to keep up").arg(name())
                         : tr("%1: off for this video, as it cannot keep up").arg(name());
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

void ShaderBackend::picked(const VideoFacts &video) {
    m_step = 0;
    m_stepFor.remove(kindOf(video));
}

void ShaderBackend::fileStarted(const UpscalerPlan &) {
    m_step = 0;
    m_watch.restart();
}

void ShaderBackend::stepDown() {
    ++m_step;
    VideoFacts video;
    video.source = QSize(int(m_host.intProperty("video-dec-params/w").value_or(0)), int(m_host.intProperty("video-dec-params/h").value_or(0)));
    video.fps = m_host.doubleProperty("container-fps").value_or(0);
    if (const QString kind = kindOf(video); !kind.isEmpty()) m_stepFor[kind] = std::max(m_stepFor.value(kind), m_step);
    const bool lighter = m_step == 1 && m_chain.lighter.size() > 0 && all(m_chain.lighter);
    logInfo() << "Player" << QStringLiteral("%1 drops frames on this video: %2").arg(name(), lighter ? QStringLiteral("lighter") : QStringLiteral("off"));
    emit note(lighter ? tr("%1: lighter for this video, to keep up").arg(name())
                      : tr("%1: off for this video, as it cannot keep up").arg(name()));
    emit replan();
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
    // The size is unknown while the chain rebuilds: dropping the filter then would rebuild it once a second.
    if (video.source.isEmpty()) {
        if (current.filter.spec.startsWith("@vsr:")) plan.filter = current.filter;
        return plan;
    }
    const double scale = VsrBackend::scale(video.scale(), video.source.width(), video.source.height());
    if (scale > 0)
        plan.filter.spec = "@vsr:d3d11vpp=scale=" + QByteArray::number(scale, 'f', 2) + ":scaling-mode=" + m_mode;
    return plan;
}

// In tenths, and only ever up, since the driver does nothing at 1 or below.
double VsrBackend::scale(double fit, int64_t w, int64_t h) {
    double scale = std::min(4.0, std::floor(fit * 10.0) / 10.0);
    if (scale <= 1.0) return 0;
    // mpv sizes each side as (int)(scale * n) made even and crops at lrintf(scale * n) (vf_d3d11vpp.c): a
    // hundredth less at a time avoids a crop past the frame and a 1088-row frame NVIDIA leaves green.
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

namespace {
// A filter option's value with any ':' (a drive letter) or ',' in it: mpv's length prefix.
QByteArray filterValue(const QString &value) {
    const QByteArray utf8 = value.toUtf8();
    return '%' + QByteArray::number(utf8.size()) + '%' + utf8;
}
}

AnimeJaNaiBackend::AnimeJaNaiBackend(UpscalerHost &host, bool hasFilter, bool tensorRt, QObject *parent)
    : UpscalerBackend(parent), m_host(host), m_hasFilter(hasFilter), m_watch(host, [this] { return m_slot != 0; }, this) {
    m_aji.setTensorRt(tensorRt);
    for (const QString &entry : Settings::instance().get(Config::AnimeJaNaiCeilings)) {
        const qsizetype at = entry.lastIndexOf(QLatin1Char('='));
        if (at > 0) m_ceilingFor.insert(entry.left(at), entry.mid(at + 1).toInt());
    }
    m_statsPath = QDir::fromNativeSeparators(Settings::tempDir()) + QStringLiteral("/animejanai-status.txt");
    m_statusTimer.setInterval(2000);
    connect(&m_statusTimer, &QTimer::timeout, this, &AnimeJaNaiBackend::pollStatus);
    connect(&m_watch, &DropWatch::overloaded, this, &AnimeJaNaiBackend::stepDown);
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
    // NV12: mpv downloads each frame from CUDA, and its default 16-bit 4:4:4 is 50 MB a 4K frame.
    plan.filter.spec = spec + ":stats=" + filterValue(m_statsPath) + ":output-444=no";
    plan.filter.slot = slot;
    return plan;
}

UpscalerPlan AnimeJaNaiBackend::plan(const VideoFacts &video, const UpscalerPlan &) {
    UpscalerPlan plan = planFor(slotFor(video));
    // Frames the decoder skips never reach the filter: faster than the video, it is not asked for them all.
    if (plan.filter.slot != 0 && video.speed > 1.05) plan.framedrop = "decoder+vo";
    return plan;
}

// Once used it stays in the chain passing frames through: a slot switch takes 0.2 s, re-adding 0.5 s.
std::optional<UpscalerPlan> AnimeJaNaiBackend::idle(const UpscalerPlan &current) {
    if (current.filter.label() != "@aji") return std::nullopt;
    return planFor(0);
}

// Unplugged, the GPU is held back: a frame has less time than the chains were worked out for.
double AnimeJaNaiBackend::headroom() {
    return Platform::onBattery() ? 0.85 : 1.0;
}

QString AnimeJaNaiBackend::kindOf(const VideoFacts &video) {
    if (video.source.isEmpty() || video.fps <= 0) return {};
    return QStringLiteral("%1x%2@%3%4").arg(video.source.width()).arg(video.source.height()).arg(qRound(video.fps))
        .arg(video.speed > 1.05 ? QStringLiteral(" fast") : QString());
}

// Heaviest first: RIFE when it is asked for and has the time, the full and light slots while their chains keep up, then off.
QList<int> AnimeJaNaiBackend::ladder(const VideoFacts &video) const {
    QList<int> rungs;
    int slot = AnimeJaNai::Slot;
    if (!video.source.isEmpty() && video.fps > 0) {
        const int w = video.source.width(), h = video.source.height();
        slot = m_aji.verdict(w, h, video.fps, video.speed, headroom()).slot;
        if (slot != 0 && m_aji.hasRife() && Settings::instance().frameInterpolation()
            && m_aji.rifeVerdict(w, h, video.fps, video.speed, headroom()).slot != 0)
            rungs << AnimeJaNai::RifeSlot;
    }
    if (slot == AnimeJaNai::Slot) rungs << AnimeJaNai::Slot << AnimeJaNai::LightSlot;
    else if (slot == AnimeJaNai::LightSlot) rungs << AnimeJaNai::LightSlot;
    return rungs << 0;
}

int AnimeJaNaiBackend::weight(int slot) {
    switch (slot) {
    case AnimeJaNai::RifeSlot: return 3;
    case AnimeJaNai::Slot: return 2;
    case AnimeJaNai::LightSlot: return 1;
    default: return 0;
    }
}

int AnimeJaNaiBackend::ceilingOf(const VideoFacts &video) const {
    const int remembered = m_ceilingFor.value(kindOf(video), kNoCeiling);
    if (m_ceiling == kNoCeiling) return remembered;
    if (remembered == kNoCeiling) return m_ceiling;
    return weight(m_ceiling) <= weight(remembered) ? m_ceiling : remembered;
}

int AnimeJaNaiBackend::slotFor(const VideoFacts &video) const {
    const QList<int> rungs = ladder(video);
    const int ceiling = ceilingOf(video);
    if (ceiling == kNoCeiling) return rungs.first();
    for (const int slot : rungs)
        if (weight(slot) <= weight(ceiling)) return slot;
    return 0;
}

QString AnimeJaNaiBackend::steppedNotice(const VideoFacts &video) const {
    const QList<int> rungs = ladder(video);
    const int now = slotFor(video);
    if (now == rungs.first()) return {};
    if (now == 0) return tr("%1: off for this video, as it cannot keep up").arg(name());
    return rungs.first() == AnimeJaNai::RifeSlot && now == AnimeJaNai::Slot
               ? tr("%1: no frame interpolation for this video, to keep up").arg(name())
               : tr("%1: lighter for this video, to keep up").arg(name());
}

void AnimeJaNaiBackend::picked(const VideoFacts &video) {
    m_ceiling = kNoCeiling;
    if (m_ceilingFor.remove(kindOf(video))) rememberCeilings();
}

void AnimeJaNaiBackend::rememberCeilings() const {
    QStringList entries;
    for (auto it = m_ceilingFor.constBegin(); it != m_ceilingFor.constEnd(); ++it)
        entries << it.key() + QLatin1Char('=') + QString::number(it.value());
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
        // RIFE has an engine of its own to build the first time, which the status file reports like the others.
        if (after.filter.slot == AnimeJaNai::RifeSlot) watchEngines();
    } else if (!is) {
        m_watch.stop();
        m_statusTimer.stop();
    }
}

void AnimeJaNaiBackend::fileStarted(const UpscalerPlan &actual) {
    if (actual.filter.label() == "@aji") m_slot = actual.filter.slot;
    m_ceiling = kNoCeiling;
    m_watch.restart();
    if (actual.filter.label() == "@aji") watchEngines();
}

void AnimeJaNaiBackend::restartWatch() {
    m_watch.restart();
}

void AnimeJaNaiBackend::stepDown() {
    VideoFacts video;
    video.speed = m_host.doubleProperty("speed").value_or(1.0);
    video.source = QSize(int(m_host.intProperty("video-dec-params/w").value_or(0)), int(m_host.intProperty("video-dec-params/h").value_or(0)));
    video.fps = m_host.doubleProperty("container-fps").value_or(0);
    const QList<int> rungs = ladder(video);
    const qsizetype at = rungs.indexOf(slotFor(video));
    m_ceiling = rungs.at(std::min<qsizetype>(at + 1, rungs.size() - 1));
    if (const QString kind = kindOf(video); !kind.isEmpty()) {
        m_ceilingFor[kind] = m_ceiling;
        rememberCeilings();
    }
    logInfo() << "Player" << QStringLiteral("AnimeJaNai drops frames on this video: slot %1 now").arg(m_ceiling);
    emit note(steppedNotice(video));
    emit replan();
}

QVariantMap AnimeJaNaiBackend::describe() const {
    QVariantMap map = UpscalerBackend::describe();
    map.insert(QStringLiteral("interpolation"), m_aji.tensorRt());   // RIFE runs on TensorRT only
    return map;
}

void AnimeJaNaiBackend::watchEngines() {
    m_since = QDateTime::currentDateTime().addSecs(-1);
    m_engines = Engines::Unknown;
    m_polls = 0;
    m_statusTimer.start();
}

// The filter's stats file: "Building TensorRT engine for ..." while trtexec works (the video plays as it is
// meanwhile), "Applied Model: ..." once the chain runs, "Engine build FAILED ..." if it cannot.
void AnimeJaNaiBackend::pollStatus() {
    QFile file(m_statsPath);
    const bool fresh = !m_since.isValid() || QFileInfo(file).lastModified() >= m_since;
    if (!fresh || !file.open(QIODevice::ReadOnly)) {
        // Not written at all: DirectML, or a video paused before its first frame. Not worth watching for ever.
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
