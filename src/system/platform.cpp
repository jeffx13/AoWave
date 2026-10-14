#include "system/platform.h"
#include "app/logger.h"
#include "app/settings.h"
#include "app/mainwindow.h"
#include <QByteArray>
#include <cstdlib>
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
#include <bcrypt.h>
#include <vector>
#include <dbghelp.h>
#include <psapi.h>
#include <QFile>
#include <QTextStream>
#include <csignal>
#include <cstdio>
#include <exception>
#include <QCryptographicHash>
#include <QLocalSocket>

namespace {

// Entropy mixed into every blob: an opaque id, not the display name, as changing it invalidates every stored secret.

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
    return QString::fromLatin1(cipher.toBase64());
}

QString Platform::unprotect(const QString &blob) {
    if (blob.isEmpty()) return {};

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
                            CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        qWarning("A saved secret could not be decrypted (error %lu)", GetLastError());
        return {};
    }

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

// _wputenv_s, not SetEnvironmentVariable: the CRT copy is what getenv and Qt read, and it sets both.
void Platform::keepStateInAppFolder(const QString &cacheDir) {
    const auto set = [](const wchar_t *name, const QString &path, bool folder = true) {
        QDir().mkpath(folder ? path : QFileInfo(path).path());
        _wputenv_s(name, reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(path).utf16()));
    };
    set(L"QML_DISK_CACHE_PATH", cacheDir + QStringLiteral("/qml"));
    set(L"QSG_RHI_PIPELINE_CACHE_SAVE", cacheDir + QStringLiteral("/qt/pipeline.bin"), false);
    if (QFileInfo::exists(cacheDir + QStringLiteral("/qt/pipeline.bin")))
        set(L"QSG_RHI_PIPELINE_CACHE_LOAD", cacheDir + QStringLiteral("/qt/pipeline.bin"), false);
    set(L"TMP", cacheDir + QStringLiteral("/tmp"));
    set(L"TEMP", cacheDir + QStringLiteral("/tmp"));
    set(L"XDG_CACHE_HOME", cacheDir);
    set(L"DENO_DIR", cacheDir + QStringLiteral("/deno"));
    set(L"CUDA_CACHE_PATH", cacheDir + QStringLiteral("/cuda"));
}

QString Platform::moduleDir() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&Platform::moduleDir), &self);
    return folderOf(self);
}

bool Platform::quiet() {
    static const bool on = qEnvironmentVariableIntValue("AOWAVE_QUIET") != 0;
    return on;
}

void Platform::keepDisplayAwake() {
    if (!quiet()) SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED);
}

bool Platform::onBattery() {
    SYSTEM_POWER_STATUS status{};
    return GetSystemPowerStatus(&status) && status.ACLineStatus == 0;
}
void Platform::allowDisplaySleep() { SetThreadExecutionState(ES_CONTINUOUS); }

bool Platform::replaceFile(const QString &from, const QString &to, QString *error) {
    const auto wide = [](const QString &path) { return QDir::toNativeSeparators(path).toStdWString(); };
    const bool moved = MoveFileExW(wide(from).c_str(), wide(to).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    const DWORD code = GetLastError();
    if (!moved && error) *error = QStringLiteral("Windows error %1").arg(code);
    return moved;
}

void Platform::closeSocket(qintptr handle) { ::closesocket(SOCKET(handle)); }
void Platform::resetOnClose(qintptr handle) {
    const linger immediate{1, 0};
    ::setsockopt(SOCKET(handle), SOL_SOCKET, SO_LINGER, reinterpret_cast<const char *>(&immediate), sizeof immediate);
}
void Platform::allowForegroundHandoff() { AllowSetForegroundWindow(ASFW_ANY); }

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
    if (quiet()) return;
    if (!g_notifyWindow) {
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = notifyWindowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = L"AoWaveNotifications";
        RegisterClassW(&windowClass);
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

Microsoft::WRL::ComPtr<IObjectArray> shellLinks(const QList<Platform::JumpItem> &items, const QSet<QString> &refused) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IObjectCollection> links;
    if (FAILED(CoCreateInstance(CLSID_EnumerableObjectCollection, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&links))))
        return nullptr;
    const std::wstring exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath()).toStdWString();
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
    if (quiet()) return;
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
    if (const auto array = shellLinks(recent, refused))
        list->AppendCategory(reinterpret_cast<LPCWSTR>(category.utf16()), array.Get());
    list->CommitList();
}

