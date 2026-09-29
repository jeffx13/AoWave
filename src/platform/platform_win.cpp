#include "platform/platform.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QSettings>
#include <QTimer>

#include <winsock2.h>   // before windows.h, which would pull in the old winsock
#include <windows.h>
#include <dpapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propsys.h>
#include <dwmapi.h>
#include <wrl/client.h>
#include <vector>

namespace {

// Entropy mixed into every blob. An opaque id, not the display name: changing it invalidates
// every stored secret, so leave it across a rename.

const char kEntropy[] = "aowave/token/v1";

DATA_BLOB entropyBlob() {
    DATA_BLOB blob{};
    blob.cbData = DWORD(sizeof(kEntropy) - 1);
    blob.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(kEntropy));
    return blob;
}

}

QString Platform::protect(const QString &plain) {
    if (plain.isEmpty()) return {};

    QByteArray source = plain.toUtf8();
    DATA_BLOB in{};
    in.cbData = DWORD(source.size());
    in.pbData = reinterpret_cast<BYTE *>(source.data());
    DATA_BLOB entropy = entropyBlob();
    DATA_BLOB out{};

    if (!CryptProtectData(&in, nullptr, &entropy, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out))
        return {};

    const QByteArray cipher(reinterpret_cast<const char *>(out.pbData), int(out.cbData));
    LocalFree(out.pbData);
    // Base64: this lands in an ini file.
    return QString::fromLatin1(cipher.toBase64());
}

QString Platform::unprotect(const QString &blob) {
    if (blob.isEmpty()) return {};

    // Strict: a non-base64 value is almost certainly plain text.
    const auto decoded = QByteArray::fromBase64Encoding(blob.toLatin1(),
                                                        QByteArray::Base64Encoding
                                                            | QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded) return {};

    QByteArray cipher = *decoded;
    DATA_BLOB in{};
    in.cbData = DWORD(cipher.size());
    in.pbData = reinterpret_cast<BYTE *>(cipher.data());
    DATA_BLOB entropy = entropyBlob();
    DATA_BLOB out{};

    if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &out))
        return {};

    const QString plain = QString::fromUtf8(reinterpret_cast<const char *>(out.pbData),
                                            int(out.cbData));
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return plain;
}

static QString folderOf(HMODULE module) {
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(module, path.data(), DWORD(path.size()));
    return QFileInfo(QString::fromWCharArray(path.data(), int(length))).absolutePath();
}

QString Platform::exeDir() { return folderOf(nullptr); }

QString Platform::moduleDir() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&Platform::moduleDir), &self);
    return folderOf(self);
}

void Platform::keepDisplayAwake() { SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED); }
void Platform::allowDisplaySleep() { SetThreadExecutionState(ES_CONTINUOUS); }

void Platform::closeSocket(qintptr handle) { ::closesocket(SOCKET(handle)); }
void Platform::allowForegroundHandoff() { AllowSetForegroundWindow(ASFW_ANY); }

// A tray icon's balloon, which Windows 10 and later show as a toast. Unlike the toast API it needs
// no registered app id, so nothing is written outside the app folder. The icon lives only as long
// as the notification needs it.
namespace {

constexpr UINT kNotifyMessage = WM_APP + 1;
std::function<void()> g_notificationClicked;
HWND g_notifyWindow = nullptr;
int g_notifyGeneration = 0;

LRESULT CALLBACK notifyWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == kNotifyMessage && (LOWORD(lParam) == NIN_BALLOONUSERCLICK || LOWORD(lParam) == WM_LBUTTONUP)) {
        if (g_notificationClicked) g_notificationClicked();
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

NOTIFYICONDATAW notifyIcon() {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = g_notifyWindow;
    data.uID = 1;
    return data;
}

void removeNotifyIcon() {
    NOTIFYICONDATAW data = notifyIcon();
    Shell_NotifyIconW(NIM_DELETE, &data);
}

}

void Platform::notify(const QString &title, const QString &message, std::function<void()> onClicked) {
    if (!g_notifyWindow) {
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = notifyWindowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = L"AoWaveNotifications";
        RegisterClassW(&windowClass);
        // Message-only: it exists to receive the icon's callbacks.
        g_notifyWindow = CreateWindowExW(0, windowClass.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                                         nullptr, windowClass.hInstance, nullptr);
        if (!g_notifyWindow) return;
        QObject::connect(qApp, &QCoreApplication::aboutToQuit, [] { removeNotifyIcon(); });
    }
    g_notificationClicked = std::move(onClicked);

    NOTIFYICONDATAW data = notifyIcon();
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_INFO;
    data.uCallbackMessage = kNotifyMessage;
    data.hIcon = LoadIconW(GetModuleHandleW(nullptr), L"IDI_APPICON");
    data.hBalloonIcon = data.hIcon;
    data.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
    // The arrays start zeroed, so copying one short of their size keeps them terminated.
    QStringLiteral(APP_NAME).left(127).toWCharArray(data.szTip);
    title.left(63).toWCharArray(data.szInfoTitle);
    message.left(255).toWCharArray(data.szInfo);
    if (!Shell_NotifyIconW(NIM_MODIFY, &data)) Shell_NotifyIconW(NIM_ADD, &data);

    const int generation = ++g_notifyGeneration;
    QTimer::singleShot(30000, qApp, [generation] {
        if (generation == g_notifyGeneration) removeNotifyIcon();
    });
}

