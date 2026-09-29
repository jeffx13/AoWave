#include "shows/providerlist.h"
#include "shows/showprovider.h"
#include "core/settings.h"
#include "core/exception.h"
#include "core/logger.h"
#include "net/client.h"
#include "net/providerhealth.h"
#include <QElapsedTimer>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrentRun>
#include <QLocale>

void ProviderList::setProviders(QList<ShowProvider *> &&providers) {
    m_allProviders = std::move(providers);
    s_byName.clear();
    for (ShowProvider *provider : std::as_const(m_allProviders)) {
        provider->setParent(this);
        s_byName.insert(provider->name(), provider);
    }
    // The saved order first; a provider it has never seen keeps its place after them.
    const QStringList saved = Settings::instance().value(QStringLiteral("providers/order")).toStringList();
    std::stable_sort(m_allProviders.begin(), m_allProviders.end(), [&saved](ShowProvider *a, ShowProvider *b) {
        const auto rank = [&saved](ShowProvider *p) {
            const qsizetype at = saved.indexOf(p->name());
            return at < 0 ? saved.size() : at;
        };
        return rank(a) < rank(b);
    });
    rebuildEnabledProviders();
}

void ProviderList::moveProvider(int from, int to) {
    if (from == to || from < 0 || to < 0 || from >= m_allProviders.size() || to >= m_allProviders.size()) return;
    m_allProviders.move(from, to);
    QStringList order;
    for (ShowProvider *provider : std::as_const(m_allProviders)) order << provider->name();
    Settings::instance().setValue(QStringLiteral("providers/order"), order);
    rebuildEnabledProviders();
}

ShowProvider *ProviderList::forUrl(const QUrl &url, QString &showLink, int &episodeIndex) {
    for (ShowProvider *provider : std::as_const(s_byName))
        if (provider->parseUrl(url, showLink, episodeIndex)) return provider;
    return nullptr;
}

// Host ownership only: a media url on a provider's CDN is its own even if unparseable.
ShowProvider *ProviderList::ownerOfHost(const QUrl &url) {
    const QString host = url.host().toLower();
    if (host.isEmpty()) return nullptr;

    ShowProvider *best = nullptr;
    int bestLength = 0;
    for (ShowProvider *provider : std::as_const(s_byName)) {
        const QString providerHost = QUrl(provider->hostUrl()).host().toLower();
        if (providerHost.isEmpty()) continue;
        // Registrable-ish tails, not whole hosts: providers serve from sibling domains.
        const QString tail = registrableTail(providerHost);
        if (tail.isEmpty()) continue;
        if (host != tail && !host.endsWith(QLatin1Char('.') + tail)) continue;
        if (tail.size() > bestLength) { best = provider; bestLength = tail.size(); }
    }
    return best;
}

// Two labels is enough: it only compares against a host the app configured.
QString ProviderList::registrableTail(const QString &host) {
    const QStringList labels = host.split(QLatin1Char('.'), Qt::SkipEmptyParts);
    if (labels.size() < 2) return host;
    return labels.mid(labels.size() - 2).join(QLatin1Char('.'));
}

void ProviderList::setCurrentIndex(int index) {
    if (index == m_index || index < 0 || index >= m_providers.size()) return;

    // Keep the same show type across providers that offer it.
    const QString previousType = (m_typeIndex >= 0 && m_typeIndex < m_types.size())
                                     ? m_types.at(m_typeIndex) : QString();

    m_index = index;
    m_current = m_providers.at(index);
    m_types = m_current->availableTypes();
    emit currentIndexChanged();

    const int carried = previousType.isEmpty() ? -1 : m_types.indexOf(previousType);
    m_typeIndex = qMax(carried, 0);
    emit currentTypeIndexChanged();
}

void ProviderList::setCurrentTypeIndex(int index) {
    if (index == m_typeIndex || index < 0 || index >= m_types.size()) return;
    m_typeIndex = index;
    emit currentTypeIndexChanged();
}

bool ProviderList::filtersSearch() const {
    return m_current && m_current->filtersSearch();
}

QString ProviderList::filterKey() const {
    return m_current ? m_current->name() + QLatin1Char('/') + QString::number(m_typeIndex) : QString();
}

void ProviderList::refreshFilterOptions() {
    const QString key = filterKey();
    m_filterOptions = m_filterCache.value(key);
    m_filterOptionsReady = key.isEmpty() || m_filterCache.contains(key);
    emit filterOptionsChanged();
    if (key.isEmpty() || m_filterCache.contains(key)) return;

    m_testRuns.add(QtConcurrent::run([this, provider = m_current, type = m_typeIndex, key, cancel = m_testCancel]() {
        Client client(cancel, false);
        QVariantMap options;
        try {
            options = provider->filterOptions(&client, type);
        } catch (const std::exception &e) {
            logWarn() << "Providers" << provider->name() << "filter options:" << e.what();
        }
        if (cancel.isCancelled()) return;
        QMetaObject::invokeMethod(this, [this, key, options]() {
            if (!options.isEmpty()) m_filterCache.insert(key, options);
            if (key != filterKey()) return;
            m_filterOptions = options;
            m_filterOptionsReady = true;
            emit filterOptionsChanged();
        }, Qt::QueuedConnection);
    }));
}

int ProviderList::indexOf(const QString &name) const {
    for (int i = 0; i < m_providers.size(); ++i)
        if (m_providers.at(i)->name() == name) return i;
    return -1;
}

