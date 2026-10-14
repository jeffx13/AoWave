#pragma once
#include <QObject>
#include <memory>

class QWindow;

class MediaSession : public QObject {
    Q_OBJECT
public:
    explicit MediaSession(QWindow *window);
    ~MediaSession() override;

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