namespace {

// Shell links that start this exe with each item's argument.
Microsoft::WRL::ComPtr<IObjectArray> shellLinks(const QList<Platform::JumpItem> &items, const QSet<QString> &refused) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IObjectCollection> links;
    if (FAILED(CoCreateInstance(CLSID_EnumerableObjectCollection, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&links))))
        return nullptr;
    const std::wstring exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath()).toStdWString();
    // PKEY_Title, spelled out: propkey.h defines it only under INITGUID.
    constexpr PROPERTYKEY kTitle{{0xF29F85E0, 0x4FF9, 0x1068, {0xAB, 0x91, 0x08, 0x00, 0x2B, 0x27, 0xB3, 0xD9}}, 2};
    UINT added = 0;
    for (const Platform::JumpItem &item : items) {
        if (refused.contains(item.argument)) continue;
        ComPtr<IShellLinkW> link;
        ComPtr<IPropertyStore> properties;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))
            || FAILED(link.As(&properties)))
            continue;
        link->SetPath(exe.c_str());
        link->SetArguments(reinterpret_cast<LPCWSTR>(item.argument.utf16()));
        link->SetIconLocation(exe.c_str(), 0);
        PROPVARIANT title{};
        title.vt = VT_LPWSTR;
        title.pwszVal = const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(item.title.utf16()));
        if (SUCCEEDED(properties->SetValue(kTitle, title)) && SUCCEEDED(properties->Commit())
            && SUCCEEDED(links->AddObject(link.Get())))
            ++added;
    }
    ComPtr<IObjectArray> array;
    if (added == 0 || FAILED(links.As(&array))) return nullptr;
    return array;
}

}

namespace {

const QString kProgId = QStringLiteral(APP_NAME ".Video");

QString openCommand() {
    return QStringLiteral("\"%1\" \"%2\"").arg(QDir::toNativeSeparators(QCoreApplication::applicationFilePath()),
                                             QStringLiteral("%1"));
}

}

void Platform::setOpenWith(const QStringList &extensions, bool on) {
    QSettings classes(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes"), QSettings::NativeFormat);
    for (const QString &extension : extensions) {
        const QString value = QStringLiteral(".%1/OpenWithProgids/%2").arg(extension, kProgId);
        if (on) classes.setValue(value, QString());
        else    classes.remove(value);
    }
    if (on) {
        const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
        // "." is a key's default value.
        classes.setValue(kProgId + QStringLiteral("/shell/open/command/."), openCommand());
        classes.setValue(kProgId + QStringLiteral("/DefaultIcon/."), QStringLiteral("\"%1\",0").arg(exe));
    } else {
        classes.remove(kProgId);
    }
    classes.sync();
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

bool Platform::openWithRegistered() {
    return QSettings(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes"), QSettings::NativeFormat)
               .value(kProgId + QStringLiteral("/shell/open/command/.")).toString() == openCommand();
}

void Platform::setJumpList(const QList<JumpItem> &tasks, const QString &category, const QList<JumpItem> &recent) {
    using Microsoft::WRL::ComPtr;
    ComPtr<ICustomDestinationList> list;
    ComPtr<IObjectArray> removed;
    UINT minSlots = 0;
    if (FAILED(CoCreateInstance(CLSID_DestinationList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&list)))
        || FAILED(list->BeginList(&minSlots, IID_PPV_ARGS(&removed))))
        return;

    // Windows refuses the whole category while it holds an entry the user removed.
    QSet<QString> refused;
    UINT removedCount = 0;
    removed->GetCount(&removedCount);
    for (UINT i = 0; i < removedCount; ++i) {
        ComPtr<IShellLinkW> link;
        wchar_t arguments[INFOTIPSIZE] = {};
        if (SUCCEEDED(removed->GetAt(i, IID_PPV_ARGS(&link))) && SUCCEEDED(link->GetArguments(arguments, INFOTIPSIZE)))
            refused.insert(QString::fromWCharArray(arguments));
    }

    if (const auto array = shellLinks(tasks, {})) list->AddUserTasks(array.Get());
    // Refused too while the user keeps recent items out of jump lists, a choice to leave alone.
    if (const auto array = shellLinks(recent, refused))
        list->AppendCategory(reinterpret_cast<LPCWSTR>(category.utf16()), array.Get());
    list->CommitList();
}

namespace {

// The window as drawn, without the invisible resize border GetWindowRect counts.
bool visibleBounds(HWND hwnd, RECT &rect) {
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof rect))
           || GetWindowRect(hwnd, &rect);
}

double area(const RECT &rect) {
    return double(qMax(0L, rect.right - rect.left)) * double(qMax(0L, rect.bottom - rect.top));
}

}

