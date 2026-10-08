#include "singleinstance.h"

#include <QCoreApplication>
#include <QLocalServer>
#include <QLocalSocket>

#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

extern char **environ;

namespace SingleInstance {
namespace {

// Wire format, client → server: u32 big-endian payload length, then
//   "SOLIDTERM1" \0 cwd \0 env-entry \0 env-entry \0 …
// Server → client: "OK\n" once the window exists.
constexpr char kMagic[] = "SOLIDTERM1";
constexpr int kAckTimeoutMs = 5000;

std::string g_socketPath;
int g_lockFd = -1; // held for the server's lifetime: "an instance owns this socket path"

uint64_t fnv1a(const std::string &s)
{
    uint64_t h = 1469598103934665603ULL;
    for (const unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

std::string env(const char *name)
{
    const char *v = std::getenv(name);
    return v ? v : "";
}

bool standaloneRequested(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--standalone") == 0)
            return true;
    for (char **e = environ; *e; ++e) {
        if (std::strncmp(*e, "SOLIDTERM_", 10) != 0)
            continue;
        if (std::strncmp(*e, "SOLIDTERM_DIR=", 14) == 0)
            continue; // selects the UI, not a debug mode — part of the socket identity instead
        return true;  // SOLIDTERM_STANDALONE or a debug/screenshot hook
    }
    return false;
}

// "inode:mtime" of a file, or empty. Part of the socket identity: see socketPath().
std::string fileStamp(const std::string &path)
{
    struct stat st;
    if (::stat(path.c_str(), &st) != 0)
        return {};
    return std::to_string(st.st_ino) + ':' + std::to_string(st.st_mtim.tv_sec) + '.'
        + std::to_string(st.st_mtim.tv_nsec);
}

std::string socketPath()
{
    char exe[PATH_MAX] = {};
    const ssize_t n = ::readlink("/proc/self/exe", exe, sizeof exe - 1);
    const std::string exePath = n > 0 ? std::string(exe, size_t(n)) : std::string("solidterm");
    std::string key = exePath;
    for (const char *v : { "SOLIDTERM_DIR", "WAYLAND_DISPLAY", "DISPLAY", "QT_QPA_PLATFORM" }) {
        key += '\n';
        key += env(v);
    }
    // The BUILD is part of the identity too: after an install a still-running instance keeps the
    // old code (and its already-compiled old UI), so forwarding to it would hand out stale windows
    // forever. A new binary or a regenerated UI gets its own socket; the old instance keeps serving
    // only the windows it already has until they close.
    key += '\n';
    key += fileStamp(exePath);
    std::string appDir = env("SOLIDTERM_DIR");
    if (appDir.empty())
        appDir = exePath.substr(0, exePath.rfind('/')) + "/../share/solidterm";
    key += '\n';
    key += fileStamp(appDir + "/App.generated.qml");
    std::string dir = env("XDG_RUNTIME_DIR");
    if (dir.empty())
        dir = "/tmp";
    char name[64];
    std::snprintf(name, sizeof name, "/solidterm-%016llx", static_cast<unsigned long long>(fnv1a(key)));
    return dir + name;
}

// The caller's working directory. Prefer $PWD when it names the same directory: it keeps the path
// the user typed (through symlinks) instead of the resolved one.
std::string callerCwd()
{
    char buf[PATH_MAX];
    std::string cwd = ::getcwd(buf, sizeof buf) ? buf : "";
    const std::string pwd = env("PWD");
    struct stat a, b;
    if (!pwd.empty() && ::stat(pwd.c_str(), &a) == 0 && ::stat(".", &b) == 0
        && a.st_dev == b.st_dev && a.st_ino == b.st_ino)
        cwd = pwd;
    return cwd;
}

bool writeAll(int fd, const char *data, size_t len)
{
    while (len > 0) {
        const ssize_t w = ::write(fd, data, len);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return false;
        data += w;
        len -= size_t(w);
    }
    return true;
}

// Ask a running instance to open our window. True only once it acknowledged.
bool tryForward()
{
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return false;
    sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    if (g_socketPath.size() >= sizeof addr.sun_path) {
        ::close(fd);
        return false;
    }
    std::memcpy(addr.sun_path, g_socketPath.c_str(), g_socketPath.size() + 1);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0) {
        ::close(fd);
        return false;
    }

    std::string payload(kMagic, sizeof kMagic); // includes the NUL separator
    payload += callerCwd();
    payload += '\0';
    for (char **e = environ; *e; ++e) {
        payload += *e;
        payload += '\0';
    }
    const uint32_t len = uint32_t(payload.size());
    const unsigned char header[4] = { uint8_t(len >> 24), uint8_t(len >> 16), uint8_t(len >> 8), uint8_t(len) };
    bool ok = writeAll(fd, reinterpret_cast<const char *>(header), 4) && writeAll(fd, payload.data(), payload.size());

    char reply[3] = {};
    size_t got = 0;
    while (ok && got < 3) {
        pollfd p = { fd, POLLIN, 0 };
        const int r = ::poll(&p, 1, kAckTimeoutMs);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0) {
            ok = false;
            break;
        }
        const ssize_t n = ::read(fd, reply + got, 3 - got);
        if (n <= 0) {
            ok = false;
            break;
        }
        got += size_t(n);
    }
    ::close(fd);
    return ok && std::memcmp(reply, "OK\n", 3) == 0;
}

bool parseRequest(const QByteArray &payload, Request &out)
{
    const QList<QByteArray> parts = payload.split('\0');
    if (parts.size() < 2 || parts[0] != kMagic)
        return false;
    out.cwd = QString::fromLocal8Bit(parts[1]);
    for (qsizetype i = 2; i < parts.size(); ++i)
        if (!parts[i].isEmpty())
            out.env << QString::fromLocal8Bit(parts[i]);
    return true;
}

} // namespace

Role negotiate(int argc, char **argv)
{
    if (standaloneRequested(argc, argv))
        return Role::Standalone;
    g_socketPath = socketPath();
    if (tryForward())
        return Role::Forwarded;

    // Nobody answered. Whoever holds the lock is (or is becoming) the instance.
    const std::string lockPath = g_socketPath + ".lock";
    g_lockFd = ::open(lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (g_lockFd >= 0 && ::flock(g_lockFd, LOCK_EX | LOCK_NB) == 0)
        return Role::Server; // a leftover socket file is stale: listen() replaces it
    if (g_lockFd >= 0) {
        ::close(g_lockFd);
        g_lockFd = -1;
    }
    // Another launch is starting the instance right now: wait for it to come up.
    for (int i = 0; i < 60; ++i) {
        ::usleep(50 * 1000);
        if (tryForward())
            return Role::Forwarded;
    }
    return Role::Standalone; // it never answered: still give the user a terminal
}

bool listen(void (*onRequest)(const Request &, void *), void *context)
{
    if (g_lockFd < 0 || g_socketPath.empty())
        return false;
    const QString path = QString::fromStdString(g_socketPath);
    QLocalServer::removeServer(path); // stale socket of a dead instance (we hold the lock)
    auto *server = new QLocalServer(QCoreApplication::instance());
    server->setSocketOptions(QLocalServer::UserAccessOption);
    if (!server->listen(path)) {
        qWarning("solidterm: cannot listen on %s: %s", g_socketPath.c_str(), qPrintable(server->errorString()));
        delete server;
        return false;
    }
    QObject::connect(server, &QLocalServer::newConnection, server, [server, onRequest, context] {
        while (QLocalSocket *socket = server->nextPendingConnection()) {
            QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            QObject::connect(socket, &QLocalSocket::readyRead, socket, [socket, onRequest, context] {
                if (socket->bytesAvailable() < 4)
                    return;
                const QByteArray head = socket->peek(4);
                const quint32 len = (quint32(uchar(head[0])) << 24) | (quint32(uchar(head[1])) << 16)
                    | (quint32(uchar(head[2])) << 8) | quint32(uchar(head[3]));
                if (len > 16u * 1024 * 1024) { // not one of ours
                    socket->abort();
                    return;
                }
                if (socket->bytesAvailable() < qint64(len) + 4)
                    return; // the rest is still in flight
                socket->read(4);
                Request request;
                if (parseRequest(socket->read(len), request)) {
                    onRequest(request, context);
                    socket->write("OK\n", 3);
                    socket->flush();
                }
                socket->disconnectFromServer();
            });
        }
    });
    return true;
}

} // namespace SingleInstance
