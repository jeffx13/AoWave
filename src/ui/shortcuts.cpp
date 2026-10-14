#include "ui/shortcuts.h"

#include <QKeySequence>
#include <QSet>
#include "app/settings.h"

namespace {

const QString kGroup = QStringLiteral("shortcuts/");
Shortcuts *s_current = nullptr;

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
        {"previousChapter", "player", {"Ctrl+Left"}},
        {"nextChapter", "player", {"Ctrl+Right"}},
        {"subtitlesEarlier", "player", {"Ctrl+["}},
        {"subtitlesLater", "player", {"Ctrl+]"}},
        {"audioEarlier", "player", {"Ctrl+-"}},
        {"audioLater", "player", {"Ctrl+=", "Ctrl++"}},
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
    QSet<QString> taken;
    for (Action &action : m_actions) {
        for (QString &key : action.defaults) key = normalized(key);
        const QVariant saved = Settings::instance().value(kGroup + QLatin1String(action.id));
        if (!saved.isValid()) continue;
        m_keys.insert(QLatin1String(action.id), saved.toStringList());
        for (const QString &key : saved.toStringList()) taken.insert(key);
    }
    for (const Action &action : std::as_const(m_actions)) {
        if (m_keys.contains(QLatin1String(action.id))) continue;
        QStringList keys;
        for (const QString &key : action.defaults)
            if (!taken.contains(key)) keys << key;
        m_keys.insert(QLatin1String(action.id), keys);
    }
    s_current = this;
}

Shortcuts::~Shortcuts() {
    if (s_current == this) s_current = nullptr;
}

QString Shortcuts::firstKey(const QString &action) {
    return s_current ? s_current->m_keys.value(action).value(0) : QString();
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

void Shortcuts::addKey(const QString &action, const QString &keyText) {
    if (!m_keys.contains(action) || keyText.isEmpty() || m_keys[action].contains(keyText)) return;
    if (const QString previous = actionFor(keyText); !previous.isEmpty()) {
        m_keys[previous].removeAll(keyText);
        save(previous);
    }
    m_keys[action].append(keyText);
    save(action);
    emit changed();
}

void Shortcuts::restore(const QVariantMap &bindings) {
    for (auto it = bindings.cbegin(); it != bindings.cend(); ++it) {
        if (!m_keys.contains(it.key())) continue;
        m_keys[it.key()] = it.value().toStringList();
        save(it.key());
    }
    emit changed();
}

void Shortcuts::reset(const QString &action) {
    for (const Action &entry : std::as_const(m_actions)) {
        if (action != QLatin1String(entry.id)) continue;
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
