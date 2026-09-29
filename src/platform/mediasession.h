#pragma once
#include <QObject>
#include <memory>

class QWindow;

// What the OS shows of playback: taskbar buttons and progress, the system media controls, and the
// media keys.
class MediaSession : public QObject {
    Q_OBJECT
public:
    explicit MediaSession(QWindow *window);
    ~MediaSession() override;

    // An empty title means nothing is loaded, which disables the buttons.
    void setTrack(const QString &title);
    void setPlaying(bool playing);
    void setProgress(double position, double duration);

signals:
    void previousRequested();
    void playPauseRequested();
    void nextRequested();

private:
    struct Native;
    std::unique_ptr<Native> m_native;
};
