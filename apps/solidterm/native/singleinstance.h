#pragma once

#include <QString>
#include <QStringList>

// One solidterm process serves every terminal window (owner 2026-10-08: "manter só um processo …
// assim o cold boot só vai na primeira"). A later `solidterm` does not start Qt at all: it connects
// to the running instance's Unix socket, hands over its working directory + environment, waits for
// the acknowledgement and exits — the window opens inside the warm process.
//
// The socket is per installation AND session: its name hashes the binary path, SOLIDTERM_DIR and the
// display variables, so a dev build never forwards to the installed one and a nested compositor gets
// its own instance. A flock decides who becomes the server when two launch at once.
//
// Standalone (the old one-process-per-window behaviour): `--standalone`, SOLIDTERM_STANDALONE=1, or
// any SOLIDTERM_* debug variable (those drive a single scripted window and must not be forwarded).
namespace SingleInstance {

enum class Role {
    Forwarded,  // another instance opened our window: exit now
    Server,     // we are the instance: open the first window, then listen()
    Standalone, // no forwarding, no listening
};

struct Request {
    QString cwd;
    QStringList env; // "KEY=VALUE"
};

// Decide the role. Plain POSIX — call BEFORE constructing QApplication.
Role negotiate(int argc, char **argv);

// Server only, after QApplication exists: accept requests; `onRequest` runs on the GUI thread.
bool listen(void (*onRequest)(const Request &request, void *context), void *context);

} // namespace SingleInstance
