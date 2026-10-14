#include "providers/provider.h"
#include "app/mainwindow.h"
#include "app/logger.h"
#include "app/settings.h"
#include "media/player/danmaku.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <QFuture>
#include <QtConcurrent/QtConcurrentRun>
#include <memory>
#include <optional>

Account::Account(const Config::Section &store, QObject *parent)
    : QObject(parent), m_store(store.path), m_fields(Settings::instance().section(store)) {
    // settings.ini is edited by hand while the app runs; Settings says when it was read again.
    connect(&Settings::instance(), &Settings::settingsChanged, this, [this] {
        const Fields stored = Settings::instance().section(Config::Section{m_store});
        if (stored == fields()) return;
        logInfo() << "Sources" << label() + QLatin1Char(':') << "the session changed in settings.ini";
        remember(stored);
        adopted();
    });
    // The texts come from tr(); the window's retranslate() does not re-read C++ properties, so the account asks again.
    connect(&Settings::instance(), &Settings::uiLanguageChanged, this, [this] {
        emit changed();
        emit loginChanged();
        if (m_standing.signedIn && m_standing.asked) refresh();
    }, Qt::QueuedConnection);
}

QString Account::pastePrompt() const {
    const QStringList need = required();
    return need.isEmpty() ? QString() : tr("Or paste a session (%1)").arg(need.join(QStringLiteral(", ")));
}

QString Account::membership() const {
    if (!m_standing.signedIn || !m_standing.asked) return {};
    if (!m_standing.member) {
        if (!m_standing.until.isValid() || m_standing.until >= QDate::currentDate()) return tr("No membership");
        const qint64 ago = m_standing.until.daysTo(QDate::currentDate());
        const QString on = m_standing.until.toString(Qt::ISODate);
        return ago == 1 ? tr("Membership expired 1 day ago, on %1").arg(on) : tr("Membership expired %1 days ago, on %2").arg(ago).arg(on);
    }
    const QString plan = m_standing.plan.isEmpty() ? tr("Member") : m_standing.plan;
    if (m_standing.lifetime) return tr("%1 · for life").arg(plan);
    if (!m_standing.until.isValid()) return plan;
    const qint64 days = QDate::currentDate().daysTo(m_standing.until);
    if (days < 0) {
        const QString on = m_standing.until.toString(Qt::ISODate);
        return days == -1 ? tr("Membership expired 1 day ago, on %1").arg(on) : tr("Membership expired %1 days ago, on %2").arg(-days).arg(on);
    }
    if (days == 0) return tr("%1 · ends today").arg(plan);
    if (days == 1) return tr("%1 · 1 day left, until %2").arg(plan, m_standing.until.toString(Qt::ISODate));
    return tr("%1 · %2 days left, until %3").arg(plan).arg(days).arg(m_standing.until.toString(Qt::ISODate));
}

QString Account::tone() const {
    if (!m_standing.signedIn) return QStringLiteral("none");
    if (!m_standing.problem.isEmpty()) return QStringLiteral("bad");
    if (!m_standing.caution.isEmpty()) return QStringLiteral("warn");
    if (!m_standing.asked) return QStringLiteral("none");
    const qint64 days = m_standing.until.isValid() ? QDate::currentDate().daysTo(m_standing.until) : 365;
    if (m_standing.member) return !m_standing.lifetime && days <= 7 ? QStringLiteral("warn") : QStringLiteral("ok");
    return m_standing.until.isValid() && days < 0 ? QStringLiteral("warn") : QStringLiteral("none");
}

QString Account::plain(double number) {
    return number == std::floor(number) && std::abs(number) < 1e15 ? QString::number(number, 'f', 0) : QString::number(number, 'g', 15);
}

QString Account::expiry() const {
    if (!m_standing.signedIn || !m_standing.sessionEnds.isValid()) return {};
    if (m_standing.sessionEnds < QDateTime::currentDateTime()) return tr("Ended");
    const QString until = m_standing.sessionEnds.toString(QStringLiteral("yyyy-MM-dd HH:mm"));
    const qint64 days = QDate::currentDate().daysTo(m_standing.sessionEnds.date());
    return days >= 2 ? tr("Until %1 · %2 days left").arg(until).arg(days) : tr("Until %1").arg(until);
}

