#pragma once
#include "providers/showdata.h"
#include "net/client.h"
#include <QAbstractListModel>
#include <QHash>
#include <QSet>
#include <qqmlintegration.h>
#include <QList>
#include <QThreadPool>

class Provider;

class ServerListModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by Playlist; QML uses it for Status.")
    Q_PROPERTY(int currentIndex READ currentIndex WRITE setCurrentIndex NOTIFY currentIndexChanged)
    Q_PROPERTY(int count        READ count                                NOTIFY countChanged)
public:
    enum Status { Unchecked, Working, Broken };
    Q_ENUM(Status)

    void setServers(const QList<VideoServer> &servers, Provider *provider);
    void setCurrentIndex(int index);
    void setCurrentServer(const QString &name);
    void setPreferredServer(int index);
    void clear();

    VideoServer& at(int index);
    int count() const { return m_servers.count(); }
    int currentIndex() const { return m_currentIndex; }
    bool isValidIndex(int index) const;

    Provider *provider() const { return m_provider; }
    const QList<VideoServer> &servers() const { return m_servers; }
    QString currentName() const { return isValidIndex(m_currentIndex) ? m_servers[m_currentIndex].name : QString(); }

    struct Cached {
        Source info;
        qint64 expiresAtMs = 0;
        bool fresh() const;
    };
    using SourceCache = QHash<QString, Cached>;
    static qint64 expiryOf(const Source &info);
    static SourceCache stamped(QHash<QString, Source> &&sources);

    void setCachedSources(SourceCache cache);
    void cacheSource(const QString &name, Source info);
    void dropSource(const QString &name);
    void markBroken(const QString &name);
    const Source *cachedSource(const QString &name) const {
        const auto it = m_sourceCache.constFind(name);
        return it != m_sourceCache.constEnd() && it->fresh() ? &it->info : nullptr;
    }
    const SourceCache &sources() const { return m_sourceCache; }

signals:
    void currentIndexChanged();
    void countChanged();

private:
    int m_currentIndex = -1;
    QList<VideoServer> m_servers;
    Provider *m_provider = nullptr;
    bool m_hasDub = false;
    SourceCache m_sourceCache;
    QSet<QString> m_brokenServers;

    int  indexOfServer(const QString &name) const;
    void emitStatusChanged(const QString &name);
    void resort();

    enum { NameRole = Qt::UserRole, LinkRole, StatusRole, TranslationRole, SectionRole };
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
};

class Provider;

class ServerSelector {
public:
    struct Result {
        int index = -1;
        Source source;
        QHash<QString, Source> cachedSources;
        QString failure;
        QString failureHeader;
        bool found() const { return index >= 0; }
    };

    enum class Playability { Playable, Broken, Unknown };

    static QThreadPool &probePool();
    enum Priority { Background = 0, Foreground = 10 };

    static Playability playability(Client *client, Source &playItem);
    static void forget(const Source &playItem);
    static Result findWorkingServer(Client *client, Provider *provider, QList<VideoServer> &servers,
                                    Priority priority = Foreground);
};
