#include "net/cloudflare.h"

#include <QApplication>
#include <QAtomicInteger>
#include <QCloseEvent>
#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QDeadlineTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMutex>
#include <QReadWriteLock>
#include <QRegularExpression>
#include <QSemaphore>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVarLengthArray>
#include <QWebEngineCookieStore>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineSettings>
#include <QWebEngineView>
#include <QPointer>
#include <atomic>
#include <functional>
#include <memory>

#include "core/logger.h"
#include "core/settings.h"

namespace {

using namespace Cloudflare;

constexpr int    kIdleSweepMs         = 30000;
constexpr qint64 kIdleCloseMs         = 90000;
constexpr qint64 kMaxBrowserBodyBytes = 32LL * 1024 * 1024;
// A stuck interstitial has usually lost its callback by the halfway mark; reloading later
// than this leaves no time for the retry to finish.
constexpr int    kReloadAtPercent     = 55;
// The hidden page clears Cloudflare's managed check by itself in 5 to 10 s (animepahe.pw,
// 2026-09-29). One still up after this is put in front of the user, who may have to tick "Verify
// you are human", and gets this long to.
constexpr qint64 kHumanAfterMs        = 15000;
constexpr qint64 kHumanTimeoutMs      = 180000;

constexpr const char *kFallbackUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/140.0.0.0 Safari/537.36";

// The default carries a "QtWebEngine/6.x.y" token. Strip it and change nothing else, no Chrome
// version and no client-hint headers: Cloudflare checks them against what the page's scripts read
// (navigator.userAgentData), and refuses a clearance to a browser that differs from the one that
// earned it, so the check reloads for ever.
QString disguisedUserAgent(const QString &defaultAgent) {
    if (defaultAgent.isEmpty()) return QString::fromLatin1(kFallbackUserAgent);
    static const QRegularExpression qtToken(QStringLiteral("\\s*QtWebEngine/[^\\s)]+"));
    QString agent = defaultAgent;
    agent.remove(qtToken);
    return agent.simplified();
}

// Set once the engine has run a page, any page. Until then a dead render process means the
// engine cannot run here at all, rather than that one page crashed it.
std::atomic_bool g_engineRan{false};
std::atomic_bool g_engineBroken{false};

// Whether this worker's last settle ended on a firewall block rather than an unfinished
// challenge - the two want very different advice. Per thread, because the solver is one
// object shared by every worker and each is in the middle of its own request.
thread_local bool g_lastWasRefusal = false;
// How this worker's last settle went with a check put in front of the user, if it came to one.
enum class HumanCheck { None, Passed, Closed, TimedOut };
thread_local HumanCheck g_lastHumanCheck = HumanCheck::None;

// JSON is a JavaScript literal and QJsonDocument does the escaping.
QString jsLiteral(const QJsonValue &value) {
    return QString::fromUtf8(QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact))
        .mid(1).chopped(1);
}

// The page a browser would be on when it asks for `url`: Cloudflare issues the clearance on
// an ordinary page of the site, and a JSON endpoint answers a document navigation with a bare
// 403 that carries no challenge at all.
QUrl originRoot(const QUrl &url) {
    QUrl root;
    root.setScheme(url.scheme());
    root.setHost(url.host());
    if (url.port() != -1) root.setPort(url.port());
    root.setPath(QStringLiteral("/"));
    return root;
}

// One lock per provider. Navigating the page is exclusive; an in-page fetch on an origin that
// is already settled is shared, so a grid of posters does not queue behind itself.
// Deliberately immortal: a lane outlives the session it guards, and destroying one at static
// teardown while a worker still held it would be undefined behaviour. There is one per
// provider, so the bound is the provider list.
QReadWriteLock *laneFor(const QString &owner) {
    static QMutex mutex;
    static QHash<QString, QReadWriteLock *> lanes;
    QMutexLocker lock(&mutex);
    QReadWriteLock *&entry = lanes[owner];
    if (!entry) entry = new QReadWriteLock;
    return entry;
}

// Short enough to notice cancellation promptly, never longer than what is left. Spelled out
// rather than via qBound(), whose mixed-type overloads are ambiguous for (int, qint64, int).
int waitSliceMs(const QDeadlineTimer &deadline) {
    const qint64 remaining = deadline.remainingTime();
    if (remaining < 1) return 1;
    return remaining > 50 ? 50 : int(remaining);
}

// A cancelled or timed-out request must be able to give up while queued.
class Lane {
public:
    enum Mode { Shared, Exclusive };

    Lane(const QString &owner, CancelToken cancel, const QDeadlineTimer &deadline)
        : m_lock(laneFor(owner)), m_cancel(std::move(cancel)), m_deadline(deadline) {}
    ~Lane() { release(); }
    Lane(const Lane &) = delete;
    Lane &operator=(const Lane &) = delete;