namespace {

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

bool switchable(HWND hwnd) {
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return false;
    DWORD process = 0;
    GetWindowThreadProcessId(hwnd, &process);
    if (process == GetCurrentProcessId()) return false;
    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((ex & WS_EX_TOOLWINDOW) && !(ex & WS_EX_APPWINDOW)) return false;
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
    for (HWND hwnd = GetTopWindow(nullptr); hwnd; hwnd = GetWindow(hwnd, GW_HWNDNEXT)) {
        if (hwnd == reinterpret_cast<HWND>(window) || !switchable(hwnd)) continue;
        return SetForegroundWindow(hwnd);
    }
    return false;
}

namespace {

class CngAes {
public:
    CngAes(const QByteArray &key, const wchar_t *mode) {
        if (BCryptOpenAlgorithmProvider(&m_alg, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) return;
        if (BCryptSetProperty(m_alg, BCRYPT_CHAINING_MODE,
                              reinterpret_cast<PUCHAR>(const_cast<wchar_t *>(mode)),
                              ULONG((wcslen(mode) + 1) * sizeof(wchar_t)), 0) != 0) return;
        if (BCryptGenerateSymmetricKey(m_alg, &m_key, nullptr, 0,
                                       reinterpret_cast<PUCHAR>(const_cast<char *>(key.constData())),
                                       key.size(), 0) != 0) return;
        m_ok = true;
    }
    ~CngAes() {
        if (m_key) BCryptDestroyKey(m_key);
        if (m_alg) BCryptCloseAlgorithmProvider(m_alg, 0);
    }
    CngAes(const CngAes &) = delete;
    CngAes &operator=(const CngAes &) = delete;

    bool ok() const { return m_ok; }
    BCRYPT_KEY_HANDLE handle() const { return m_key; }

    bool encryptBlock(const unsigned char *in, unsigned char *out) {
        ULONG n = 0;
        return BCryptEncrypt(m_key, const_cast<PUCHAR>(in), 16, nullptr,
                             nullptr, 0, out, 16, &n, 0) == 0;
    }

private:
    bool m_ok = false;
    BCRYPT_ALG_HANDLE m_alg = nullptr;
    BCRYPT_KEY_HANDLE m_key = nullptr;
};

BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO gcmInfo(const QByteArray &iv, unsigned char *tag) {
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = reinterpret_cast<PUCHAR>(const_cast<char *>(iv.constData()));
    info.cbNonce = ULONG(iv.size());
    info.pbTag   = tag;
    info.cbTag   = 16;
    return info;
}

}

QByteArray Aes::ctr(const QByteArray &key, QByteArray counter, const QByteArray &input) {
    if (key.size() != 32 || counter.size() != 16) return {};
    CngAes enc(key, BCRYPT_CHAIN_MODE_ECB);
    if (!enc.ok()) return {};

    QByteArray out(input.size(), Qt::Uninitialized);
    unsigned char ks[16];
    for (int off = 0; off < input.size(); off += 16) {
        if (!enc.encryptBlock(reinterpret_cast<const unsigned char *>(counter.constData()), ks))
            return {};
        const int blk = qMin(16, input.size() - off);
        for (int i = 0; i < blk; ++i)
            out[off + i] = static_cast<char>(static_cast<unsigned char>(input[off + i]) ^ ks[i]);
        for (int i = 15; i >= 0; --i) {
            unsigned char v = static_cast<unsigned char>(counter[i]) + 1;
            counter[i] = static_cast<char>(v);
            if (v != 0) break;
        }
    }
    return out;
}

QByteArray Aes::gcmDecrypt(const QByteArray &key, const QByteArray &iv, const QByteArray &ctWithTag) {
    constexpr int tagLen = 16;
    if (ctWithTag.size() < tagLen) return {};
    CngAes aes(key, BCRYPT_CHAIN_MODE_GCM);
    if (!aes.ok()) return {};

    const int ctLen = ctWithTag.size() - tagLen;
    const auto *ct = reinterpret_cast<const unsigned char *>(ctWithTag.constData());
    auto info = gcmInfo(iv, const_cast<unsigned char *>(ct + ctLen));
    QByteArray plaintext(ctLen, 0);
    ULONG resultLen = 0;
    if (BCryptDecrypt(aes.handle(), const_cast<PUCHAR>(ct), ctLen, &info, nullptr, 0,
                      reinterpret_cast<PUCHAR>(plaintext.data()), ctLen, &resultLen, 0) != 0)
        return {};
    plaintext.resize(resultLen);
    return plaintext;
}

QByteArray Aes::gcmEncrypt(const QByteArray &key, const QByteArray &iv, const QByteArray &plaintext) {
    CngAes aes(key, BCRYPT_CHAIN_MODE_GCM);
    if (!aes.ok()) return {};

    QByteArray out(plaintext.size() + 16, 0);
    auto *tag = reinterpret_cast<unsigned char *>(out.data()) + plaintext.size();
    auto info = gcmInfo(iv, tag);
    ULONG resultLen = 0;
    if (BCryptEncrypt(aes.handle(),
                      reinterpret_cast<PUCHAR>(const_cast<char *>(plaintext.constData())),
                      ULONG(plaintext.size()), &info, nullptr, 0,
                      reinterpret_cast<PUCHAR>(out.data()), ULONG(plaintext.size()), &resultLen, 0) != 0)
        return {};
    return out;
}

QByteArray Aes::cbcDecrypt(const QByteArray &key, const QByteArray &iv, const QByteArray &input) {
    if (iv.size() != 16 || input.isEmpty() || input.size() % 16 != 0) return {};
    CngAes aes(key, BCRYPT_CHAIN_MODE_CBC);
    if (!aes.ok()) return {};

    QByteArray chain = iv;
    QByteArray plain(input.size(), Qt::Uninitialized);
    ULONG length = 0;
    if (BCryptDecrypt(aes.handle(), reinterpret_cast<PUCHAR>(const_cast<char *>(input.constData())),
                      ULONG(input.size()), nullptr, reinterpret_cast<PUCHAR>(chain.data()), ULONG(chain.size()),
                      reinterpret_cast<PUCHAR>(plain.data()), ULONG(plain.size()), &length,
                      BCRYPT_BLOCK_PADDING) != 0)
        return {};
    plain.resize(qsizetype(length));
    return plain;
}

#ifndef APP_VERSION
#define APP_VERSION "0.0"
#endif

namespace {

QString g_crashDir;

const char *exceptionName(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:      return "ACCESS_VIOLATION";
    case EXCEPTION_STACK_OVERFLOW:        return "STACK_OVERFLOW";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:    return "FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_ILLEGAL_INSTRUCTION:   return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_PRIV_INSTRUCTION:      return "PRIV_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR:         return "IN_PAGE_ERROR";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "NONCONTINUABLE";
    case 0xE06D7363:                      return "C++ exception";
    default:                              return "unknown";
    }
}

// The process is already dying: Win32 and fixed buffers only, no allocation.
void write(HANDLE file, const char *text) {
    DWORD written = 0;
    WriteFile(file, text, DWORD(strlen(text)), &written, nullptr);
}

void writeLine(HANDLE file, const char *format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    write(file, buffer);
    write(file, "\r\n");
}

void timestamp(char *out, size_t size) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    snprintf(out, size, "%04d%02d%02d-%02d%02d%02d",
             t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}

void writeMiniDump(const wchar_t *path, EXCEPTION_POINTERS *ex) {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    MINIDUMP_EXCEPTION_INFORMATION info{};
    info.ThreadId = GetCurrentThreadId();
    info.ExceptionPointers = ex;
    info.ClientPointers = FALSE;

    // Stacks, modules and thread state, not the heap or globals where a session or key may sit: the dump may be sent on.
    const auto type = MINIDUMP_TYPE(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type,
                      ex ? &info : nullptr, nullptr, nullptr);
    CloseHandle(file);
}

void writeReport(const wchar_t *path, EXCEPTION_POINTERS *ex, const char *reason) {
    Log::flushLog(200);

    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    char when[32];
    timestamp(when, sizeof(when));
    writeLine(file, APP_NAME " %s crash report", APP_VERSION);
    writeLine(file, "time      : %s", when);
    writeLine(file, "reason    : %s", reason);

    void *faultPc = nullptr;
    if (ex && ex->ExceptionRecord) {
        const auto *record = ex->ExceptionRecord;
        faultPc = record->ExceptionAddress;
        writeLine(file, "exception : 0x%08lX (%s)",
                  record->ExceptionCode, exceptionName(record->ExceptionCode));
        writeLine(file, "address   : 0x%p", record->ExceptionAddress);
        if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
            const ULONG_PTR op = record->ExceptionInformation[0];
            writeLine(file, "operation : %s of 0x%p",
                      op == 0 ? "read" : op == 1 ? "write" : op == 8 ? "execute" : "?",
                      (void *)record->ExceptionInformation[1]);
        }
    }
    writeLine(file, "");

