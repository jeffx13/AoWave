#pragma once
#include <QString>
#include <QVariantMap>

// Per-provider outcomes for this session. In memory only: last week says nothing about tonight.

namespace ProviderHealth {

// Called from Client for every request with a provider session. Thread-safe.
void record(const QString &owner, bool ok, qint64 elapsedMs);

QVariantMap statsFor(const QString &owner);

// Used by the Test button, so a retest is not judged by old failures.
void reset(const QString &owner);

}