    bool acquire(Mode mode) {
        release();
        while (!m_cancel.isCancelled() && !m_deadline.hasExpired()) {
            const int slice = waitSliceMs(m_deadline);
            if (mode == Shared ? m_lock->tryLockForRead(slice) : m_lock->tryLockForWrite(slice)) {
                m_held = true;
                return true;
            }
        }
        return false;
    }

    void release() {
        if (!m_held) return;
        m_held = false;
        m_lock->unlock();
    }

private:
    QReadWriteLock *m_lock;
    CancelToken     m_cancel;
    QDeadlineTimer  m_deadline;
    bool            m_held = false;
};

// The hidden page, shown in a window of its own for a check only a person can pass: the same
// page, so the clearance it earns is the one the app's requests carry. Closing it gives up.
class CheckWindow final : public QWebEngineView {
public:
    explicit CheckWindow(QWebEnginePage *page) {
        setPage(page);
        resize(520, 620);
        // Closing the app's window still quits it.
        setAttribute(Qt::WA_QuitOnClose, false);
    }
    void ask(const QString &host, std::function<void()> onClosed) {
        m_onClosed = std::move(onClosed);
        setWindowTitle(QStringLiteral("%1 wants to check you're human").arg(host));
        show();
        raise();
        activateWindow();
    }
    // Cleared, or torn down with its session: closing is no longer giving up.
    void finish() { m_onClosed = nullptr; }

protected:
    void closeEvent(QCloseEvent *event) override {
        if (const auto onClosed = std::exchange(m_onClosed, nullptr)) onClosed();
        QWebEngineView::closeEvent(event);
    }

private:
    std::function<void()> m_onClosed;
};

struct Session {
    QWebEngineProfile *profile = nullptr;
    QWebEnginePage    *page    = nullptr;
    QPointer<CheckWindow> check;   // while the user is asked to pass a check
    QString userAgent;
    QUrl    settledUrl;     // the document the page came to rest on, empty while navigating
    quint64 navigation = 0; // bumped per load, so a stale poller cannot publish an origin
    qint64  lastUsedMs = 0;

    Session() = default;
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;
    // Qt owns the render process, so closing a session is a delete - and the profile has to
    // outlive its pages.
    ~Session() {
        if (check) { check->finish(); delete check.data(); }
        delete page;
        delete profile;
    }
};

// Every member runs on the GUI thread.
class Host : public QObject {
public:
    static Host &instance() {
        static Host *host = [] {
            auto *created = new Host;
            created->moveToThread(QCoreApplication::instance()->thread());
            return created;
        }();
        return *host;
    }

    Session *find(const QString &owner) {
        const auto it = m_sessions.constFind(owner);
        return it == m_sessions.constEnd() ? nullptr : it->get();
    }

    Session &session(const QString &owner) {
        std::shared_ptr<Session> &slot = m_sessions[owner];
        if (slot) {
            slot->lastUsedMs = QDateTime::currentMSecsSinceEpoch();
            return *slot;
        }
        slot = std::make_shared<Session>();
        Session &entry = *slot;

        // On disk, not off the record: cf_clearance is bound to this profile's fingerprint,
        // which a restart does not change, so the next run skips the challenge entirely. In
        // data\browser with the rest of the app's state; its cache goes with the others.
        entry.profile = new QWebEngineProfile(QStringLiteral("aowave-") + owner);
        entry.profile->setPersistentStoragePath(Settings::dataDir() + QStringLiteral("/browser/") + owner);
        entry.profile->setCachePath(Settings::tempDir() + QStringLiteral("/browser/") + owner);
        entry.profile->setPersistentCookiesPolicy(QWebEngineProfile::ForcePersistentCookies);
        entry.userAgent = disguisedUserAgent(entry.profile->httpUserAgent());
        entry.profile->setHttpUserAgent(entry.userAgent);
        // A headless scraper usually sends none.
        entry.profile->setHttpAcceptLanguage(QStringLiteral("en-US,en;q=0.9"));

        // So a later plain Client request carries the same clearance. Binding the User-Agent
        // here rather than at the end of a solve also covers the cookies the profile restores
        // from disk: a clearance replayed without the agent that earned it is spent on the
        // first direct request of the session.
        QWebEngineCookieStore *jar = entry.profile->cookieStore();
        const QString agent = entry.userAgent;
        connect(jar, &QWebEngineCookieStore::cookieAdded, this,
                [owner, agent](const QNetworkCookie &cookie) {
                    CookieStore::instance().insert({cookie}, owner);
                    if (cookie.name() != "cf_clearance" || cookie.value().isEmpty()) return;
                    QString domain = cookie.domain();
                    if (domain.startsWith(QLatin1Char('.'))) domain.remove(0, 1);
                    rememberUserAgent(domain, agent, owner);
                });
        jar->loadAllCookies();   // replays the restored cookies through the mirror above

        entry.page = new QWebEnginePage(entry.profile);
        QWebEngineSettings *settings = entry.page->settings();
        settings->setAttribute(QWebEngineSettings::JavascriptEnabled, true);
        settings->setAttribute(QWebEngineSettings::LocalStorageEnabled, true);
        // Turnstile lays itself out around images.
        settings->setAttribute(QWebEngineSettings::AutoLoadImages, true);
        settings->setAttribute(QWebEngineSettings::JavascriptCanAccessClipboard, false);
        settings->setAttribute(QWebEngineSettings::PlaybackRequiresUserGesture, true);
        settings->setAttribute(QWebEngineSettings::WebGLEnabled, true);
        settings->setAttribute(QWebEngineSettings::ScrollAnimatorEnabled, false);
        entry.page->setBackgroundColor(Qt::transparent);

        entry.lastUsedMs = QDateTime::currentMSecsSinceEpoch();
        startIdleSweep();
        return entry;
    }

