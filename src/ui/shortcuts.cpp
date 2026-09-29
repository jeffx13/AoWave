#include "ui/shortcuts.h"

#include <QKeySequence>
#include "core/settings.h"

namespace {

const QString kGroup = QStringLiteral("shortcuts/");

QString normalized(const QString &text) {
    return QKeySequence::fromString(text, QKeySequence::PortableText).toString(QKeySequence::PortableText);
}

}

Shortcuts::Shortcuts(QObject *parent) : QObject(parent) {
    m_actions = {
        {"playPause", "player", {"Space", "Clear"}},
        {"seekBack", "player", {"Left", "Z"}},
        {"seekForward", "player", {"Right", "X"}},
        {"skipBack", "player", {"End", "Ctrl+Z"}},
        {"skipForward", "player", {"PgDown", "Ctrl+X"}},
        {"previousEpisode", "player", {"Home", "Ctrl+S"}},
        {"nextEpisode", "player", {"PgUp", "Ctrl+D"}},
        {"previousPlaylist", "player", {"Ctrl+Shift+S"}},
        {"nextPlaylist", "player", {"Ctrl+Shift+D"}},
        {"volumeUp", "player", {"Up", "Q"}},
        {"volumeDown", "player", {"Down", "A"}},
        {"mute", "player", {"M"}},
        {"faster", "player", {"+", "D"}},
        {"slower", "player", {"-", "S"}},
        {"doubleSpeed", "player", {"R"}},
        {"fullscreen", "player", {"F"}},
        {"pip", "player", {"Ctrl+A"}},
        {"leave", "player", {"Esc"}},
        {"subtitles", "player", {"C"}},
        {"abLoop", "player", {"L"}},
        {"panel", "player", {"G"}},
        {"playlist", "player", {"P", "W"}},
        {"title", "player", {"Tab", "*"}},
        {"peek", "player", {"/"}},
        {"screenshot", "player", {"F12"}},
        {"copyFrame", "player", {"Ctrl+F12"}},
        {"copyLink", "player", {"Ctrl+C"}},
        {"openFile", "player", {"E"}},
        {"openFolder", "player", {"Ctrl+E"}},
        {"openDownloads", "player", {"Ctrl+Shift+E"}},
        {"openClipboard", "player", {"Ctrl+V"}},
        {"reload", "app", {"Ctrl+R"}},
        {"quickSearch", "app", {"Ctrl+K"}},
        {"back", "app", {"Alt+Left"}},
        {"forward", "app", {"Alt+Right"}},
        {"nextPage", "app", {"Ctrl+Tab"}},
        {"previousPage", "app", {"Ctrl+Shift+Tab"}},
        {"pageSearch", "app", {"1"}},
        {"pageInfo", "app", {"2"}},
        {"pageLibrary", "app", {"3"}},
        {"pagePlayer", "app", {"4"}},
        {"pageDownloads", "app", {"5"}},
        {"pageLogs", "app", {"6"}},
        {"bossKey", "app", {"Ctrl+Q"}},
        {"bossScreen", "app", {"Ctrl+Space"}},
        {"close", "app", {"Ctrl+W"}},
    };
    for (Action &action : m_actions) {
        for (QString &key : action.defaults) key = normalized(key);
        const QVariant saved = Settings::instance().value(kGroup + QLatin1String(action.id));
        m_keys.insert(QLatin1String(action.id), saved.isValid() ? saved.toStringList() : action.defaults);
    }
}

QString Shortcuts::keyText(int key, int modifiers) {
    Qt::KeyboardModifiers held = Qt::KeyboardModifiers(modifiers)
                                 & (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier | Qt::MetaModifier);
    if (key > 0x20 && key < 0x7f && !QChar(key).isLetterOrNumber()) held &= ~Qt::ShiftModifier;
    return QKeySequence(QKeyCombination(held, Qt::Key(key))).toString(QKeySequence::PortableText);
}

QString Shortcuts::actionFor(const QString &keyText) const {
    for (auto it = m_keys.cbegin(); it != m_keys.cend(); ++it)
        if (it.value().contains(keyText)) return it.key();
    return {};
}

void Shortcuts::bind(const QString &action, const QString &keyText) {
    if (!m_keys.contains(action) || keyText.isEmpty()) return;
    const QString previous = actionFor(keyText);
    if (previous == action && m_keys[action] == QStringList{keyText}) return;
    if (!previous.isEmpty() && previous != action) {
        m_keys[previous].removeAll(keyText);
        save(previous);
    }
    m_keys[action] = {keyText};
    save(action);
    emit changed();
}

void Shortcuts::reset(const QString &action) {
    for (const Action &entry : std::as_const(m_actions)) {
        if (action != QLatin1String(entry.id)) continue;
        // The defaults come back only where no other action has taken them since.
        QStringList keys;
        for (const QString &key : entry.defaults)
            if (const QString owner = actionFor(key); owner.isEmpty() || owner == action) keys << key;
        m_keys[action] = keys;
        Settings::instance().remove(kGroup + action);
        emit changed();
        return;
    }
}

void Shortcuts::resetAll() {
    for (const Action &action : std::as_const(m_actions)) {
        m_keys[QLatin1String(action.id)] = action.defaults;
        Settings::instance().remove(kGroup + QLatin1String(action.id));
    }
    emit changed();
}

QVariantMap Shortcuts::bindings() const {
    QVariantMap map;
    for (auto it = m_keys.cbegin(); it != m_keys.cend(); ++it) map.insert(it.key(), it.value());
    return map;
}

QVariantList Shortcuts::actions() const {
    QVariantList list;
    for (const Action &action : m_actions) {
        const QString id = QLatin1String(action.id);
        list.append(QVariantMap{{"id", id}, {"group", QLatin1String(action.group)},
                                {"custom", m_keys.value(id) != action.defaults}});
    }
    return list;
}

void Shortcuts::save(const QString &action) {
    Settings::instance().setValue(kGroup + action, m_keys.value(action));
}
