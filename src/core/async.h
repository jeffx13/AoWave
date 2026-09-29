#pragma once
#include <QFuture>
#include <QList>
#include <QtGlobal>

// An exception escaping waitForFinished() at teardown is terminate.
template <typename Waitable>
void waitFor(Waitable &waitable, const char *what) {
    if (!waitable.isRunning()) return;
    try {
        waitable.waitForFinished();
    } catch (...) {
        qWarning("%s threw during shutdown", what);
    }
}

// Every background run an object has started and may still be waiting on, superseded ones
// included. Its owner cancels the runs' tokens, then waits here before anything they touch
// is destroyed.
class RunSet {
public:
    template <typename T>
    QFuture<T> add(QFuture<T> run) {
        m_runs.removeIf([](const QFuture<void> &f) { return f.isFinished(); });
        m_runs.append(run);
        return run;
    }

    void waitAll(const char *what) {
        for (QFuture<void> &run : m_runs) waitFor(run, what);
        m_runs.clear();
    }

private:
    QList<QFuture<void>> m_runs;
};