    void closeAll() { m_sessions.clear(); }

private:
    Host() = default;

    void startIdleSweep() {
        if (m_idle) return;
        m_idle = new QTimer(this);
        m_idle->setInterval(kIdleSweepMs);
        connect(m_idle, &QTimer::timeout, this, [this]() { closeIdle(); });
        m_idle->start();
    }

    void closeIdle() {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        for (auto it = m_sessions.begin(); it != m_sessions.end();) {
            if (now - (*it)->lastUsedMs < kIdleCloseMs) { ++it; continue; }
            // A request in flight always holds the lane, so failing to take it exclusively is
            // what proves the session is idle. Non-blocking: this is the GUI thread.
            QReadWriteLock *lane = laneFor(it.key());
            if (!lane->tryLockForWrite()) { ++it; continue; }
            logInfo() << "Cloudflare" << "closing the idle" << it.key() << "web engine session";
            it = m_sessions.erase(it);
            lane->unlock();
        }
    }

    QHash<QString, std::shared_ptr<Session>> m_sessions;
    QTimer *m_idle = nullptr;
};

// False if the deadline passed or the request was cancelled first; `task` may still run
// afterwards, so it must only touch things that outlive the call.
bool runOnGui(std::function<void()> task, const QDeadlineTimer &deadline,
              const CancelToken &cancel) {
    if (QThread::currentThread() == QCoreApplication::instance()->thread()) {
        task();
        return true;
    }
    auto done = std::make_shared<QSemaphore>();
    QMetaObject::invokeMethod(&Host::instance(), [task = std::move(task), done]() {
        task();
        done->release();
    }, Qt::QueuedConnection);
    while (!deadline.hasExpired() && !cancel.isCancelled())
        if (done->tryAcquire(1, waitSliceMs(deadline))) return true;
    return false;
}

// runJavaScript cannot return a Promise, so the fetch stashes its result on window and this
// polls for it. Keyed by id, because fetches on a settled origin run concurrently.
struct FetchState {
    QSemaphore done;
    QMutex mutex;
    QString payload;
};

// Evaluates `probe` until it returns a non-empty string.
void pollFor(QWebEnginePage *page, const QString &probe, const std::shared_ptr<FetchState> &state,
             qint64 deadlineMs, int attempt = 0) {
    if (QDateTime::currentMSecsSinceEpoch() > deadlineMs) { state->done.release(); return; }
    page->runJavaScript(probe, [page, probe, state, deadlineMs, attempt](const QVariant &value) {
        const QString payload = value.toString();
        if (!payload.isEmpty()) {
            QMutexLocker lock(&state->mutex);
            state->payload = payload;
            state->done.release();
            return;
        }
        // Tight for the first half second, then back off: a small body is usually there
        // immediately, and a large one is not worth spinning on.
        const int wait = attempt < 50 ? 10 : 50;
        QTimer::singleShot(wait, page, [page, probe, state, deadlineMs, attempt]() {
            pollFor(page, probe, state, deadlineMs, attempt + 1);
        });
    });
}

void pollFetched(QWebEnginePage *page, const std::shared_ptr<FetchState> &state, quint64 id,
                 qint64 deadlineMs) {
    pollFor(page,
            QStringLiteral("(function(){var s=window.__aoFetch;if(!s)return '';var v=s['%1'];"
                           "if(!v)return '';delete s['%1'];return v;})()").arg(id),
            state, deadlineMs);
}

