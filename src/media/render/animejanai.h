#pragma once
#include <QByteArray>
#include <QList>
#include <QString>
#include <cstdint>

// aji takes the first chain whose size and rate hold the video; with none, the video plays as it is.
class AnimeJaNai {
public:
    static constexpr int Slot = 1;
    static constexpr int LightSlot = 2;
    static constexpr int RifeSlot = 3;

    struct Stage {
        bool sd;
        int percent;
    };
    struct Chain {
        int width, height;
        double fps;
        QList<Stage> stages;
        bool rife = false;
    };
    // Milliseconds: fixed, per megapixel into the SD and HD models, of picture out (read back for mpv), and of source for RIFE.
    struct Cost {
        double fixed, sd, hd, out, rife;
    };
    static Cost defaultCost();

    explicit AnimeJaNai(const Cost &cost = defaultCost());

    // DirectML needs no engines but runs at half the speed (45 against 94 fps at 640x480), and has no RIFE.
    void setTensorRt(bool on);
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
    static QList<Chain> worked(const Cost &cost, bool light);
    static QList<Chain> workedWithRife(const Cost &cost);

    Cost m_cost;
    Cost m_active;
    QList<Chain> m_full, m_light, m_rife;
    bool m_tensorRt = true;
    mutable QByteArray m_written;
};
