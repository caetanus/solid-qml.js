#pragma once

#include <QObject>
#include <QSocketNotifier>
#include <QStringList>

#include <atomic>
#include <chrono>
#include <cstdint>

// The pty leg of a terminal session: opens a pseudo-terminal, forks the user's shell on the
// slave side (login-shell semantics, TERM=xterm-256color), and streams master-side bytes both
// ways. Pure POSIX + Qt; the VT interpretation lives in TerminalView (libvterm).
class PtySession : public QObject {
    Q_OBJECT

public:
    explicit PtySession(QObject *parent = nullptr);
    ~PtySession() override;

    // Spawn $SHELL (or `program`) at rows×cols. Returns false when the pty/fork fails.
    // `cwd` / `env` ("KEY=VALUE" entries) default to this process's; a window opened for another
    // `solidterm` invocation passes THAT caller's, so the shell starts where it was asked for.
    bool start(int rows, int cols, const QString &program = QString(), const QString &cwd = QString(),
               const QStringList &env = QStringList());
    // Stall-watchdog accounting (stallwatch.h).
    void noteRead(size_t n);
    void resize(int rows, int cols);
    void writeBytes(const QByteArray &bytes);
    bool running() const { return m_pid > 0; }
    qint64 pid() const { return m_pid; }

    // Selection-drag backpressure (owner: "quando seleciono texto o stdout tem que travar"): while a
    // LOCAL selection drag is live, TerminalView calls pause() to STOP reading the master fd — no
    // event-loop polling, just disabling the QSocketNotifier. The writing program's own write(2) then
    // blocks on the kernel's full pty buffer (the same backpressure a real XOFF gives a flow-controlled
    // line) — nothing is read into OUR process and buffered, so nothing can grow unbounded. resume()
    // re-enables the notifier; since it's level-triggered, it simply fires again for whatever the
    // kernel already has queued (and keeps firing, one bounded read(2) per event-loop turn — see
    // onReadable — until drained), no special "drain" loop needed here. Both are idempotent and safe
    // to call before start() (pause()'d-before-start is honoured when the notifier is created).
    void pause();
    void resume();
    bool paused() const { return m_paused; }

signals:
    void bytesRead(const QByteArray &bytes);
    void finished();

private:
    void onReadable();
    // How long one notifier activation may keep reading (see onReadable()).
    static constexpr std::chrono::nanoseconds kDrainBudget { 8'000'000 };
    friend struct StallWatchAccess; // stallwatch.h diagnostics (env-gated)

    int m_master = -1;
    qint64 m_pid = -1;
    QSocketNotifier *m_notifier = nullptr;
    bool m_paused = false;
    // Stall watchdog counters (written only while StallWatch::on).
    std::atomic<int64_t> m_swLastReadNs { 0 };
    std::atomic<int64_t> m_swBytes { 0 };
    std::atomic<int64_t> m_swReads { 0 };
};