    writeLine(file, "--- stack (module+offset; the .dmp beside this file opens with the PDB) ---");
    void *frames[62];
    const USHORT count = CaptureStackBackTrace(0, 62, frames, nullptr);
    for (USHORT i = 0; i < count; ++i) {
        HMODULE module = nullptr;
        char name[MAX_PATH] = "?";
        uintptr_t offset = 0;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)frames[i], &module) && module) {
            GetModuleFileNameA(module, name, MAX_PATH);
            offset = uintptr_t(frames[i]) - uintptr_t(module);
        }
        const char *base = strrchr(name, '\\');
        writeLine(file, "  [%02d] 0x%p  %s+0x%llx", i, frames[i],
                  base ? base + 1 : name, (unsigned long long)offset);
    }
    if (faultPc) writeLine(file, "  (faulting pc above is the exception address)");
    writeLine(file, "");

    writeLine(file, "--- modules ---");
    HMODULE modules[256];
    DWORD needed = 0;
    if (EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) {
        const DWORD n = needed / sizeof(HMODULE);
        for (DWORD i = 0; i < n && i < 256; ++i) {
            char name[MAX_PATH] = "?";
            GetModuleFileNameA(modules[i], name, MAX_PATH);
            MODULEINFO mi{};
            GetModuleInformation(GetCurrentProcess(), modules[i], &mi, sizeof(mi));
            const char *base = strrchr(name, '\\');
            writeLine(file, "  0x%p +0x%08lx  %s", modules[i], mi.SizeOfImage,
                      base ? base + 1 : name);
        }
    }
    CloseHandle(file);
}