struct SettleState {
    QSemaphore done;
    // The GUI thread writes it, the worker reads it after the deadline as well as the semaphore.
    std::atomic_bool ok{false};
    // A firewall rule, not a challenge: the page rendered, and what it says is no. Waiting
    // longer, reloading, or solving anything cannot change that answer.
    std::atomic_bool refused{false};
    bool reloaded = false;   // GUI thread only
    // Moved on when a check is put in front of the user, who needs longer than a script.
    std::atomic<qint64> deadlineMs{0};
    qint64 challengedSinceMs = 0;   // GUI thread only
    std::atomic_bool askedHuman{false};
    std::atomic_bool closedByUser{false};
    // The watcher's own bookkeeping; GUI thread only.
    QPointer<QTimer> watcher;
    bool finished = false;
    qint64 probeSentMs = 0;   // 0 while no probe is out

    void note(const QString &text) { QMutexLocker lock(&m_noteMutex); m_note = text; }
    QString lastNote() const {
        QMutexLocker lock(&m_noteMutex);
        return m_note.isEmpty() ? QStringLiteral("the page never reported a finished load - "
                                                 "the web engine may not have started")
                                : m_note;
    }

private:
    mutable QMutex m_noteMutex;
    QString m_note;
};

// Ends a settle's watch and wakes its worker. GUI thread.
void finishSettle(const std::shared_ptr<SettleState> &state) {
    if (state->finished) return;
    state->finished = true;
    if (state->watcher) state->watcher->deleteLater();
    state->done.release();
}

// Puts the page in front of the user; false where there is no widget application to do it in.
bool askHuman(Session *entry, const QString &host, const std::shared_ptr<SettleState> &state) {
    if (!qobject_cast<QApplication *>(QCoreApplication::instance())) return false;
    if (!entry->check) entry->check = new CheckWindow(entry->page);
    entry->check->ask(host, [state] {
        state->closedByUser = true;
        finishSettle(state);
    });
    logInfo() << "Cloudflare" << host << "wants a person to confirm they are human; asking";
    return true;
}

void closeCheck(Session *entry) {
    if (!entry || !entry->check) return;
    entry->check->finish();
    entry->check->hide();
}

// Probes the page until it settles, is refused, is given up on or runs out of time. A timer, not
// a chain of replies: a navigation drops a pending runJavaScript reply, and a page navigates by
// itself as its check clears.
void watchSettle(QWebEnginePage *page, const std::shared_ptr<SettleState> &state,
                 const QString &owner, const QString &expectedOwner, quint64 navigation,
                 qint64 reloadAtMs) {
    // The challenge page's own elements, and its title below. Not `_cf_chl_opt`: Cloudflare
    // leaves that on the site's pages once they are cleared, and not a Turnstile widget, which a
    // site can use in its own forms. The body text is only collected once the page has loaded,
    // so a long page is not serialised on every tick.
    static const QString probe = QStringLiteral(
        "(function(){var done=document.readyState==='complete';"
        "var cf=!!document.querySelector("
        "'#challenge-running,#challenge-stage,#cf-challenge-running,#challenge-form,#cf-chl-widget');"
        "return JSON.stringify({origin:location.origin,ready:document.readyState,"
        "title:document.title||'',cf:cf,"
        "text:(done&&document.body)?document.body.textContent.slice(0,4096):''});})()");

    const auto answer = [page, state, owner, expectedOwner, navigation, reloadAtMs](const QVariant &value) {
        state->probeSentMs = 0;
        if (state->finished) return;
        Session *entry = Host::instance().find(owner);
        // A newer navigation owns the page now, so this watch must not publish anything.
        if (!entry || entry->navigation != navigation) { finishSettle(state); return; }

        const QJsonObject probed = QJsonDocument::fromJson(value.toString().toUtf8()).object();
        if (probed.isEmpty()) {
            state->note(QStringLiteral("the page did not answer the probe script"));
            return;
        }
        g_engineRan.store(true);
        const QString origin = probed.value("origin").toString();
        const QString title  = probed.value("title").toString();
        const QString ready  = probed.value("ready").toString();
        const QString host   = QUrl(origin).host();
        const bool challenged = probed.value("cf").toBool() || looksLikeChallengePage(title);
        const bool blockPage  = looksLikeBlockPage(title)
                             || looksLikeBlockPage(probed.value("text").toString());
        state->note(QStringLiteral("origin %1, readyState %2, challenge %3, title \"%4\"")
                        .arg(origin.isEmpty() ? QStringLiteral("(none)") : origin,
                             ready, challenged ? QStringLiteral("yes") : QStringLiteral("no"),
                             title));

        // Before commit this still probes the previous document, whose readyState is
        // already "complete", so the origin has to match before the state is believed.
        const bool arrived = !host.isEmpty() && providerSession(host) == expectedOwner;
        if (arrived && ready == QLatin1String("complete") && !challenged && !blockPage) {
            entry->settledUrl = page->url();
            entry->lastUsedMs = QDateTime::currentMSecsSinceEpoch();
            state->ok = true;
            closeCheck(entry);
            finishSettle(state);
            return;
        }
        // Redirected out of the provider's site family: dodged, not cleared.
        if (!host.isEmpty() && !arrived) { finishSettle(state); return; }
        // A hard block never comes good by waiting.
        if (blockPage) {
            state->refused = true;
            finishSettle(state);
            return;
        }
        // A check that has not cleared on its own goes in front of the user.
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (arrived && challenged) {
            if (!state->challengedSinceMs) state->challengedSinceMs = now;
            if (!state->askedHuman && now - state->challengedSinceMs > kHumanAfterMs
                && askHuman(entry, host, state)) {
                state->askedHuman = true;
                state->deadlineMs = now + kHumanTimeoutMs;
            }
        }
        // One plain reload only: bypassing the cache restarts the challenge from scratch.
        // Never under the user's hand.
        if (!state->reloaded && !state->askedHuman && now > reloadAtMs) {
            state->reloaded = true;
            page->triggerAction(QWebEnginePage::Reload);
        }
    };

    auto *timer = new QTimer(page);
    state->watcher = timer;
    timer->setInterval(100);
    QObject::connect(timer, &QTimer::timeout, page, [page, state, answer]() {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now > state->deadlineMs.load()) { finishSettle(state); return; }
        // One probe out at a time; one a navigation swallowed is written off after a while.
        if (state->probeSentMs && now - state->probeSentMs < 2000) return;
        state->probeSentMs = now;
        // In a world of its own: the check watches for script run in the page's, and a probe there
        // has it reload itself, cleared, for ever.
        page->runJavaScript(probe, QWebEngineScript::ApplicationWorld, answer);
    });
    timer->start();
}

