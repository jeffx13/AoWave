#pragma once
#include <QAbstractListModel>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>
#include "core/async.h"
#include "net/canceltoken.h"
#include <qqmlintegration.h>

class ShowProvider;

class ProviderList : public QAbstractListModel
{
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(int      currentIndex     READ currentIndex     WRITE setCurrentIndex     NOTIFY currentIndexChanged)
    Q_PROPERTY(int      currentTypeIndex READ currentTypeIndex WRITE setCurrentTypeIndex NOTIFY currentTypeIndexChanged)
    Q_PROPERTY(QVariant showTypes        READ showTypes                                  NOTIFY currentIndexChanged)
    Q_PROPERTY(QVariantList languageGroups READ languageGroups NOTIFY selectionChanged)
    // Every provider in the user's order, enabled or not, for the Providers card.
    Q_PROPERTY(QVariantList ordered READ ordered NOTIFY selectionChanged)
    Q_PROPERTY(int allCheckState READ allCheckState NOTIFY selectionChanged)
    // What the current provider and type can be browsed by; see ShowProvider::filterOptions.
    Q_PROPERTY(QVariantMap filterOptions READ filterOptions NOTIFY filterOptionsChanged)
    // False while the current provider's options are still being fetched.
    Q_PROPERTY(bool filterOptionsReady READ filterOptionsReady NOTIFY filterOptionsChanged)
    Q_PROPERTY(bool filtersSearch READ filtersSearch NOTIFY currentIndexChanged)
public:
    explicit ProviderList(QObject *parent = nullptr) : QAbstractListModel(parent) {
        connect(this, &ProviderList::currentTypeIndexChanged, this, &ProviderList::refreshFilterOptions);
    }
    // Waits for provider tests: the providers are children, deleted right after.
    ~ProviderList() override {
        m_testCancel.cancel();
        m_testRuns.waitAll("Provider test");
    }

    // Takes ownership.
    void setProviders(QList<ShowProvider *> &&providers);

    Q_INVOKABLE void cycle();
    // Among the enabled providers, which the combo box lists; -1 when not there.
    Q_INVOKABLE int indexOf(const QString &name) const;
    // Enabled or not.
    Q_INVOKABLE bool has(const QString &name) const { return byName(name) != nullptr; }
    Q_INVOKABLE void setProviderEnabled(const QString &name, bool enabled);

    // For the Providers card.
    Q_INVOKABLE QVariantMap health(const QString &name) const;
    Q_INVOKABLE void testProvider(const QString &name);
    Q_SIGNAL void providerTested(QString name, bool ok, int results, int elapsedMs, QString error);
    Q_SIGNAL void healthChanged();
    Q_INVOKABLE void setLanguageEnabled(const QString &language, bool enabled);
    // The order the combo box, Migrate and cycling go by; saved.
    Q_INVOKABLE void moveProvider(int from, int to);
    QVariantList ordered() const;
    Q_INVOKABLE void setAllEnabled(bool enabled);
    QVariantList languageGroups() const;
    int allCheckState() const;

    ShowProvider *currentProvider() const { return m_current; }
    int currentTypeIndex() const { return m_typeIndex; }
    QVariantMap filterOptions() const { return m_filterOptions; }
    bool filterOptionsReady() const { return m_filterOptionsReady; }
    bool filtersSearch() const;

    static ShowProvider *byName(const QString &providerName) {
        return s_byName.value(providerName, nullptr);
    }
    // The provider that owns a pasted url.
    static ShowProvider *forUrl(const QUrl &url, QString &showLink, int &episodeIndex);
    // The provider whose site a url sits on, parseable or not.
    static ShowProvider *ownerOfHost(const QUrl &url);

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
    // Fetched once per provider and type; a failed fetch is tried again on the next visit.
    void refreshFilterOptions();
    QString filterKey() const;

    enum { NameRole = Qt::UserRole, LanguageRole };
    void rebuildEnabledProviders();
    bool providerEnabled(const QString &name) const;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    QList<ShowProvider *> m_providers;
    QList<ShowProvider *> m_allProviders;
    inline static QHash<QString, ShowProvider *> s_byName;
    ShowProvider *m_current = nullptr;
    CancelToken m_testCancel;
    RunSet m_testRuns;
    int m_index = -1;
    int m_typeIndex = 0;
    QStringList m_types;
    QVariantMap m_filterOptions;
    bool m_filterOptionsReady = false;
    QHash<QString, QVariantMap> m_filterCache;
};
