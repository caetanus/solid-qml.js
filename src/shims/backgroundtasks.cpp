#include "backgroundtasks.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlEngine>
#include <QStandardPaths>
#include <QTimer>

namespace SolidWorkers {

BackgroundTasks::BackgroundTasks(QQmlEngine *engine)
    : QObject(engine)
    , m_engine(engine)
{
    loadPersisted();
}

QString BackgroundTasks::storePath() const
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir + QStringLiteral("/background-tasks.json");
}

void BackgroundTasks::schedule(const QVariantMap &spec)
{
    const QString id = spec.value(QStringLiteral("id")).toString();
    if (id.isEmpty())
        return;
    cancel(id); // same-id replace (WorkManager REPLACE semantics)

    Task task;
    task.spec = spec;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 delay = spec.contains(QStringLiteral("delayMs"))
        ? spec.value(QStringLiteral("delayMs")).toLongLong()
        : spec.value(QStringLiteral("periodMs")).toLongLong();
    task.nextDueMs = now + qMax<qint64>(0, delay);
    m_tasks.insert(id, task);
    arm(id, false);
    if (spec.value(QStringLiteral("persist")).toBool())
        persistAll();
}

void BackgroundTasks::cancel(const QString &id)
{
    auto it = m_tasks.find(id);
    if (it == m_tasks.end())
        return;
    if (it->timer)
        it->timer->deleteLater();
    const bool persisted = it->spec.value(QStringLiteral("persist")).toBool();
    m_tasks.erase(it);
    if (persisted)
        persistAll();
}

QVariantList BackgroundTasks::list() const
{
    QVariantList out;
    for (auto it = m_tasks.cbegin(); it != m_tasks.cend(); ++it) {
        QVariantMap entry = it->spec;
        entry.insert(QStringLiteral("nextDueMs"), it->nextDueMs);
        out.append(entry);
    }
    return out;
}

void BackgroundTasks::arm(const QString &id, bool missedFire)
{
    Task &task = m_tasks[id];
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 waitMs = qMax<qint64>(0, task.nextDueMs - now);
    if (task.timer)
        task.timer->deleteLater();
    task.timer = new QTimer(this);
    task.timer->setSingleShot(true);
    connect(task.timer, &QTimer::timeout, this, [this, id] { fire(id, false); });
    task.timer->start(int(qMin<qint64>(waitMs, INT_MAX)));
    if (missedFire)
        QTimer::singleShot(0, this, [this, id] { fire(id, true); });
}

void BackgroundTasks::fire(const QString &id, bool missed)
{
    auto it = m_tasks.find(id);
    if (it == m_tasks.end())
        return;
    QVariantMap info = it->spec;
    info.insert(QStringLiteral("firedAt"), QDateTime::currentMSecsSinceEpoch());
    info.insert(QStringLiteral("missed"), missed);
    const qint64 period = it->spec.value(QStringLiteral("periodMs")).toLongLong();
    if (period > 0) {
        if (!missed) { // a missed periodic fire is extra — the regular timer is already armed
            it->nextDueMs = QDateTime::currentMSecsSinceEpoch() + period;
            arm(id, false);
            if (it->spec.value(QStringLiteral("persist")).toBool())
                persistAll();
        }
    } else {
        // One-shot: consumed — missed or not.
        const bool persisted = it->spec.value(QStringLiteral("persist")).toBool();
        if (it->timer)
            it->timer->deleteLater();
        m_tasks.erase(it);
        if (persisted)
            persistAll();
    }
    emit taskDue(id, info);
}

void BackgroundTasks::persistAll() const
{
    QJsonArray arr;
    for (auto it = m_tasks.cbegin(); it != m_tasks.cend(); ++it) {
        if (!it->spec.value(QStringLiteral("persist")).toBool())
            continue;
        QJsonObject o = QJsonObject::fromVariantMap(it->spec);
        o.insert(QStringLiteral("nextDueMs"), double(it->nextDueMs));
        arr.append(o);
    }
    QFile f(storePath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

void BackgroundTasks::loadPersisted()
{
    QFile f(storePath());
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonArray arr = QJsonDocument::fromJson(f.readAll()).array();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const auto &entry : arr) {
        QVariantMap spec = entry.toObject().toVariantMap();
        const QString id = spec.value(QStringLiteral("id")).toString();
        if (id.isEmpty())
            continue;
        qint64 due = qint64(spec.take(QStringLiteral("nextDueMs")).toDouble());
        Task task;
        task.spec = spec;
        const bool missed = due <= now;
        const qint64 period = spec.value(QStringLiteral("periodMs")).toLongLong();
        task.nextDueMs = missed && period > 0 ? now + period : due;
        m_tasks.insert(id, task);
        if (missed && period <= 0) {
            // Overdue one-shot: ONLY the missed run (which consumes it) — a live timer at ~0ms
            // would double-fire.
            QTimer::singleShot(0, this, [this, id] { fire(id, true); });
        } else {
            // On time, or overdue periodic (re-aimed one period out + one missed run now).
            arm(id, missed);
        }
    }
}

void BackgroundTasks::install(QQmlEngine *engine)
{
    auto *tasks = new BackgroundTasks(engine);
    engine->globalObject().setProperty(QStringLiteral("__solidBgTasks"), engine->newQObject(tasks));
    QJSValue mod = engine->evaluate(QStringLiteral(R"((function () {
        var handlers = {};
        __solidBgTasks.taskDue.connect(function (id, info) {
            var fn = handlers[id];
            if (fn) fn(info);
        });
        return {
            scheduleTask: function (spec) { __solidBgTasks.schedule(spec); },
            cancelTask: function (id) { __solidBgTasks.cancel(String(id)); },
            listTasks: function () { return __solidBgTasks.list(); },
            onTask: function (id, fn) { handlers[String(id)] = fn; }
        };
    })())"));
    mod.setProperty(QStringLiteral("default"), mod);
    engine->registerModule(QStringLiteral("solid:background"), mod);
}

} // namespace SolidWorkers
