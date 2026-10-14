#pragma once
#include <QHash>
#include <QObject>
#include <QStringList>
#include <QVariantMap>
#include <qqmlintegration.h>

class Shortcuts : public QObject {
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(QVariantMap bindings READ bindings NOTIFY changed)
    Q_PROPERTY(QVariantList actions READ actions NOTIFY changed)

public:
    explicit Shortcuts(QObject *parent = nullptr);
    ~Shortcuts() override;

    Q_INVOKABLE static QString keyText(int key, int modifiers);
    Q_INVOKABLE QString actionFor(const QString &keyText) const;
    Q_INVOKABLE void bind(const QString &action, const QString &keyText);
    Q_INVOKABLE void addKey(const QString &action, const QString &keyText);
    Q_INVOKABLE void reset(const QString &action);
    Q_INVOKABLE void resetAll();
    Q_INVOKABLE void restore(const QVariantMap &bindings);
    static QString firstKey(const QString &action);

    QVariantMap bindings() const;
    QVariantList actions() const;

signals:
    void changed();

private:
    struct Action {
        const char *id;
        const char *group;
        QStringList defaults;
    };
    void save(const QString &action);

    QList<Action> m_actions;
    QHash<QString, QStringList> m_keys;
};