QVariantList Account::details() const {
    QVariantList rows;
    for (const auto &[label, value] : m_standing.details)
        if (!value.isEmpty()) rows.append(QVariantMap{{QStringLiteral("label"), label}, {QStringLiteral("value"), value}});
    return rows;
}

QString Account::checked() const {
    if (!m_standing.signedIn) return {};
    if (!m_standing.checkedAt.isValid()) return tr("Not checked yet");
    const QDateTime at = m_standing.checkedAt;
    return at.date() == QDate::currentDate() ? at.toString(QStringLiteral("HH:mm")) : at.toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

Account::Fields Account::fields() const {
    QMutexLocker lock(&m_fieldsMutex);
    return m_fields;
}

Account::Fields Account::parse(const QString &text) {
    Fields fields;
    QString body = text.trimmed();
    if (body.startsWith(QLatin1Char('{'))) {
        const QJsonObject object = QJsonDocument::fromJson(body.toUtf8()).object();
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            const QString value = it->isString() ? it->toString() : it->isDouble() ? plain(it->toDouble()) : QString();
            if (!value.isEmpty()) fields.insert(it.key(), value);
        }
        return fields;
    }
    if (body.startsWith(QLatin1String("Cookie:"), Qt::CaseInsensitive)) body.remove(0, 7);
    static const QRegularExpression separator(QStringLiteral("[;&\\n\\r\\t]"));
    for (const QString &part : body.split(separator, Qt::SkipEmptyParts)) {
        const int equals = part.indexOf(QLatin1Char('='));
        if (equals <= 0) continue;
        const QString name = part.left(equals).trimmed(), value = part.mid(equals + 1).trimmed();
        if (!name.isEmpty() && !value.isEmpty()) fields.insert(name, value);
    }
    return fields;
}

void Account::remember(const Fields &fields) {
    {
        QMutexLocker lock(&m_fieldsMutex);
        m_fields = fields;
    }
    emit sessionChanged();
}

void Account::keep(const Fields &session) {
    Fields stored;
    for (auto it = session.constBegin(); it != session.constEnd(); ++it)
        if (!it.value().isEmpty()) stored.insert(it.key(), it.value());
    if (stored == fields()) return;
    Settings::instance().setSection(Config::Section{m_store}, stored);
    remember(stored);
}

bool Account::signInWithText(const QString &pasted) {
    const Fields session = parse(pasted);
    const QStringList need = required();
    if (need.isEmpty()) return false;
    if (!std::all_of(need.begin(), need.end(), [&](const QString &name) { return !session.value(name).isEmpty(); })) {
        MainWindow::instance().reportError(tr("That does not look like a %1 session. It has to contain %2.")
                                             .arg(label(), need.join(QStringLiteral(", "))), label());
        return false;
    }
    cancelLogin();
    keep(session);
    adopted();
    MainWindow::instance().reportInfo(tr("Signed in to %1.").arg(label()), label());
    return true;
}

void Account::signOut() {
    cancelLogin();
    keep({});
    setStanding({});
    setBusy(false);
}

void Account::setStanding(const Standing &standing) {
    m_standing = standing;
    emit changed();
}

void Account::setBusy(bool busy) {
    if (m_busy == busy) return;
    m_busy = busy;
    emit changed();
}

void Account::setLogin(const QString &status, const QString &qr) {
    if (m_loginStatus == status && m_loginQr == qr) return;
    m_loginStatus = status;
    m_loginQr = qr;
    emit loginChanged();
}

void Provider::offerDanmaku(Source &info, const QString &key, const QVariantMap &ids) {
    DanmakuSource *source = danmaku();
    if (!source) return;
    info.danmakuKey = key;
    info.danmakuSource = [source, ids, started = std::make_shared<std::optional<QFuture<QList<DanmakuComment>>>>()]() {
        if (!*started)
            *started = QtConcurrent::run([source, ids] {
                Client worker({}, false);
                return source->comments(&worker, ids);
            });
        return **started;
    };
    if (DanmakuOptions::current().enabled) info.danmakuSource();
}
