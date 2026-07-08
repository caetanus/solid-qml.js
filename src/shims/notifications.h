#pragma once

#include <QHash>
#include <QObject>
#include <QThread>
#include <QVariantMap>

// Desktop notifications for generated apps, exposed to QML/JS as the `solidNotifications`
// context property (`import { notifications } from "qml-solid"` rewrites to it).
//
// Cross-platform shape, per-platform backends: the public API never changes, `available`
// tells the author what this build/desktop can do. The Linux backend speaks
// org.freedesktop.Notifications (Desktop Notifications spec) — actions, close events and
// inline reply where the server advertises them.
//
// Threading (the qbar NmReader pattern): QDBusConnection calls are synchronous, so a worker
// QObject lives on its OWN QThread with a PRIVATE bus connection (connectToBus with a
// dedicated name); every D-Bus call runs inside the worker thread and results come back to
// the GUI thread through queued signal connections. The GUI thread never touches the bus.
class NotifyWorker;

class SolidNotifications final : public QObject {
    Q_OBJECT
    // A notification server is reachable (Linux: org.freedesktop.Notifications registered).
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    // Server capabilities (Linux: GetCapabilities): action buttons / inline text reply.
    Q_PROPERTY(bool supportsActions READ supportsActions NOTIFY availableChanged)
    Q_PROPERTY(bool supportsReply READ supportsReply NOTIFY availableChanged)

public:
    explicit SolidNotifications(QObject *parent = nullptr);
    ~SolidNotifications() override;

    bool available() const { return m_available; }
    bool supportsActions() const { return m_supportsActions; }
    bool supportsReply() const { return m_supportsReply; }

    // spec: { title, body, icon, timeoutMs, replace (id from a previous send),
    //         actions: [{ id, label }, …], reply: true | "placeholder" }
    // Returns the app-side notification id (echoed by every event signal), 0 when unavailable.
    Q_INVOKABLE int send(const QVariantMap &spec);
    Q_INVOKABLE void close(int id);

signals:
    void availableChanged();
    // An action button was clicked; `action` is the author's action id ("default" for a plain
    // click on servers that advertise it).
    void actionInvoked(int id, const QString &action);
    // The user typed into the notification's inline reply field.
    void replied(int id, const QString &text);
    // The notification left the screen: 1 expired, 2 dismissed, 3 closed by the app.
    void closed(int id, int reason);

private:
    bool m_available = false;
    bool m_supportsActions = false;
    bool m_supportsReply = false;
    int m_nextId = 1;
    NotifyWorker *m_worker = nullptr;
    QThread m_thread;
};

// Bus-side half. Lives on the worker thread; all members are touched ONLY there.
class NotifyWorker final : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

public slots:
    void start();                                // open the private bus, probe capabilities
    void doSend(int id, const QVariantMap &spec); // Notify() — sync, but on this thread
    void doClose(int id);                         // CloseNotification()

signals:
    void ready(bool available, bool actions, bool reply);
    void actionInvoked(int id, const QString &action);
    void replied(int id, const QString &text);
    void closed(int id, int reason);

private slots:
    void onActionInvoked(uint serverId, const QString &action);
    void onNotificationClosed(uint serverId, uint reason);
    void onNotificationReplied(uint serverId, const QString &text);

private:
    int localIdFor(uint serverId) const;

    bool m_reply = false;
    QHash<uint, int> m_serverToLocal;  // server notification id -> app id
    QHash<int, uint> m_localToServer;
};
