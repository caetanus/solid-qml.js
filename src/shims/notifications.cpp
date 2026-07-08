#include "notifications.h"

#include <QCoreApplication>
#include <QHash>

#if defined(HAVE_QTDBUS)
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusReply>

namespace {
constexpr auto kBusName = "solidqml-notify"; // private connection, dispatched on the worker thread
constexpr auto kService = "org.freedesktop.Notifications";
constexpr auto kPath = "/org/freedesktop/Notifications";
constexpr auto kInterface = "org.freedesktop.Notifications";
} // namespace
#endif

SolidNotifications::SolidNotifications(QObject *parent)
    : QObject(parent)
    , m_worker(new NotifyWorker)
{
    // qbar's NmReader pattern: the worker owns a private bus connection on its own thread;
    // everything crosses back through queued connections, so the GUI thread never blocks on
    // the (synchronous) QDBusConnection API.
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::started, m_worker, &NotifyWorker::start);
    connect(m_worker, &NotifyWorker::ready, this, [this](bool available, bool actions, bool reply) {
        m_available = available;
        m_supportsActions = actions;
        m_supportsReply = reply;
        emit availableChanged();
    });
    connect(m_worker, &NotifyWorker::actionInvoked, this, &SolidNotifications::actionInvoked);
    connect(m_worker, &NotifyWorker::replied, this, &SolidNotifications::replied);
    connect(m_worker, &NotifyWorker::closed, this, &SolidNotifications::closed);
    m_thread.start();
}

SolidNotifications::~SolidNotifications()
{
    m_thread.quit();
    m_thread.wait();
    delete m_worker;
#if defined(HAVE_QTDBUS)
    QDBusConnection::disconnectFromBus(QLatin1String(kBusName));
#endif
}

int SolidNotifications::send(const QVariantMap &spec)
{
    if (!m_available)
        return 0;
    const int id = m_nextId++;
    // Queued: doSend runs on the worker thread, where the sync D-Bus call is harmless.
    QMetaObject::invokeMethod(m_worker, "doSend", Qt::QueuedConnection,
                              Q_ARG(int, id), Q_ARG(QVariantMap, spec));
    return id;
}

void SolidNotifications::close(int id)
{
    QMetaObject::invokeMethod(m_worker, "doClose", Qt::QueuedConnection, Q_ARG(int, id));
}

// ─── worker (bus thread) ────────────────────────────────────────────────────────────────

#if defined(HAVE_QTDBUS)

void NotifyWorker::start()
{
    QDBusConnection bus = QDBusConnection::connectToBus(QDBusConnection::SessionBus,
                                                        QLatin1String(kBusName));
    if (!bus.isConnected() || !bus.interface()->isServiceRegistered(QLatin1String(kService))) {
        emit ready(false, false, false);
        return;
    }

    // Event signals for every notification this connection posts.
    bus.connect(QLatin1String(kService), QLatin1String(kPath), QLatin1String(kInterface),
                QStringLiteral("ActionInvoked"), this, SLOT(onActionInvoked(uint,QString)));
    bus.connect(QLatin1String(kService), QLatin1String(kPath), QLatin1String(kInterface),
                QStringLiteral("NotificationClosed"), this, SLOT(onNotificationClosed(uint,uint)));
    // Inline reply (spec 1.3 / KDE, dunst…): only emitted by servers that advertise it.
    bus.connect(QLatin1String(kService), QLatin1String(kPath), QLatin1String(kInterface),
                QStringLiteral("NotificationReplied"), this, SLOT(onNotificationReplied(uint,QString)));

    QDBusInterface iface(QString::fromLatin1(kService), QString::fromLatin1(kPath), QString::fromLatin1(kInterface), bus);
    const QDBusReply<QStringList> caps = iface.call(QStringLiteral("GetCapabilities"));
    const QStringList capabilities = caps.isValid() ? caps.value() : QStringList();
    const bool actions = capabilities.contains(QLatin1String("actions"));
    m_reply = capabilities.contains(QLatin1String("inline-reply"));
    emit ready(true, actions, m_reply);
}

