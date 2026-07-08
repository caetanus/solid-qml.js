#pragma once

#include <QHash>
#include <QObject>
#include <QVariant>

class QQmlEngine;
class QTimer;

// Cross-platform background-task API (owner 2026-07-08: "worker (pra mobile)" = OS background
// work). The JS surface is ONE module — `solid:background` — with per-OS backends underneath:
//
//   import { scheduleTask, cancelTask, listTasks, onTask } from "solid:background";
//   scheduleTask({ id: "sync", periodMs: 900000, persist: true,
//                  constraints: { network: true, charging: false } });
//   onTask("sync", (info) => { …do the work, return a Promise to signal completion… });
//
// Backend mapping (the CONTRACT the mobile ports implement — this file is the desktop one):
//   - Desktop (THIS backend): in-process QTimer scheduler. `persist: true` tasks are stored in
//     <AppDataLocation>/background-tasks.json and re-armed on app start (overdue periodic tasks
//     fire once on load). Constraints are accepted/stored but NOT enforced — a desktop app that
//     is running has network/power by assumption.
//   - Android (with the mobile port): WorkManager. periodMs → PeriodicWorkRequest (min 15min,
//     clamped by the OS), delayMs → OneTimeWorkRequest.setInitialDelay, constraints.network →
//     NetworkType.CONNECTED, constraints.charging → setRequiresCharging. The task handler runs
//     in a headless engine (the Worker runtime) woken by the Worker's doWork().
//   - iOS: BGTaskScheduler. periodMs → BGAppRefreshTaskRequest.earliestBeginDate (iOS decides
//     the real cadence), heavy work → BGProcessingTaskRequest (charging/network flags map 1:1).
//     Handlers must call the completion within the OS budget — the promise resolution maps to
//     setTaskCompleted.
//
// Spec fields: { id (required, stable), delayMs XOR periodMs, persist?, constraints? } —
// scheduling the same id replaces the previous registration (WorkManager REPLACE semantics).
namespace SolidWorkers {

class BackgroundTasks : public QObject {
    Q_OBJECT

public:
    explicit BackgroundTasks(QQmlEngine *engine);

    static void install(QQmlEngine *engine);

    Q_INVOKABLE void schedule(const QVariantMap &spec);
    Q_INVOKABLE void cancel(const QString &id);
    Q_INVOKABLE QVariantList list() const;

signals:
    // JS dispatch: onTask handlers hang off this. info = the stored spec + {firedAt, missed}.
    void taskDue(const QString &id, const QVariantMap &info);

private:
    struct Task {
        QVariantMap spec;
        QTimer *timer = nullptr;
        qint64 nextDueMs = 0; // epoch ms, for persistence across restarts
    };

    void arm(const QString &id, bool missedFire);
    void fire(const QString &id, bool missed);
    void persistAll() const;
    void loadPersisted();
    QString storePath() const;

    QQmlEngine *m_engine;
    QHash<QString, Task> m_tasks;
};

} // namespace SolidWorkers