void ProviderList::cycle() {
    if (m_providers.isEmpty()) return;
    setCurrentIndex((m_index + 1) % m_providers.size());
}

int ProviderList::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : m_providers.size();
}

QVariant ProviderList::data(const QModelIndex &index, int role) const {
    if (index.row() < 0 || index.row() >= m_providers.size())
        return {};
    if (role == NameRole) return m_providers.at(index.row())->name();
    if (role == LanguageRole) return m_providers.at(index.row())->language();
    return {};
}

QHash<int, QByteArray> ProviderList::roleNames() const {
    return {{NameRole, "text"}, {LanguageRole, "language"}};
}

bool ProviderList::providerEnabled(const QString &name) const {
    return Settings::instance().value(QStringLiteral("providers/enabled/") + name, true).toBool();
}

void ProviderList::rebuildEnabledProviders() {
    ShowProvider *previous = m_current;
    beginResetModel();
    m_providers.clear();
    for (ShowProvider *provider : std::as_const(m_allProviders))
        if (providerEnabled(provider->name())) m_providers.append(provider);
    m_index = -1;
    m_current = nullptr;
    m_types.clear();
    m_typeIndex = 0;
    endResetModel();
    if (!m_providers.isEmpty()) setCurrentIndex(qMax(0, int(m_providers.indexOf(previous))));
    else { emit currentIndexChanged(); emit currentTypeIndexChanged(); }
    emit selectionChanged();
}

void ProviderList::setProviderEnabled(const QString &name, bool enabled) {
    if (!s_byName.contains(name) || providerEnabled(name) == enabled) return;
    Settings::instance().setValue(QStringLiteral("providers/enabled/") + name, enabled);
    rebuildEnabledProviders();
}

void ProviderList::setLanguageEnabled(const QString &language, bool enabled) {
    for (ShowProvider *provider : std::as_const(m_allProviders))
        if (provider->language() == language)
            Settings::instance().setValue(QStringLiteral("providers/enabled/") + provider->name(), enabled);
    rebuildEnabledProviders();
}

void ProviderList::setAllEnabled(bool enabled) {
    for (ShowProvider *provider : std::as_const(m_allProviders))
        Settings::instance().setValue(QStringLiteral("providers/enabled/") + provider->name(), enabled);
    rebuildEnabledProviders();
}

int ProviderList::allCheckState() const {
    return m_providers.isEmpty() ? Qt::Unchecked
        : m_providers.size() == m_allProviders.size() ? Qt::Checked : Qt::PartiallyChecked;
}

QVariantMap ProviderList::health(const QString &name) const {
    ShowProvider *provider = byName(name);
    return provider ? ProviderHealth::statsFor(provider->name()) : QVariantMap{};
}

// A real search, not a HEAD: providers fail by changing markup more often than by going down.
void ProviderList::testProvider(const QString &name) {
    ShowProvider *provider = byName(name);
    if (!provider) return;
    // Old failures must not colour a retest.
    ProviderHealth::reset(provider->name());
    emit healthChanged();

    m_testRuns.add(QtConcurrent::run([this, provider, name, cancel = m_testCancel]() {
        Client client(cancel, false);
        QElapsedTimer clock;
        clock.start();
        int count = 0;
        QString error;
        try {
            count = int(provider->search(&client, QStringLiteral("a"), 1, 0).size());
        } catch (const std::exception &e) {
            error = QString::fromUtf8(e.what());
        }
        const int elapsed = int(clock.elapsed());
        if (cancel.isCancelled()) return;
        QMetaObject::invokeMethod(this, [this, name, count, elapsed, error]() {
            emit providerTested(name, error.isEmpty() && count > 0, count, elapsed, error);
            emit healthChanged();
        }, Qt::QueuedConnection);
    }));
}

QString ProviderList::languageLabel(const QString &language) {
    // QLocale names carry a territory ("American English").
    static const QHash<QString, QString> labels{
        {"en", QStringLiteral("English")}, {"zh_CN", QStringLiteral("简体中文")}, {"zh_TW", QStringLiteral("繁體中文")},
        {"ja", QStringLiteral("日本語")}, {"ko", QStringLiteral("한국어")},
    };
    return labels.value(language, QLocale(language).nativeLanguageName());
}

QVariantList ProviderList::ordered() const {
    QVariantList result;
    for (ShowProvider *provider : m_allProviders)
        result.append(QVariantMap{{"name", provider->name()}, {"language", languageLabel(provider->language())},
                                  {"enabled", providerEnabled(provider->name())}});
    return result;
}

QVariantList ProviderList::languageGroups() const {
    QMap<QString, QVariantList> groups;
    for (ShowProvider *provider : m_allProviders)
        groups[provider->language()].append(QVariantMap{{"name", provider->name()},
                                                       {"enabled", providerEnabled(provider->name())}});
    QVariantList result;
    for (auto it = groups.cbegin(); it != groups.cend(); ++it) {
        int enabled = 0;
        for (const auto &entry : it.value()) enabled += entry.toMap().value("enabled").toBool();
        const QString label = languageLabel(it.key());
        result.append(QVariantMap{{"language", it.key()}, {"label", label}, {"providers", it.value()},
            {"checkState", enabled == 0 ? Qt::Unchecked : enabled == it.value().size() ? Qt::Checked : Qt::PartiallyChecked}});
    }
    return result;
}
