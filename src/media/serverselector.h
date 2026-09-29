#pragma once
#include <QHash>
#include <QList>
#include <QThreadPool>
#include "net/client.h"
#include "shows/playinfo.h"

class ShowProvider;

class ServerSelector {
public:
    struct Result {
        int index = -1;
        PlayInfo playInfo;
        QHash<QString, PlayInfo> cachedSources;
        // The provider's own refusal, so the caller says why instead of "nothing worked".
        QString failure;
        QString failureHeader;
        bool found() const { return index >= 0; }
    };

    // Unknown is not a verdict: the probe never reached one.
    enum class Playability { Playable, Broken, Unknown };

    // Ordered by urgency, so a click is never queued behind the prefetch.
    static QThreadPool &probePool();
    enum Priority { Background = 0, Foreground = 10 };

    static Playability playability(Client *client, PlayInfo &playItem);
    static Result findWorkingServer(Client *client, ShowProvider *provider, QList<VideoServer> &servers,
                                    Priority priority = Foreground);
};