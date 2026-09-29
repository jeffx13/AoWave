#include "core/danmakuoptions.h"

#include <QMutex>

namespace {

QMutex         g_optionsMutex;
DanmakuOptions g_options;

}

DanmakuOptions DanmakuOptions::current() {
    QMutexLocker lock(&g_optionsMutex);
    return g_options;
}

void DanmakuOptions::set(const DanmakuOptions &options) {
    QMutexLocker lock(&g_optionsMutex);
    g_options = options;
}
