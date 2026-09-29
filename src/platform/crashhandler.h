#pragma once
#include <QString>

// Only what kills the process from inside.
namespace CrashHandler {

// First thing in main(), before any Qt object exists.
void install();

void reportPending();

QString crashDir();

}