// A hidden Chromium page per provider: it meets the challenge like any browser, then fetches from
// inside the cleared page, so the request carries the clearance, the User-Agent and the TLS
// fingerprint it is bound to.
class Browser {
public:
    static bool available() {
        return QCoreApplication::instance() != nullptr && !g_engineBroken.load();
    }

    static PageFetch fetch(const QUrl &url, const CancelToken &requestCancel, int timeoutMs, bool bytes,
                           const QMap<QString, QString> &headers, const QString &owner) {
        const CancelToken cancel = requestCancel.composeWith(shutdownToken());
        QDeadlineTimer deadline(qMax(1, timeoutMs));
        const QUrl root = originRoot(url);
        Lane lane(owner, cancel, deadline);
        PageFetch page;
        g_lastWasRefusal = false;

        // The page already sits on this origin, so its clearance is there to be shared.
        bool refused = false;
        if (lane.acquire(Lane::Shared) && settledOn(owner, root, cancel, deadline)) {
            page = fetchOnce(url, cancel, deadline, bytes, headers, owner);
            if (!challenged(page, bytes)) return report(std::move(page), url, bytes, owner);
            refused = true;
        }
        lane.release();

        // Meet the challenge on the origin's root first: an API path answers a document
        // navigation with a bare 403 and never presents an interstitial. Only if the resource
        // is still refused is the rule on the path itself, and worth navigating to directly.
        QVarLengthArray<QUrl, 2> documents;
        documents.append(root);
        if (url != root) documents.append(url);

        if (!lane.acquire(Lane::Exclusive)) {
            if (page.error.isEmpty())
                page.error = QStringLiteral("The web engine request was cancelled while queued");
            return page;
        }
        for (const QUrl &document : documents) {
            if (refused) CookieStore::instance().invalidateClearance(url, owner);
            // A request that queued behind the one clearing this origin uses its page as it is.
            const bool ready = !refused && document == root && settledOn(owner, root, cancel, deadline);
            if (!ready && !settle(document, cancel, deadline, owner)) {
                // A firewall block, or a check the user closed or left: no other page differs.
                if (g_lastWasRefusal || g_lastHumanCheck != HumanCheck::None) break;
                refused = false;   // never arrived, so the clearance is not the suspect
                continue;
            }
            page = fetchOnce(url, cancel, deadline, bytes, headers, owner);
            if (!challenged(page, bytes)) break;
            refused = true;
        }
        return report(std::move(page), url, bytes, owner);
    }

    static QString solve(const QUrl &url, const CancelToken &requestCancel, int timeoutMs,
                         const QString &owner) {
        const CancelToken cancel = requestCancel.composeWith(shutdownToken());
        QDeadlineTimer deadline(qMax(1, timeoutMs));
        const QUrl root = originRoot(url);
        Lane lane(owner, cancel, deadline);
        g_lastWasRefusal = false;
        if (!lane.acquire(Lane::Exclusive)) return {};

        if (!settle(root, cancel, deadline, owner)
            && (url == root || !settle(url, cancel, deadline, owner)))
            return {};
        // The clearance is only usable by whoever presents the agent it was issued to.
        const QString agent = sessionUserAgent(owner, cancel, deadline);
        if (!agent.isEmpty()) rememberUserAgent(url.host(), agent, owner);
        return agent;
    }

private:
    static bool challenged(const PageFetch &page, bool bytes) {
        return isBlocked(page.code, page.headers,
                         bytes ? QString::fromUtf8(page.bytes.left(kBodyScanBytes)) : page.body);
    }

