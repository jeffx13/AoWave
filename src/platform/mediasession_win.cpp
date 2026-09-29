#include "platform/mediasession.h"

#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QImage>
#include <QPainter>
#include <QPointer>
#include <QWindow>
#include <array>
#include <cstring>
#include <windows.h>
#include <shobjidl.h>
#include <unknwn.h>   // before C++/WinRT, for its classic COM interop
#include <systemmediatransportcontrolsinterop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.h>

namespace Media = winrt::Windows::Media;

namespace {

enum Button { Previous, PlayPause, Next };

HICON mediaIcon(int kind) {
    QImage image(32, 32, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(25, 30, 40, 160), 1));
    painter.setBrush(QColor(245, 248, 255));
    if (kind == 1) {
        painter.drawRoundedRect(QRectF(9, 7, 5, 18), 1, 1);
        painter.drawRoundedRect(QRectF(18, 7, 5, 18), 1, 1);
    } else if (kind == 0) {
        painter.drawPolygon(QPolygonF{QPointF(11, 7), QPointF(24, 16), QPointF(11, 25)});
    } else if (kind == 2) {
        painter.drawPolygon(QPolygonF{QPointF(23, 7), QPointF(10, 16), QPointF(23, 25)});
        painter.drawRoundedRect(QRectF(7, 7, 3, 18), 1, 1);
    } else {
        painter.drawPolygon(QPolygonF{QPointF(9, 7), QPointF(22, 16), QPointF(9, 25)});
        painter.drawRoundedRect(QRectF(22, 7, 3, 18), 1, 1);
    }
    painter.end();

    BITMAPINFO bitmap{};
    bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap.bmiHeader.biWidth = 32;
    bitmap.bmiHeader.biHeight = -32;
    bitmap.bmiHeader.biPlanes = 1;
    bitmap.bmiHeader.biBitCount = 32;
    bitmap.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HBITMAP color = CreateDIBSection(nullptr, &bitmap, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!color || !bits) return nullptr;
    std::memcpy(bits, image.constBits(), 32 * 32 * 4);
    HBITMAP mask = CreateBitmap(32, 32, 1, 1, nullptr);
    ICONINFO info{TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&info);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

}

struct MediaSession::Native final : QAbstractNativeEventFilter {
    MediaSession *owner = nullptr;
    QPointer<QWindow> window;
    HWND hwnd = nullptr;
    ITaskbarList3 *taskbar = nullptr;
    unsigned int taskbarCreated = 0;
    bool comInitialized = false;
    bool buttonsAdded = false;
    int lastButtonState = -1;
    std::array<HICON, 4> icons{};

    // The system media controls: the volume flyout's media overlay, the lock screen, and the
    // media keys while another window has focus.
    Media::SystemMediaTransportControls controls{nullptr};
    winrt::event_token buttonToken;
    double reportedPosition = -1;

    QString title;
    bool playing = false;
    double position = 0;
    double duration = 0;

    void attachControls() {
        try {
            const auto interop = winrt::get_activation_factory<Media::SystemMediaTransportControls,
                                                               ISystemMediaTransportControlsInterop>();
            winrt::check_hresult(interop->GetForWindow(hwnd, winrt::guid_of<Media::SystemMediaTransportControls>(),
                                                       winrt::put_abi(controls)));
            controls.IsPlayEnabled(true);
            controls.IsPauseEnabled(true);
            controls.IsPreviousEnabled(true);
            controls.IsNextEnabled(true);
            controls.DisplayUpdater().Type(Media::MediaPlaybackType::Video);
            controls.IsEnabled(false);
            // Raised on a thread pool thread.
            // A press can land while the session is being destroyed: only qApp is certain to
            // outlive it, and the session is checked back on the GUI thread.
            buttonToken = controls.ButtonPressed([session = QPointer<MediaSession>(owner)](const auto &,
                    const Media::SystemMediaTransportControlsButtonPressedEventArgs &args) {
                const Media::SystemMediaTransportControlsButton button = args.Button();
                QMetaObject::invokeMethod(qApp, [session, button] {
                    if (!session) return;
                    using enum Media::SystemMediaTransportControlsButton;
                    if (button == Previous) emit session->previousRequested();
                    if (button == Play || button == Pause) emit session->playPauseRequested();
                    if (button == Next) emit session->nextRequested();
                }, Qt::QueuedConnection);
            });
        } catch (const winrt::hresult_error &) {
            controls = nullptr;
        }
    }

    void detachControls() {
        if (!controls) return;
        controls.ButtonPressed(buttonToken);
        controls.IsEnabled(false);
        controls = nullptr;
    }

    void updateControls() {
        if (!controls) return;
        controls.IsEnabled(!title.isEmpty());
        if (title.isEmpty()) return;
        auto display = controls.DisplayUpdater();
        display.VideoProperties().Title(winrt::hstring(reinterpret_cast<const wchar_t *>(title.utf16()), title.size()));
        display.Update();
        controls.PlaybackStatus(playing ? Media::MediaPlaybackStatus::Playing : Media::MediaPlaybackStatus::Paused);
        reportedPosition = -1;
        updateTimeline();
    }

