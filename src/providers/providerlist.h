#pragma once
#include <QAbstractListModel>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>
#include "app/async.h"
#include <qqmlintegration.h>

class Provider;

class ProviderList : public QAbstractListModel
{
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(int      currentIndex     READ currentIndex     WRITE setCurrentIndex     NOTIFY currentIndexChanged)
    Q_PROPERTY(int      currentTypeIndex READ currentTypeIndex WRITE setCurrentTypeIndex NOTIFY currentTypeIndexChanged)
    Q_PROPERTY(QVariant showTypes        READ showTypes                                  NOTIFY currentIndexChanged)
    Q_PROPERTY(QVariantList languageGroups READ languageGroups NOTIFY selectionChanged)
    Q_PROPERTY(QVariantList ordered READ ordered NOTIFY selectionChanged)
    Q_PROPERTY(int allCheckState READ allCheckState NOTIFY selectionChanged)
    Q_PROPERTY(QVariantMap filterOptions READ filterOptions NOTIFY filterOptionsChanged)
    Q_PROPERTY(bool filterOptionsReady READ filterOptionsReady NOTIFY filterOptionsChanged)
    Q_PROPERTY(bool filtersSearch READ filtersSearch NOTIFY currentIndexChanged)
    Q_PROPERTY(QVariantList accounts READ accounts CONSTANT)
public:
    explicit ProviderList(QObject *parent = nullptr) : QAbstractListModel(parent) {
        connect(this, &ProviderList::currentTypeIndexChanged, this, &ProviderList::refreshFilterOptions);
    }
    // Waits for provider tests: the providers are children, deleted right after.
    ~ProviderList() override {
        m_testCancel.cancel();
        m_testRuns.waitAll("Provider test");
    }

    void setProviders(QList<Provider *> &&providers);

    Q_INVOKABLE void cycle();
    Q_INVOKABLE int indexOf(const QString &name) const;
    Q_INVOKABLE bool has(const QString &name) const { return byName(name) != nullptr; }
    Q_INVOKABLE void setProviderEnabled(const QString &name, bool enabled);

    Q_INVOKABLE QVariantMap health(const QString &name) const;
    Q_INVOKABLE void testProvider(const QString &name);
    Q_SIGNAL void providerTested(QString name, bool ok, int results, int elapsedMs, QString error);
    // Once a week at most, in the background: a test search on each enabled provider the Cloudflare check does not front.
    // False when nothing started (not due, already running, or a quiet run); sitesChecked names those that did not answer.
    bool checkSites(bool quiet);
    Q_SIGNAL void sitesChecked(QStringList failed);
    Q_SIGNAL void healthChanged();
    Q_INVOKABLE void setLanguageEnabled(const QString &language, bool enabled);
    Q_INVOKABLE void moveProvider(int from, int to);
    QVariantList ordered() const;
    Q_INVOKABLE void setAllEnabled(bool enabled);
    QVariantList languageGroups() const;
    int allCheckState() const;

    QVariantList accounts() const;
    Provider *currentProvider() const { return m_current; }
    int currentTypeIndex() const { return m_typeIndex; }
    QVariantMap filterOptions() const { return m_filterOptions; }
    bool filterOptionsReady() const { return m_filterOptionsReady; }
    bool filtersSearch() const;

    static Provider *byName(const QString &providerName) {
        return s_byName.value(providerName, nullptr);
    }
    static Provider *forUrl(const QUrl &url, QString &showLink, int &episodeIndex);
    static Provider *ownerOfHost(const QUrl &url);

signals:
    void currentIndexChanged();
    void currentTypeIndexChanged();
    void selectionChanged();
    void filterOptionsChanged();

private:
    int  currentIndex() const { return m_index; }
    void setCurrentIndex(int index);
    void setCurrentTypeIndex(int index);
    QVariant showTypes() const {
        return QVariant::fromValue(m_types.isEmpty() ? QStringList{"All"} : m_types);
    }

    static QString registrableTail(const QString &host);
    static QString languageLabel(const QString &language);
    void refreshFilterOptions();
    QString filterKey() const;

    enum { NameRole = Qt::UserRole, LanguageRole };
    void rebuildEnabledProviders();
    bool providerEnabled(const QString &name) const;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    QList<Provider *> m_providers;
    QList<Provider *> m_allProviders;
    inline static QHash<QString, Provider *> s_byName;
    Provider *m_current = nullptr;
    CancelToken m_testCancel;
    RunSet m_testRuns;
    bool m_checking = false;
    int m_index = -1;
    int m_typeIndex = 0;
    QStringList m_types;
    QVariantMap m_filterOptions;
    bool m_filterOptionsReady = false;
    QHash<QString, QVariantMap> m_filterCache;
};