    static PageFetch report(PageFetch page, const QUrl &url, bool bytes, const QString &owner) {
        const bool blocked = challenged(page, bytes);
        const bool ok = page.code >= 200 && page.code < 300 && !blocked;
        if (ok) markBrowserBound(url.host(), owner);
        else if (page.error.isEmpty())
            page.error = g_lastHumanCheck == HumanCheck::Closed
                             ? QStringLiteral("%1 wants you to confirm you're human, and the check was "
                                              "closed before it cleared. Try again to get it back.").arg(url.host())
                         : g_lastHumanCheck == HumanCheck::TimedOut
                             ? QStringLiteral("%1 wants you to confirm you're human, and the check "
                                              "wasn't done in time. Try again to get it back.").arg(url.host())
                         : g_lastWasRefusal
                             ? QStringLiteral("%1 is blocking this network outright - its firewall "
                                              "answers with a block page rather than a challenge, "
                                              "so there is nothing to solve. A different "
                                              "connection is what this needs.").arg(url.host())
                         : blocked
                             ? QStringLiteral("%1 still refuses the web engine after renewing its "
                                              "clearance").arg(url.host())
                         // An answer, a 404 or a 429, speaks for itself.
                         : page.code == 0
                             ? QStringLiteral("The web engine could not fetch %1").arg(url.host())
                             : QString();
        return page;
    }

    static bool settledOn(const QString &owner, const QUrl &root, const CancelToken &cancel,
                   const QDeadlineTimer &deadline) {
        auto match = std::make_shared<std::atomic_bool>(false);
        const bool answered = runOnGui([owner, root, match]() {
            const Session *entry = Host::instance().find(owner);
            match->store(entry && !entry->settledUrl.isEmpty()
                         && originRoot(entry->settledUrl) == root);
        }, deadline, cancel);
        return answered && match->load();
    }

    static QString sessionUserAgent(const QString &owner, const CancelToken &cancel,
                             const QDeadlineTimer &deadline) {
        auto agent = std::make_shared<QString>();
        const bool answered = runOnGui([owner, agent]() {
            if (const Session *entry = Host::instance().find(owner)) *agent = entry->userAgent;
        }, deadline, cancel);
        return answered ? *agent : QString();
    }

    // Drives the page to `document` and reports whether it came to rest, cleared, on the
    // provider's own site.
    // A check the user had to pass renews `deadline`, so the request it was for still has time.
    static bool settle(const QUrl &document, const CancelToken &cancel, QDeadlineTimer &deadline,
                const QString &owner) {
        auto state = std::make_shared<SettleState>();
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const qint64 budget = deadline.remainingTime();
        state->deadlineMs = now + budget;
        const qint64 reloadAtMs = now + budget * kReloadAtPercent / 100;
        const QString expectedOwner = providerSession(document.host());
        g_lastHumanCheck = HumanCheck::None;

        auto listeners = std::make_shared<QList<QMetaObject::Connection>>();
        const bool posted = runOnGui([owner, document, state, expectedOwner, reloadAtMs, listeners]() {
            Session &entry = Host::instance().session(owner);
            QWebEnginePage *page = entry.page;
            const quint64 navigation = ++entry.navigation;
            entry.settledUrl.clear();

            // Before commit the page still holds the old document, so the probe has to run
            // from loadFinished rather than straight away.
            *listeners << QObject::connect(page, &QWebEnginePage::loadFinished, page,
                             [page, state, owner, expectedOwner, navigation, reloadAtMs](bool ok) {
                                 if (!ok) state->note(QStringLiteral("the navigation failed"));
                                 watchSettle(page, state, owner, expectedOwner, navigation, reloadAtMs);
                             }, Qt::SingleShotConnection);
            // Otherwise a dead render process is a silent wait for the whole deadline.
            *listeners << QObject::connect(page, &QWebEnginePage::renderProcessTerminated, page,
                             [state](QWebEnginePage::RenderProcessTerminationStatus status,
                                     int exitCode) {
                                 state->note(QStringLiteral("the render process stopped "
                                                            "(status %1, exit code %2)")
                                                 .arg(int(status)).arg(exitCode));
                                 if (!g_engineRan.load() && !g_engineBroken.exchange(true))
                                     logWarn() << "Cloudflare"
                                               << "the web engine cannot start here, so sites "
                                                  "behind Cloudflare will not load - status"
                                               << int(status) << "exit code" << exitCode;
                                 finishSettle(state);
                             }, Qt::SingleShotConnection);
            page->load(document);
        }, deadline, cancel);
        if (!posted) return false;

        while (!cancel.isCancelled() && QDateTime::currentMSecsSinceEpoch() <= state->deadlineMs.load())
            if (state->done.tryAcquire(1, 50)) break;
        g_lastWasRefusal = state->refused.load();
        // Whatever ended it - an answer, the deadline, a cancel, the window closed - the watch
        // and its listeners go too.
        runOnGui([listeners, state]() {
            for (const auto &listener : *listeners) QObject::disconnect(listener);
            finishSettle(state);
        }, QDeadlineTimer(3000), {});
        if (state->askedHuman) {
            g_lastHumanCheck = state->ok ? HumanCheck::Passed
                             : state->closedByUser ? HumanCheck::Closed : HumanCheck::TimedOut;
            // Timed out or cancelled with the window still up.
            runOnGui([owner]() { closeCheck(Host::instance().find(owner)); }, QDeadlineTimer(3000), {});
            if (state->ok) deadline = QDeadlineTimer(kSolveTimeoutMs);
        }
        if (state->ok) return true;
        if (g_lastHumanCheck == HumanCheck::Closed) {
            logWarn() << "Cloudflare" << document.host() << "check closed before it cleared";
            return false;
        }
        if (g_lastWasRefusal)
            logWarn() << "Cloudflare" << document.host()
                      << "served a block page to the web engine - that is a firewall rule on this "
                         "network, not a challenge, so retrying will not clear it -" << state->lastNote();
        else
            logWarn() << "Cloudflare" << owner << "did not settle on" << document.host()
                      << "-" << state->lastNote();
        return false;
    }