    void updateTimeline() {
        if (!controls || title.isEmpty() || duration <= 0) return;
        // The overlay advances its own clock; a report a second is plenty, plus every seek.
        if (reportedPosition >= 0 && position >= reportedPosition && position - reportedPosition < 1) return;
        reportedPosition = position;
        const auto span = [](double seconds) {
            return winrt::Windows::Foundation::TimeSpan(std::chrono::milliseconds(qint64(seconds * 1000)));
        };
        Media::SystemMediaTransportControlsTimelineProperties timeline;
        timeline.StartTime(span(0));
        timeline.MinSeekTime(span(0));
        timeline.EndTime(span(duration));
        timeline.MaxSeekTime(span(duration));
        timeline.Position(span(qMax(0.0, position)));
        controls.UpdateTimelineProperties(timeline);
    }

    bool nativeEventFilter(const QByteArray &type, void *message, qintptr *result) override {
        if ((type != "windows_generic_MSG" && type != "windows_dispatcher_MSG") || !message || !hwnd)
            return false;
        auto *event = static_cast<MSG *>(message);
        if (!window || event->hwnd != hwnd) return false;
        if (event->message == taskbarCreated) {
            buttonsAdded = false;
            lastButtonState = -1;
            QMetaObject::invokeMethod(owner, [this] { updateButtons(); }, Qt::QueuedConnection);
            return false;
        }
        int button = -1;
        if (event->message == WM_COMMAND && HIWORD(event->wParam) == THBN_CLICKED)
            button = int(LOWORD(event->wParam)) - 0x5100;
        if (event->message == WM_APPCOMMAND) {
            const int command = GET_APPCOMMAND_LPARAM(event->lParam);
            if (command == APPCOMMAND_MEDIA_PREVIOUSTRACK) button = Previous;
            if (command == APPCOMMAND_MEDIA_PLAY_PAUSE) button = PlayPause;
            if (command == APPCOMMAND_MEDIA_NEXTTRACK) button = Next;
        }
        if (button < Previous || button > Next) return false;
        // Queued: emitting from inside Qt's native callback re-enters qwindows mid-dispatch.
        QMetaObject::invokeMethod(owner, [owner = owner, button] {
            if (button == Previous)  emit owner->previousRequested();
            if (button == PlayPause) emit owner->playPauseRequested();
            if (button == Next)      emit owner->nextRequested();
        }, Qt::QueuedConnection);
        if (result) *result = 0;
        return true;
    }

    void updateButtons() {
        if (!taskbar || !hwnd) return;
        const int buttonState = (title.isEmpty() ? 0 : 2) | (playing ? 1 : 0);
        if (buttonsAdded && buttonState == lastButtonState) return;

        THUMBBUTTON buttons[3]{};
        const QStringList labels{MediaSession::tr("Previous episode"),
                                 playing ? MediaSession::tr("Pause") : MediaSession::tr("Play"),
                                 MediaSession::tr("Next episode")};
        const int iconFor[]{2, playing ? 1 : 0, 3};
        for (int i = 0; i < 3; ++i) {
            buttons[i].dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
            buttons[i].iId = 0x5100 + i;
            buttons[i].hIcon = icons[iconFor[i]];
            buttons[i].dwFlags = title.isEmpty() ? THBF_DISABLED : THBF_ENABLED;
            labels[i].left(259).toWCharArray(buttons[i].szTip);
        }
        if (buttonsAdded) taskbar->ThumbBarUpdateButtons(hwnd, 3, buttons);
        else buttonsAdded = SUCCEEDED(taskbar->ThumbBarAddButtons(hwnd, 3, buttons));
        lastButtonState = buttonState;
    }

    void updateProgress() {
        if (!taskbar || !hwnd) return;
        if (duration > 0) {
            taskbar->SetProgressState(hwnd, playing ? TBPF_NORMAL : TBPF_PAUSED);
            taskbar->SetProgressValue(hwnd, ULONGLONG(qMax(0.0, position) * 1000), ULONGLONG(duration * 1000));
        } else {
            taskbar->SetProgressState(hwnd, TBPF_NOPROGRESS);
        }
    }
};

MediaSession::MediaSession(QWindow *window) : QObject(window), m_native(std::make_unique<Native>()) {
    m_native->owner = this;
    m_native->window = window;
    if (!window) return;
    m_native->hwnd = reinterpret_cast<HWND>(window->winId());
    m_native->comInitialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    m_native->attachControls();
    if (FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskbarList3,
                                reinterpret_cast<void **>(&m_native->taskbar)))) return;
    if (FAILED(m_native->taskbar->HrInit())) {
        m_native->taskbar->Release();
        m_native->taskbar = nullptr;
        return;
    }
    for (int i = 0; i < 4; ++i) m_native->icons[i] = mediaIcon(i);
    m_native->taskbarCreated = RegisterWindowMessageW(L"TaskbarButtonCreated");
    QCoreApplication::instance()->installNativeEventFilter(m_native.get());
}

MediaSession::~MediaSession() {
    m_native->detachControls();
    QCoreApplication::instance()->removeNativeEventFilter(m_native.get());
    if (m_native->taskbar) m_native->taskbar->Release();
    for (HICON icon : m_native->icons) if (icon) DestroyIcon(icon);
    if (m_native->comInitialized) CoUninitialize();
}

void MediaSession::setTrack(const QString &title) {
    m_native->title = title;
    m_native->updateButtons();
    m_native->updateProgress();
    m_native->updateControls();
}

void MediaSession::setPlaying(bool playing) {
    m_native->playing = playing;
    m_native->updateButtons();
    m_native->updateProgress();
    m_native->updateControls();
}

void MediaSession::setProgress(double position, double duration) {
    m_native->position = position;
    m_native->duration = duration;
    m_native->updateProgress();
    m_native->updateTimeline();
}