// Settings::logDir() as a plain buffer, set by install(): the handler cannot call Qt, and reportPending() looks here.
wchar_t g_logDir[MAX_PATH];

void buildPaths(wchar_t *report, wchar_t *dump, size_t size) {
    char when[32];
    timestamp(when, sizeof(when));
    swprintf(report, size, L"%s\\crash_%hs.txt", g_logDir, when);
    swprintf(dump, size, L"%s\\crash_%hs.dmp", g_logDir, when);
}

LONG WINAPI onUnhandled(EXCEPTION_POINTERS *ex) {
    static LONG entered = 0;
    if (InterlockedExchange(&entered, 1)) return EXCEPTION_EXECUTE_HANDLER;

    wchar_t report[MAX_PATH], dump[MAX_PATH];
    buildPaths(report, dump, MAX_PATH);
    writeReport(report, ex, "unhandled exception");
    writeMiniDump(dump, ex);
    return EXCEPTION_EXECUTE_HANDLER;
}

void onTerminate() {
    wchar_t report[MAX_PATH], dump[MAX_PATH];
    buildPaths(report, dump, MAX_PATH);

    const char *reason = "std::terminate";
    if (auto current = std::current_exception()) {
        try { std::rethrow_exception(current); }
        catch (const std::exception &e) {
            static char detail[512];
            snprintf(detail, sizeof(detail), "uncaught std::exception: %s", e.what());
            reason = detail;
        } catch (...) { reason = "uncaught non-std exception"; }
    }
    writeReport(report, nullptr, reason);
    writeMiniDump(dump, nullptr);
    _exit(3);
}