    // From inside the settled origin, so the request carries the page's cookies, its
    // User-Agent and its TLS fingerprint. Chromium drops the headers it owns, and Referer has
    // to travel as the request's referrer instead.
    static PageFetch fetchOnce(const QUrl &url, const CancelToken &cancel, const QDeadlineTimer &deadline,
                        bool bytes, const QMap<QString, QString> &headers, const QString &owner) {
        PageFetch page;
        static QAtomicInteger<quint64> lastId;
        const quint64 id = lastId.fetchAndAddRelaxed(1) + 1;

        QJsonObject requestHeaders;
        QString referrer;
        for (auto it = headers.constBegin(); it != headers.constEnd(); ++it) {
            const QString key = it.key().toLower();
            if (key == "referer" || key == "referrer") { referrer = it.value(); continue; }
            if (key == "cookie" || key == "user-agent" || key == "host" || key == "origin"
                || key == "content-length" || key == "connection" || key == "accept-encoding"
                || key.startsWith("sec-"))
                continue;
            requestHeaders.insert(it.key(), it.value());
        }
        QJsonObject options{{"credentials", "include"}, {"cache", "no-store"},
                            {"redirect", "follow"}, {"headers", requestHeaders}};
        // fetch() only honours a same-origin referrer and quietly falls back to the page's own
        // url otherwise - which is the site root, and what providers expect anyway.
        if (!referrer.isEmpty()) options.insert("referrer", referrer);

        const QString script =
            QStringLiteral("(function(){var s=window.__aoFetch||(window.__aoFetch={});var id='")
            + QString::number(id) + QStringLiteral("';s[id]=null;(async function(){var out;")
            + QStringLiteral("var c=new AbortController();var t=setTimeout(function(){c.abort();},")
            + QString::number(qMax<qint64>(1, deadline.remainingTime()))
            + QStringLiteral(");try{var o=") + jsLiteral(options)
            + QStringLiteral(";o.signal=c.signal;var r=await fetch(") + jsLiteral(url.toString())
            + QStringLiteral(",o);var b=new Uint8Array(await r.arrayBuffer());if(b.length>")
            + QString::number(kMaxBrowserBodyBytes)
            + QStringLiteral(")throw new Error('response exceeds the browser transfer limit');"
                             "var body;if(")
            + (bytes ? QStringLiteral("true") : QStringLiteral("false"))
            + QStringLiteral("){var p='';for(var i=0;i<b.length;i+=32768)"
                             "p+=String.fromCharCode.apply(null,b.subarray(i,i+32768));"
                             "body=btoa(p);}else body=new TextDecoder().decode(b);"
                             "out=JSON.stringify({code:r.status,url:r.url,"
                             "headers:Object.fromEntries(r.headers.entries()),body:body});}"
                             "catch(e){out=JSON.stringify({code:0,error:((e&&e.name)||'Error')"
                             "+': '+((e&&e.message)||'')});}"
                             "finally{clearTimeout(t);s[id]=out;}})();})();true");

        auto state = std::make_shared<FetchState>();
        const qint64 deadlineMs = QDateTime::currentMSecsSinceEpoch() + deadline.remainingTime();
        const bool posted = runOnGui([owner, script, state, id, deadlineMs]() {
            Session &entry = Host::instance().session(owner);
            entry.page->runJavaScript(script);
            pollFetched(entry.page, state, id, deadlineMs);
        }, deadline, cancel);
        if (!posted) {
            page.error = QStringLiteral("The web engine request was cancelled");
            return page;
        }

        while (!deadline.hasExpired() && !cancel.isCancelled())
            if (state->done.tryAcquire(1, 50)) break;
        QString payload;
        { QMutexLocker lock(&state->mutex); payload = state->payload; }
        if (payload.isEmpty()) return page;

        const QJsonObject json = QJsonDocument::fromJson(payload.toUtf8()).object();
        page.code = json.value("code").toInt();
        page.error = json.value("error").toString();
        page.finalUrl = QUrl(json.value("url").toString());
        const QJsonObject responseHeaders = json.value("headers").toObject();
        for (auto it = responseHeaders.constBegin(); it != responseHeaders.constEnd(); ++it)
            page.headers[it.key()] = it.value().toString();
        page.contentType = page.headers.value(QStringLiteral("content-type"));
        if (bytes) page.bytes = QByteArray::fromBase64(json.value("body").toString().toLatin1());
        else       page.body = json.value("body").toString();
        return page;
    }
};

}

