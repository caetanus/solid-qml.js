#pragma once

#include <QByteArray>
#include <QJSValue>
#include <QObject>
#include <QVariant>

class QQmlEngine;

// SharedArrayBuffer + Atomics.wait/notify for the V4 runtime (owner: "threads" = SAB/Atomics).
//
// V4 ships a SharedArrayBuffer global and real Atomics RMW ops, but TypedArray construction
// rejects a SAB (the as<ArrayBuffer>() vtable cast fails) — so upstream shared memory is
// unusable. The working path (verified in Qt sources): ExecutionEngine::newArrayBuffer(QByteArray)
// adopts the byte array's ref-counted block WITHOUT copying, inherits isShared=true from
// SharedArrayBuffer::init (so V4's own Atomics accept views over it), and NOTHING in the write
// paths ever detaches. Two engines holding ArrayBuffers built from the same QByteArray therefore
// see each other's writes — real shared memory.
//
// So `new SharedArrayBuffer(n)` is overridden to return such a buffer, with the backing
// QByteArray kept by a holder QObject attached as an expando (`__sabHold`) — the holder rides
// the buffer's GC lifetime and is how postMessage extracts the block for pass-through (no copy).
// Atomics.wait/notify (upstream stubs that throw) are replaced with a process-wide futex table
// keyed by backing address: real blocking waits between workers.
namespace SolidWorkers {

// Message-transport capsule: the ref-counted block crossing threads.
struct SharedBufferRef {
    QByteArray bytes;
};

// Keeps the backing block alive exactly as long as the JS buffer object (JS-owned expando).
class SharedBufferHolder : public QObject {
    Q_OBJECT

public:
    explicit SharedBufferHolder(const QByteArray &bytes, QObject *parent = nullptr)
        : QObject(parent)
        , m_bytes(bytes)
    {
    }

    const QByteArray &bytes() const { return m_bytes; }

private:
    QByteArray m_bytes;
};

class SharedBuffers : public QObject {
    Q_OBJECT

public:
    explicit SharedBuffers(QQmlEngine *engine);

    // Overrides globalThis.SharedArrayBuffer and Atomics.wait/notify on the engine.
    static void install(QQmlEngine *engine);

    Q_INVOKABLE QJSValue create(int byteLength);
    // wait/notify take the buffer JS object (view.buffer) + absolute byte offset.
    // wait returns 0 = "ok", 1 = "not-equal", 2 = "timed-out"; timeoutMs < 0 = infinite.
    Q_INVOKABLE int wait(const QJSValue &buffer, int byteOffset, int expected, double timeoutMs);
    Q_INVOKABLE int notify(const QJSValue &buffer, int byteOffset, int count);
    // Rehydrate a transported message on THIS engine (SharedBufferRef → live shared buffer).
    Q_INVOKABLE QJSValue hydrate(const QVariant &data);

    // Transport helpers for the Worker message paths (run on the OWNING engine's thread).
    static QVariant marshal(const QJSValue &message);
    static QJSValue rehydrate(QQmlEngine *engine, const QVariant &data);

private:
    static QByteArray backingOf(const QJSValue &buffer); // null when not one of ours
    QJSValue wrap(const QByteArray &bytes);

    QQmlEngine *m_engine;
};

} // namespace SolidWorkers

Q_DECLARE_METATYPE(SolidWorkers::SharedBufferRef)