double Platform::coveredFraction(quintptr window) {
    const HWND self = reinterpret_cast<HWND>(window);
    if (!self || !IsWindowVisible(self) || IsIconic(self)) return 1.0;
    RECT mine;
    if (!visibleBounds(self, mine)) return 0.0;
    // Off every screen is out of sight too.
    const RECT desktop{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                       GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
                       GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
    const double total = area(mine);
    if (total <= 0) return 1.0;
    RECT onScreen;
    if (!IntersectRect(&onScreen, &mine, &desktop)) return 1.0;

    HRGN seen = CreateRectRgnIndirect(&onScreen);
    for (HWND above = GetWindow(self, GW_HWNDPREV); above; above = GetWindow(above, GW_HWNDPREV)) {
        if (!IsWindowVisible(above) || IsIconic(above)) continue;
        // Game and GPU overlays sit on top of everything, click-through and never activated.
        if (GetWindowLongPtrW(above, GWL_EXSTYLE) & (WS_EX_TRANSPARENT | WS_EX_NOACTIVATE)) continue;
        BOOL cloaked = FALSE;
        if (SUCCEEDED(DwmGetWindowAttribute(above, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked) continue;
        RECT theirs;
        if (!visibleBounds(above, theirs)) continue;
        HRGN cover = CreateRectRgnIndirect(&theirs);
        const int left = CombineRgn(seen, seen, cover, RGN_DIFF);
        DeleteObject(cover);
        if (left == NULLREGION || left == ERROR) break;
    }

    double visible = 0;
    if (const DWORD size = GetRegionData(seen, 0, nullptr)) {
        std::vector<char> buffer(size);
        auto *data = reinterpret_cast<RGNDATA *>(buffer.data());
        if (GetRegionData(seen, size, data)) {
            const auto *rects = reinterpret_cast<const RECT *>(data->Buffer);
            for (DWORD i = 0; i < data->rdh.nCount; ++i) visible += area(rects[i]);
        }
    }
    DeleteObject(seen);
    return qBound(0.0, 1.0 - visible / total, 1.0);
}

void Platform::setTopmost(quintptr window, bool topmost) {
    SetWindowPos(reinterpret_cast<HWND>(window), topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void Platform::restoreWithoutFocus(quintptr window) {
    ShowWindow(reinterpret_cast<HWND>(window), SW_SHOWNOACTIVATE);
}

void Platform::setRoundedCorners(quintptr window, bool rounded) {
    const DWM_WINDOW_CORNER_PREFERENCE preference = rounded ? DWMWCP_ROUND : DWMWCP_DEFAULT;
    DwmSetWindowAttribute(reinterpret_cast<HWND>(window), DWMWA_WINDOW_CORNER_PREFERENCE,
                          &preference, sizeof preference);
}

namespace {

// What Alt+Tab lists: a shown, uncloaked top-level app window of another process. Tool windows,
// owned popups and the desktop's own windows are not.
bool switchable(HWND hwnd) {
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return false;
    DWORD process = 0;
    GetWindowThreadProcessId(hwnd, &process);
    if (process == GetCurrentProcessId()) return false;
    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((ex & WS_EX_TOOLWINDOW) && !(ex & WS_EX_APPWINDOW)) return false;
    // Always-on-top windows sit above whatever was used last, whenever they were used.
    if (ex & (WS_EX_NOACTIVATE | WS_EX_TOPMOST)) return false;
    if (GetAncestor(hwnd, GA_ROOTOWNER) != hwnd && !(ex & WS_EX_APPWINDOW)) return false;
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked) return false;
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    return wcscmp(cls, L"Progman") != 0 && wcscmp(cls, L"WorkerW") != 0 && wcscmp(cls, L"Shell_TrayWnd") != 0
           && wcscmp(cls, L"Shell_SecondaryTrayWnd") != 0;
}

}

bool Platform::focusPreviousWindow(quintptr window) {
    // Z-order below the topmost band is the order windows were last active in.
    for (HWND hwnd = GetTopWindow(nullptr); hwnd; hwnd = GetWindow(hwnd, GW_HWNDNEXT)) {
        if (hwnd == reinterpret_cast<HWND>(window) || !switchable(hwnd)) continue;
        return SetForegroundWindow(hwnd);
    }
    return false;
}
