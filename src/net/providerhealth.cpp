#include "net/providerhealth.h"

#include <QHash>
#include <QList>
#include <QMutex>

#include <algorithm>

namespace {

// Bounded on purpose: forty failures an hour ago must stop dominating.
constexpr int kWindow = 60;

struct Record {
    QList<bool>   outcomes;    // newest last
    QList<qint64> latencies;   // successes only; a timeout's duration says nothing
};

QMutex g_mutex;
QHash<QString, Record> g_records;

template <typename T>
void push(QList<T> &list, T value) {
    list.append(value);
    if (list.size() > kWindow) list.removeFirst();
}

}

void ProviderHealth::record(const QString &owner, bool ok, qint64 elapsedMs) {
    if (owner.isEmpty()) return;
    QMutexLocker lock(&g_mutex);
    Record &entry = g_records[owner];
    push(entry.outcomes, ok);
    if (ok && elapsedMs >= 0) push(entry.latencies, elapsedMs);
}

QVariantMap ProviderHealth::statsFor(const QString &owner) {
    QMutexLocker lock(&g_mutex);
    const auto it = g_records.constFind(owner);
    if (it == g_records.constEnd() || it->outcomes.isEmpty()) return {};

    const int total = int(it->outcomes.size());
    int failures = 0;
    for (bool ok : it->outcomes) if (!ok) ++failures;

    // Median, not mean: one 30s timeout would swamp forty fast responses.
    qint64 median = 0;
    if (!it->latencies.isEmpty()) {
        QList<qint64> sorted = it->latencies;
        std::sort(sorted.begin(), sorted.end());
        median = sorted.at(sorted.size() / 2);
    }

    const double rate = double(total - failures) / total;
    return {
        {QStringLiteral("name"), owner},
        {QStringLiteral("requests"), total},
        {QStringLiteral("failures"), failures},
        {QStringLiteral("successRate"), rate},
        {QStringLiteral("medianMs"), median},
        // Forgiving: providers routinely 404 an episode that does not exist yet.
        {QStringLiteral("healthy"), rate >= 0.67},
    };
}

void ProviderHealth::reset(const QString &owner) {
    QMutexLocker lock(&g_mutex);
    if (owner.isEmpty()) g_records.clear();
    else                 g_records.remove(owner);
}