void NotifyWorker::doSend(int id, const QVariantMap &spec)
{
    QDBusConnection bus = QDBusConnection(QString::fromLatin1(kBusName));
    QDBusInterface iface(QString::fromLatin1(kService), QString::fromLatin1(kPath), QString::fromLatin1(kInterface), bus);

    QStringList actions;
    const QVariantList authorActions = spec.value(QStringLiteral("actions")).toList();
    for (const QVariant &a : authorActions) {
        const QVariantMap m = a.toMap();
        actions << m.value(QStringLiteral("id")).toString()
                << m.value(QStringLiteral("label")).toString();
    }
    const QVariant reply = spec.value(QStringLiteral("reply"));
    if (m_reply && reply.isValid() && reply.toBool()) {
        // The spec'd action pair for an inline text field; the label doubles as placeholder.
        const QString placeholder = reply.userType() == QMetaType::QString
            ? reply.toString() : QStringLiteral("Reply");
        actions << QStringLiteral("inline-reply") << placeholder;
    }

    QVariantMap hints;
    // Replaced notifications reuse the server id so they update in place.
    uint replacesId = 0;
    const int replace = spec.value(QStringLiteral("replace")).toInt();
    if (replace > 0)
        replacesId = m_localToServer.value(replace, 0);

    const QDBusReply<uint> posted = iface.call(
        QStringLiteral("Notify"),
        QCoreApplication::applicationName().isEmpty() ? QStringLiteral("solid-qml")
                                                      : QCoreApplication::applicationName(),
        replacesId,
        spec.value(QStringLiteral("icon")).toString(),
        spec.value(QStringLiteral("title")).toString(),
        spec.value(QStringLiteral("body")).toString(),
        actions,
        hints,
        spec.contains(QStringLiteral("timeoutMs")) ? spec.value(QStringLiteral("timeoutMs")).toInt() : -1);

    if (!posted.isValid())
        return;
    m_serverToLocal.insert(posted.value(), id);
    m_localToServer.insert(id, posted.value());
}

void NotifyWorker::doClose(int id)
{
    const uint serverId = m_localToServer.value(id, 0);
    if (serverId == 0)
        return;
    QDBusConnection bus = QDBusConnection(QString::fromLatin1(kBusName));
    QDBusInterface iface(QString::fromLatin1(kService), QString::fromLatin1(kPath), QString::fromLatin1(kInterface), bus);
    iface.call(QStringLiteral("CloseNotification"), serverId);
}

void NotifyWorker::onActionInvoked(uint serverId, const QString &action)
{
    const int id = localIdFor(serverId);
    if (id > 0 && action != QLatin1String("inline-reply"))
        emit actionInvoked(id, action);
}

void NotifyWorker::onNotificationClosed(uint serverId, uint reason)
{
    const int id = localIdFor(serverId);
    if (id > 0)
        emit closed(id, int(reason));
    m_localToServer.remove(id);
    m_serverToLocal.remove(serverId);
}

void NotifyWorker::onNotificationReplied(uint serverId, const QString &text)
{
    const int id = localIdFor(serverId);
    if (id > 0)
        emit replied(id, text);
}

int NotifyWorker::localIdFor(uint serverId) const
{
    return m_serverToLocal.value(serverId, 0);
}

#else // !HAVE_QTDBUS — no desktop-notification backend on this platform yet.

void NotifyWorker::start() { emit ready(false, false, false); }
void NotifyWorker::doSend(int, const QVariantMap &) { }
void NotifyWorker::doClose(int) { }
void NotifyWorker::onActionInvoked(uint, const QString &) { }
void NotifyWorker::onNotificationClosed(uint, uint) { }
void NotifyWorker::onNotificationReplied(uint, const QString &) { }
int NotifyWorker::localIdFor(uint) const { return 0; }

#endif