void onAbort(int) {
    wchar_t report[MAX_PATH], dump[MAX_PATH];
    buildPaths(report, dump, MAX_PATH);
    writeReport(report, nullptr, "abort()");
    writeMiniDump(dump, nullptr);
    _exit(3);
}

}

void CrashHandler::install() {
    QDir::toNativeSeparators(Settings::logDir()).left(MAX_PATH - 1).toWCharArray(g_logDir);
    SetUnhandledExceptionFilter(onUnhandled);
    ULONG guarantee = 65536;
    SetThreadStackGuarantee(&guarantee);
    std::set_terminate(onTerminate);
    signal(SIGABRT, onAbort);
}

QString CrashHandler::crashDir() {
    if (g_crashDir.isEmpty())
        g_crashDir = Settings::logDir();
    return g_crashDir;
}

void CrashHandler::reportPending() {
    QDir dir(crashDir());
    if (!dir.exists()) return;

    const auto all = dir.entryInfoList({QStringLiteral("crash_*")}, QDir::Files, QDir::Time);
    for (int i = 20; i < all.size(); ++i)
        QFile::remove(all[i].absoluteFilePath());

    const auto reports = dir.entryInfoList({QStringLiteral("crash_*.txt")}, QDir::Files, QDir::Time);
    if (reports.isEmpty()) return;

    const QFileInfo &latest = reports.first();
    static const QString kSeenKey = QStringLiteral("crash/lastReported");
    Settings &settings = Settings::instance();
    if (settings.value(kSeenKey).toString() == latest.fileName()) return;
    settings.setValue(kSeenKey, latest.fileName());

    logError() << "App" << "Previous run crashed -" << latest.fileName();

    QFile file(latest.absoluteFilePath());
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&file);
        for (int line = 0; line < 6 && !in.atEnd(); ++line) {
            const QString text = in.readLine().trimmed();
            if (!text.isEmpty()) logError() << "App" << text;
        }
    }

    MainWindow::instance().reportError(
        QObject::tr("%1 closed unexpectedly last time.\nReport: %2").arg(QStringLiteral(APP_NAME), latest.fileName()),
        QObject::tr("Crash report"));
}

SingleInstance::SingleInstance(const QString &key, QObject *parent) : QObject(parent) {
    // Pipe names are machine-wide, so another user's copy of the same folder must not answer.
    const QByteArray seed = (key.toLower() + QLatin1Char('|') + qEnvironmentVariable("USERNAME")).toUtf8();
    m_name = QStringLiteral(APP_NAME "-")
             + QString::fromLatin1(QCryptographicHash::hash(seed, QCryptographicHash::Sha1).toHex().left(16));
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&m_server, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket *socket = m_server.nextPendingConnection()) {
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
                if (!socket->canReadLine()) return;
                emit argumentReceived(QString::fromUtf8(socket->readLine()).chopped(1));
                socket->disconnectFromServer();
            });
        }
    });
}

bool SingleInstance::handOff(const QString &argument) {
    QLocalSocket socket;
    socket.connectToServer(m_name);
    if (!socket.waitForConnected(500)) return false;
    Platform::allowForegroundHandoff();
    socket.write(argument.toUtf8() + '\n');
    socket.disconnectFromServer();
    return socket.state() == QLocalSocket::UnconnectedState || socket.waitForDisconnected(2000);
}

bool SingleInstance::listen() {
    if (m_server.listen(m_name)) return true;
    QLocalServer::removeServer(m_name);
    return m_server.listen(m_name);
}
