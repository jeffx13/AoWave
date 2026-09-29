#pragma once

// Prefer specific module headers: the module-level ones pull in hundreds and slow PCH generation.
#include <QObject>
#include <QString>
#include <QByteArray>
#include <QList>
#include <QMap>
#include <QHash>
#include <QSet>
#include <QVariant>
#include <QUrl>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTimer>
#include <QDateTime>
#include <QRegularExpression>
#include <QSharedPointer>
#include <QWeakPointer>
#include <QDebug>
#include <QCoreApplication>

#include <QtConcurrent/QtConcurrentRun>
#include <QFuture>
#include <QFutureWatcher>
#include <QGuiApplication>

#include <atomic>
#include <memory>
#include <functional>
#include <utility>
#include <algorithm>