PageFetch Cloudflare::fetchInBrowser(const QUrl &url, const CancelToken &cancel, int timeoutMs, bool bytes,
                                     const QMap<QString, QString> &headers, const QString &sessionOwner) {
    const QString owner = sessionOwner.isEmpty() ? providerSession(url.host()) : sessionOwner;
    if (!Browser::available()) {
        PageFetch page;
        page.error = QStringLiteral("The web engine is not available");
        return page;
    }
    return Browser::fetch(url, cancel, timeoutMs, bytes, headers, owner);
}

QString Cloudflare::solveChallenge(const QUrl &url, const CancelToken &cancel, int timeoutMs,
                                   const QString &sessionOwner) {
    if (cancel.isCancelled() || shutdownToken().isCancelled() || !Browser::available()) return {};
    const QString owner = sessionOwner.isEmpty() ? providerSession(url.host()) : sessionOwner;
    return Browser::solve(url, cancel, timeoutMs, owner);
}

void Cloudflare::shutdown() {
    shutdownToken().cancel();
    // Render processes go before the GUI thread.
    runOnGui([]() { Host::instance().closeAll(); }, QDeadlineTimer(3000), {});
    CookieStore::instance().flush();
}

QString Cloudflare::captureInBrowser(const QUrl &url, const QString &hook, const QString &probe,
                                     const CancelToken &requestCancel, int timeoutMs,
                                     const QString &owner) {
    if (!Browser::available()) return {};
    const CancelToken cancel = requestCancel.composeWith(shutdownToken());
    const QDeadlineTimer deadline(qMax(1, timeoutMs));
    Lane lane(owner, cancel, deadline);
    if (!lane.acquire(Lane::Exclusive)) return {};

    // Until the new document commits, the probe runs against the old one, which may be an
    // earlier capture with answers of its own. The marker tells them apart.
    static QAtomicInteger<quint64> lastRun;
    const QString run = QString::number(lastRun.fetchAndAddRelaxed(1) + 1);
    const QString marked = QStringLiteral("window.__aoCapture=") + run + QLatin1Char(';') + hook;
    const QString guarded = QStringLiteral("(window.__aoCapture===") + run
                          + QStringLiteral("?(") + probe + QStringLiteral("):'')");

    auto state = std::make_shared<FetchState>();
    const qint64 deadlineMs = QDateTime::currentMSecsSinceEpoch() + deadline.remainingTime();
    const bool posted = runOnGui([owner, url, marked, guarded, state, deadlineMs]() {
        Session &entry = Host::instance().session(owner);
        ++entry.navigation;
        entry.settledUrl.clear();
        QWebEngineScriptCollection &scripts = entry.page->scripts();
        for (const QWebEngineScript &old : scripts.find(QStringLiteral("aowave-capture")))
            scripts.remove(old);
        QWebEngineScript script;
        script.setName(QStringLiteral("aowave-capture"));
        script.setSourceCode(marked);
        script.setInjectionPoint(QWebEngineScript::DocumentCreation);
        script.setWorldId(QWebEngineScript::MainWorld);
        script.setRunsOnSubFrames(false);
        scripts.insert(script);
        entry.page->load(url);
        pollFor(entry.page, guarded, state, deadlineMs);
    }, deadline, cancel);
    if (!posted) return {};

    while (!deadline.hasExpired() && !cancel.isCancelled())
        if (state->done.tryAcquire(1, 50)) break;
    QMutexLocker lock(&state->mutex);
    return state->payload;
}
