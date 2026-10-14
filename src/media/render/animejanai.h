#pragma once
#include "media/render/upscaler.h"
#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QSize>
#include <QString>
#include <QTimer>
#include <cstdint>

class AnimeJaNai {
public:
    static constexpr int Slot = 1;
    static constexpr int LightSlot = 2;
    static constexpr int RifeSlot = 3;

    struct Stage {
        bool sd;
        int percent;
        int tier = 0;
    };
    struct Chain {
        int width, height;
        double fps;
        QList<Stage> stages;
        bool rife = false;
    };
    struct Cost {
        double fixed, sd, hd, out, rife;
    };
    static Cost defaultCost();

    explicit AnimeJaNai(const Cost &cost = defaultCost());

    void setTensorRt(bool on);
    void setCostScale(double scale);
    double costScale() const { return m_scale; }
    bool setScreen(QSize screen);
    QSize screen() const;
    bool setTiers(bool performance);
    bool hasPerformanceModel() const;
    bool tensorRt() const { return m_tensorRt; }
    bool usable() const;
    QString root() const;
    bool hasRife() const;
    QString confPath() const;
    QByteArray conf() const;

    const Chain *chainFor(int slot, int64_t width, int64_t height, double fps) const;
    double frameMs(const Chain &chain, int64_t width, int64_t height) const;

    struct Verdict {
        int slot = 0;
        double ms = 0;
        double budgetMs = 0;
        bool limited = false;
    };
    Verdict verdict(int64_t width, int64_t height, double fps, double speed, double headroom = 1.0) const;
    Verdict rifeVerdict(int64_t width, int64_t height, double fps, double speed, double headroom = 1.0) const;
    bool covers(int64_t width, int64_t height, double fps) const { return verdict(width, height, fps, 1).slot != 0; }

private:
    enum class Kind { Full, Light, Rife };
    static QList<Chain> worked(const Cost &cost, QSize screen, bool tiers, Kind kind);
    void rebuild();

    Cost m_cost;
    double m_scale = 1.0;
    QSize m_screen;
    bool m_tiers = false;
    Cost m_active;
    QList<Chain> m_full, m_light, m_rife;
    bool m_tensorRt = true;
    mutable QByteArray m_written;
};

class AnimeJaNaiBackend : public SteppedBackend {
    Q_OBJECT
public:
    AnimeJaNaiBackend(UpscalerHost &host, bool hasFilter, bool tensorRt, QObject *parent = nullptr);

    static constexpr int kCleanTicks = 600;

    QString id() const override { return QStringLiteral("animejanai"); }
    QString name() const override { return QStringLiteral("AnimeJaNai"); }
    bool offered() const override { return m_hasFilter && m_aji.usable(); }
    bool ownsFilter() const override { return true; }
    QString stepName() const override { return QStringLiteral("slot %1").arg(m_slot); }
    QString stepLabel() const override;
    UpscalerPlan plan(const VideoFacts &video, const UpscalerPlan &current) override;
    std::optional<UpscalerPlan> idle(const UpscalerPlan &current) override;
    QString notice(const VideoFacts &video) const override;
    void applied(const UpscalerPlan &before, const UpscalerPlan &after) override;
    void picked(const VideoFacts &video) override;
    void fileStarted(const UpscalerPlan &actual) override;
    void useCosts(UpscalerCosts *costs) override;
    void pollStatus();
    QVariantMap describe() const override;

protected:
    QList<int> rungs(const VideoFacts &video) const override;
    QString kindOf(const VideoFacts &video) const override;
    int initial(const QList<int> &rungs, const VideoFacts &) const override { return rungs.first(); }
    double predicted(int step, const VideoFacts &video) const override;
    double frameMs(const VideoFacts &video, const LoadSample &sample) const override;
    void moved(int from, int to, Move why, const VideoFacts &video) override;
    void remembered() override;
    void onLoad(const LoadSample &sample) override;

private:
    enum class Engines { Unknown, Building, Ready, Failed };
    void watchEngines();

    UpscalerPlan planFor(int slot) const;
    static int slotOf(int step);
    static double framesOf(int slot);
    static int stepOf(int slot);
    QString steppedNotice(const VideoFacts &video) const;
    void fitScreen(const VideoFacts &video);
    QString enhanceNotice(const VideoFacts &video) const;

    AnimeJaNai m_aji;
    bool m_hasFilter;
    int m_slot = 0;
    int m_revision = 0;
    double m_nextScale = 1.0;
    bool m_overloaded = false;
    int m_cleanTicks = 0;

    QString m_statsPath;
    QTimer m_statusTimer;
    QDateTime m_since;
    Engines m_engines = Engines::Unknown;
    int m_polls = 0;
};
