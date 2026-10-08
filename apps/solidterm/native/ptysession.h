#pragma once

#include <QObject>
#include <QSocketNotifier>
#include <QStringList>

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
    void resize(int rows, int cols);
    void writeBytes(const QByteArray &bytes);
    bool running() const { return m_pid > 0; }
    qint64 pid() const { return m_pid; }

signals:
    void bytesRead(const QByteArray &bytes);
    void finished();

private:
    void onReadable();

    int m_master = -1;
    qint64 m_pid = -1;
    QSocketNotifier *m_notifier = nullptr;
};
