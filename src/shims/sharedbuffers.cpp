#include "sharedbuffers.h"

#include <QDeadlineTimer>
#include <QHash>
#include <QJSValueIterator>
#include <QMutex>
#include <QQmlEngine>
#include <QWaitCondition>

#include <private/qjsvalue_p.h>
#include <private/qv4arraybuffer_p.h>
#include <private/qv4engine_p.h>
#include <private/qv4identifiertable_p.h>
#include <private/qv4object_p.h>

#include <atomic>

namespace SolidWorkers {

namespace {

// Process-wide futex table: one wait slot per (backing address + byte offset). Addresses are
// stable — the shared blocks never reallocate (created once, never resized).
struct FutexTable {
    QMutex mutex;
    struct Slot {
        QWaitCondition cond;
        int waiters = 0;
    };
    QHash<const void *, Slot *> waitSlots;
};

FutexTable *futexTable()
{
    static FutexTable table;
    return &table;
}

// QQmlEngine LOCKS the global object and every reachable builtin (initQmlGlobalObject →
// lockObject, recursive): JS assignment to SharedArrayBuffer or Atomics.wait throws. The lock is
// attribute-level, so swap the VALUE underneath it — resolve the member's slot in the internal
// class and write the member data directly.
void forceSetMember(QQmlEngine *engine, const QJSValue &object, const QString &name, const QJSValue &value)
{
    QV4::ExecutionEngine *v4 = engine->handle();
    QV4::Scope scope(v4);
    QV4::ScopedObject obj(scope, QJSValuePrivate::asReturnedValue(&object));
    if (!obj)
        return;
    QV4::ScopedString s(scope, v4->newString(name));
    // propertyKey() asserts unless the string is interned — go through the identifier table.
    const QV4::PropertyKey key = v4->identifierTable->asPropertyKey(s);
    const auto entry = obj->internalClass()->findValueOrGetter(key);
    if (!entry.isValid())
        return;
    QV4::ScopedValue v(scope, QJSValuePrivate::asReturnedValue(&value));
    obj->setProperty(entry.index, v);
}

} // namespace

SharedBuffers::SharedBuffers(QQmlEngine *engine)
    : QObject(engine)
    , m_engine(engine)
{
}

QJSValue SharedBuffers::wrap(const QByteArray &bytes)
{
    // newArrayBuffer(QByteArray) ADOPTS the ref-counted block (no copy) and the resulting buffer
    // reports isSharedArrayBuffer() == true (inherited init) — V4's Atomics accept views on it,
    // and TypedArray construction works (real ArrayBuffer vtable).
    QV4::ExecutionEngine *v4 = m_engine->handle();
    QV4::Scope scope(v4);
    QV4::ScopedValue ab(scope, v4->newArrayBuffer(bytes)->asReturnedValue());
    // The winning combination: the ArrayBuffer VTABLE (so TypedArray's as<ArrayBuffer> cast
    // accepts views), the isShared FLAG (so V4's native Atomics RMW ops accept them), and the
    // SharedArrayBuffer PROTOTYPE (its byteLength/slice require the flag; ArrayBuffer's own
    // prototype rejects it — spec-mirrored checks on both sides).
    {
        QV4::Scoped<QV4::SharedArrayBuffer> sab(scope, ab);
        if (sab) {
            sab->d()->setSharedArrayBuffer(true);
            QV4::ScopedObject proto(scope, v4->sharedArrayBufferPrototype());
            sab->setPrototypeOf(proto);
        }
    }
    QJSValue buffer = QJSValuePrivate::fromReturnedValue(ab->asReturnedValue());
    // The holder pins the block for exactly the JS object's lifetime (JS-owned expando) and is
    // the extraction point for postMessage pass-through.
    auto *holder = new SharedBufferHolder(bytes);
    QQmlEngine::setObjectOwnership(holder, QQmlEngine::JavaScriptOwnership);
    buffer.setProperty(QStringLiteral("__sabHold"), m_engine->newQObject(holder));
    return buffer;
}

QJSValue SharedBuffers::create(int byteLength)
{
    if (byteLength < 0)
        byteLength = 0;
    return wrap(QByteArray(byteLength, '\0'));
}

QByteArray SharedBuffers::backingOf(const QJSValue &buffer)
{
    const QJSValue hold = buffer.property(QStringLiteral("__sabHold"));
    if (auto *holder = qobject_cast<SharedBufferHolder *>(hold.toQObject()))
        return holder->bytes();
    return QByteArray();
}

int SharedBuffers::wait(const QJSValue &buffer, int byteOffset, int expected, double timeoutMs)
{
    const QByteArray bytes = backingOf(buffer);
    if (bytes.isNull() || byteOffset < 0 || byteOffset + 4 > bytes.size() || (byteOffset % 4))
        return 1;
    const void *addr = bytes.constData() + byteOffset;
    auto *atomic = reinterpret_cast<const std::atomic<qint32> *>(addr);

    FutexTable *table = futexTable();
    QMutexLocker lock(&table->mutex);
    // The check must happen under the futex lock (spec: atomic with respect to notify).
    if (atomic->load(std::memory_order_seq_cst) != expected)
        return 1;
    FutexTable::Slot *&slot = table->waitSlots[addr];
    if (!slot)
        slot = new FutexTable::Slot;
    ++slot->waiters;
    bool woken;
    if (timeoutMs < 0) {
        woken = slot->cond.wait(&table->mutex);
    } else {
        woken = slot->cond.wait(&table->mutex, QDeadlineTimer(qint64(timeoutMs)));
    }
    --slot->waiters;
    return woken ? 0 : 2;
}

int SharedBuffers::notify(const QJSValue &buffer, int byteOffset, int count)
{
    const QByteArray bytes = backingOf(buffer);
    if (bytes.isNull() || byteOffset < 0 || byteOffset + 4 > bytes.size())
        return 0;
    const void *addr = bytes.constData() + byteOffset;
    FutexTable *table = futexTable();
    QMutexLocker lock(&table->mutex);
    FutexTable::Slot *slot = table->waitSlots.value(addr);
    if (!slot || slot->waiters == 0)
        return 0;
    const int woken = count < 0 ? slot->waiters : qMin(count, slot->waiters);
    if (count < 0 || count >= slot->waiters) {
        slot->cond.wakeAll();
    } else {
        for (int i = 0; i < woken; ++i)
            slot->cond.wakeOne();
    }
    return woken;
}

QJSValue SharedBuffers::hydrate(const QVariant &data)
{
    return rehydrate(m_engine, data);
}

QVariant SharedBuffers::marshal(const QJSValue &message)
{
    // The shared buffer itself.
    const QByteArray direct = backingOf(message);
    if (!direct.isNull())
        return QVariant::fromValue(SharedBufferRef{ direct });
    // One level deep: a plain object carrying shared buffers among its properties (the common
    // {sab, …config} shape). Anything deeper copies like any other value.
    if (message.isObject() && !message.isArray() && !message.isCallable()) {
        bool hasShared = false;
        QJSValueIterator probe(message);
        while (probe.hasNext()) {
            probe.next();
            if (!backingOf(probe.value()).isNull()) {
                hasShared = true;
                break;
            }
        }
        if (hasShared) {
            QVariantMap map;
            QJSValueIterator it(message);
            while (it.hasNext()) {
                it.next();
                const QByteArray shared = backingOf(it.value());
                if (!shared.isNull())
                    map.insert(it.name(), QVariant::fromValue(SharedBufferRef{ shared }));
                else
                    map.insert(it.name(), it.value().toVariant());
            }
            return map;
        }
    }
    return message.toVariant();
}

QJSValue SharedBuffers::rehydrate(QQmlEngine *engine, const QVariant &data)
{
    auto *self = engine->property("__solidSharedBuffers").value<SharedBuffers *>();
    if (!self)
        return engine->toScriptValue(data);
    if (data.userType() == qMetaTypeId<SharedBufferRef>())
        return self->wrap(data.value<SharedBufferRef>().bytes);
    if (data.userType() == QMetaType::QVariantMap) {
        const QVariantMap map = data.toMap();
        bool hasShared = false;
        for (auto it = map.cbegin(); it != map.cend(); ++it) {
            if (it.value().userType() == qMetaTypeId<SharedBufferRef>()) {
                hasShared = true;
                break;
            }
        }
        if (hasShared) {
            QJSValue obj = engine->newObject();
            for (auto it = map.cbegin(); it != map.cend(); ++it) {
                if (it.value().userType() == qMetaTypeId<SharedBufferRef>())
                    obj.setProperty(it.key(), self->wrap(it.value().value<SharedBufferRef>().bytes));
                else
                    obj.setProperty(it.key(), engine->toScriptValue(it.value()));
            }
            return obj;
        }
    }
    return engine->toScriptValue(data);
}

void SharedBuffers::install(QQmlEngine *engine)
{
    auto *shared = new SharedBuffers(engine);
    engine->setProperty("__solidSharedBuffers", QVariant::fromValue(shared));
    engine->globalObject().setProperty(QStringLiteral("__solidSAB"), engine->newQObject(shared));
    // Build the replacements as plain JS values…
    const QJSValue parts = engine->evaluate(QStringLiteral(R"((function () {
        // Spec surface: Int32Array only, index in ELEMENTS, timeout in ms (default infinite).
        var verdicts = ["ok", "not-equal", "timed-out"];
        // Clone the native Atomics (the real RMW ops) and add wait/notify: the native object is
        // LOCKED and predates the notify rename (only the throwing wait/wake stubs exist), so it
        // cannot be extended — it gets replaced wholesale.
        var mine = {};
        var ops = ["load", "store", "add", "sub", "and", "or", "xor", "exchange", "compareExchange", "isLockFree"];
        for (var i = 0; i < ops.length; i++)
            if (typeof Atomics[ops[i]] === "function") mine[ops[i]] = Atomics[ops[i]];
        mine.wait = function (ta, index, value, timeout) {
            return verdicts[__solidSAB.wait(ta.buffer, ta.byteOffset + index * 4, value,
                                            timeout === undefined ? -1 : timeout)];
        };
        mine.notify = function (ta, index, count) {
            return __solidSAB.notify(ta.buffer, ta.byteOffset + index * 4,
                                     count === undefined ? -1 : count);
        };
        mine.wake = mine.notify;
        return {
            SharedArrayBuffer: function SharedArrayBuffer(n) { return __solidSAB.create(n); },
            Atomics: mine
        };
    })())"));
    // …and force them under the lock (the native SharedArrayBuffer ctor makes buffers TypedArrays
    // reject — the as<ArrayBuffer> vtable cast fails).
    const QJSValue global = engine->globalObject();
    forceSetMember(engine, global, QStringLiteral("SharedArrayBuffer"),
                   parts.property(QStringLiteral("SharedArrayBuffer")));
    forceSetMember(engine, global, QStringLiteral("Atomics"),
                   parts.property(QStringLiteral("Atomics")));
}

} // namespace SolidWorkers
