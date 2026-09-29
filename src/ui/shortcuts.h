#pragma once
#include <QHash>
#include <QObject>
#include <QStringList>
#include <QVariantMap>
#include <qqmlintegration.h>

// Keyboard shortcuts the user can rebind. Each action has default keys; a key the user gives it
// replaces them, is taken from whichever action had it, and is saved. Keys are QKeySequence's
// portable text ("Ctrl+Shift+S", "PgUp", "*").
class Shortcuts : public QObject {
    Q_OBJECT
    QML_ANONYMOUS
    // action -> its keys, for Shortcut.sequences and the settings list
    Q_PROPERTY(QVariantMap bindings READ bindings NOTIFY changed)
    // The actions in display order: {id, group, custom}. Names live in QML, for qsTr.
    Q_PROPERTY(QVariantList actions READ actions NOTIFY changed)

public:
    explicit Shortcuts(QObject *parent = nullptr);

    // A key event as text. Shift is dropped from a symbol that needs it to be typed: "*".
    Q_INVOKABLE static QString keyText(int key, int modifiers);
    // Empty when no action has the key.
    Q_INVOKABLE QString actionFor(const QString &keyText) const;
    Q_INVOKABLE void bind(const QString &action, const QString &keyText);
    Q_INVOKABLE void reset(const QString &action);
    Q_INVOKABLE void resetAll();

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
