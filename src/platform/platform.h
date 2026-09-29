#pragma once
#include <QList>
#include <QString>
#include <QtGlobal>
#include <functional>

// The OS seam: one implementation per platform, and nothing above it includes <windows.h>.
namespace Platform {

// Secrets at rest. OAuth tokens are bearer credentials.
// Windows: DPAPI, keyed to the logged-in user. Worthless on another account or machine.

QString protect(const QString &plain);

// Empty for anything this user cannot decrypt.
QString unprotect(const QString &blob);

// Idempotent.
void keepDisplayAwake();
void allowDisplaySleep();

// For a descriptor no QTcpSocket took ownership of.
void closeSocket(qintptr handle);

// Lets another process bring its window to the front, as a handed-off launch expects.
void allowForegroundHandoff();

// A desktop notification. `onClicked` runs on the GUI thread if the user clicks it.
void notify(const QString &title, const QString &message, std::function<void()> onClicked = {});

// The taskbar jump list, replaced whole: tasks, then a category of recent items. Each item starts
// the app with its argument.
struct JumpItem {
    QString title, argument;
    bool operator==(const JumpItem &) const = default;
};
void setJumpList(const QList<JumpItem> &tasks, const QString &category, const QList<JumpItem> &recent);

// "Open with" entries for these extensions ("mkv"), for this user. Never the default app: Windows
// leaves that choice to the user.
void setOpenWith(const QStringList &extensions, bool on);
// Whether they exist and start this copy of the app.
bool openWithRegistered();

// How much of a window other windows hide, 0 to 1; 1 when it is minimised. Click-through
// overlays and cloaked windows (other desktops, suspended apps) hide nothing.
double coveredFraction(quintptr window);
// On top of other windows or not, without taking focus from the one in use.
void setTopmost(quintptr window, bool topmost);
// Out of the taskbar, back where it was, without taking focus.
void restoreWithoutFocus(quintptr window);
// Windows 11's rounded corners and shadow for a frameless window; nothing on Windows 10.
void setRoundedCorners(quintptr window, bool rounded);
// The focus to the window Alt+Tab would go to from this one: the next app window below it, not
// minimised. False when there is none.
bool focusPreviousWindow(quintptr window);

// The running exe's folder, and the folder of the binary holding this code: app\bin in a release,
// where the app's own tools sit. Both work before a QCoreApplication exists.
QString exeDir();
QString moduleDir();

}
