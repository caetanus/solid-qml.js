#include "ptysession.h"

#include <QByteArray>
#include <QProcessEnvironment>

#include <csignal>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

PtySession::PtySession(QObject *parent)
    : QObject(parent)
{
}

PtySession::~PtySession()
{
    if (m_pid > 0)
        ::kill(pid_t(m_pid), SIGHUP);
    if (m_master >= 0)
        ::close(m_master);
}

bool PtySession::start(int rows, int cols, const QString &program)
{
    struct winsize ws = {};
    ws.ws_row = static_cast<unsigned short>(rows);
    ws.ws_col = static_cast<unsigned short>(cols);

    int master = -1;
    const pid_t pid = forkpty(&master, nullptr, nullptr, &ws);
    if (pid < 0)
        return false;
    if (pid == 0) {
        // Child: the slave pty is already the controlling terminal (forkpty did login_tty).
        ::setenv("TERM", "xterm-256color", 1);
        ::setenv("COLORTERM", "truecolor", 1);
        const QByteArray shell = program.isEmpty()
            ? qgetenv("SHELL").isEmpty() ? QByteArray("/bin/bash") : qgetenv("SHELL")
            : program.toLocal8Bit();
        // argv[0] with a leading dash = login shell (profile/rc load, like every terminal).
        const QByteArray argv0 = QByteArray("-") + shell.mid(shell.lastIndexOf('/') + 1);
        ::execlp(shell.constData(), argv0.constData(), nullptr);
        ::_exit(127);
    }

    m_master = master;
    m_pid = pid;
    m_notifier = new QSocketNotifier(m_master, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, [this] { onReadable(); });
    return true;
}

void PtySession::resize(int rows, int cols)
{
    if (m_master < 0)
        return;
    struct winsize ws = {};
    ws.ws_row = static_cast<unsigned short>(rows);
    ws.ws_col = static_cast<unsigned short>(cols);
    ::ioctl(m_master, TIOCSWINSZ, &ws);
}

void PtySession::writeBytes(const QByteArray &bytes)
{
    if (m_master < 0 || bytes.isEmpty())
        return;
    qint64 off = 0;
    while (off < bytes.size()) {
        const ssize_t n = ::write(m_master, bytes.constData() + off, size_t(bytes.size() - off));
        if (n <= 0)
            break;
        off += n;
    }
}

void PtySession::onReadable()
{
    char buf[65536];
    const ssize_t n = ::read(m_master, buf, sizeof(buf));
    if (n <= 0) {
        // EOF/EIO: the shell exited. Reap and report.
        m_notifier->setEnabled(false);
        int status = 0;
        ::waitpid(pid_t(m_pid), &status, WNOHANG);
        m_pid = -1;
        emit finished();
        return;
    }
    emit bytesRead(QByteArray(buf, int(n)));
}
