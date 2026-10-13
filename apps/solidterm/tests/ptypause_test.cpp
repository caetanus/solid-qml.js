// PtySession::pause()/resume() — the backpressure mechanism behind the owner's ask ("quando
// seleciono texto o stdout tem que travar"): while a local selection drag is live, TerminalView
// stops reading the pty master (disables the QSocketNotifier). Nothing is buffered in our process;
// the writing program simply blocks on write(2) once the kernel's pty buffer fills, exactly like a
// flow-controlled line (XOFF) — no data lost, no busy loop. Real pty, real child shell throughout
// (no Qt GUI needed — PtySession is pure QtCore + POSIX).
//
// Gotcha found while writing this: the pty echoes back whatever WE write via writeBytes() (local
// echo, on by default) — so a naive `captured.contains(marker)` check can match the marker text
// INSIDE the echoed command line we just sent, long before the shell has executed anything. Rather
// than fight terminal echo (disabling it is itself a command that has to be typed-and-processed
// before it takes effect — another race, and a worse one under CPU contention), the marker is
// never spelled out contiguously in the bytes we WRITE: `m1="PTYPAUSE"; m2="-DONE"; echo "$m1$m2"`
// only produces the literal "PTYPAUSE-DONE" substring in the shell's OWN OUTPUT, never in the
// typed source (so even an echoed copy of our input can't match it). The digit sequence itself
// doesn't need this treatment — an echoed command line is one long line full of non-digit
// characters, so it never matches the "whole line is digits" filter below regardless of echo.
//
// Proven two independent ways:
//  1. backpressureBlocksChild: a side-channel progress FILE the shell writes to (via an ATOMIC
//     rename, `mv -f tmp final` — a plain `>` redirect truncates-then-writes, which a concurrent
//     reader can catch mid-truncate; rename(2) has no such window) only right AFTER its stdout
//     echo of the same counter. If echo's write(2) blocks on the full pty buffer, the shell never
//     reaches the next statement, so the file's counter visibly stops advancing — true with or
//     without us ever reading the (deliberately unread, paused) pty itself. The plateau wait is
//     generous (tens of seconds) because this runs alongside the rest of the suite in parallel —
//     under heavy CPU contention the child is simply slower to reach the kernel buffer's limit,
//     not unblocked; it always gets there eventually.
//  2. resumeDrainsWithoutDataLoss: a bounded 1..N sequence, paused BEFORE our event loop ever gets
//     a chance to read any of it (so the kernel buffer fills and the child blocks well before N is
//     reached), must still arrive byte-for-byte once resumed and drained — proving pause+resume
//     loses nothing even when the buffer overflowed while frozen.
#include "../native/ptysession.h"

#include <QCoreApplication>
#include <QFile>
#include <QRegularExpression>
#include <QElapsedTimer>
#include <QTest>
#include <QSocketNotifier>
#include <QThread>

namespace {

// Progress file lives next to the test binary (the build dir) — never under /tmp.
QString progressFilePath()
{
    return QCoreApplication::applicationDirPath()
        + QStringLiteral("/ptypause-progress-%1.txt").arg(QCoreApplication::applicationPid());
}

qint64 readProgressValue(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return -1;
    const QByteArray bytes = f.readAll().trimmed();
    bool ok = false;
    const qint64 v = bytes.toLongLong(&ok);
    return ok ? v : -1;
}

// A LOGIN shell (PtySession always starts one — see ptysession.cpp's argv0 "-<shell>") sources
// /etc/profile, which on this machine pulls in shell-integration snippets (VTE's preexec/precmd
// OSC sequences were observed in practice) that interleave control codes with the real output —
// completely normal for a real terminal (libvterm parses and discards them), but this test parses
// raw bytes as text, so strip the two escape forms VT100/xterm programs actually emit: CSI
// (ESC [ ... final-byte) and OSC (ESC ] ... BEL-or-ESC\) — anything left over is real program text.
QString stripAnsi(const QString &text)
{
    static const QRegularExpression kCsi(QStringLiteral("\x1b\\[[0-9;:?]*[ -/]*[@-~]"));
    static const QRegularExpression kOsc(QStringLiteral("\x1b\\][^\x07\x1b]*(?:\x07|\x1b\\\\)"));
    QString out = text;
    out.remove(kOsc);
    out.remove(kCsi);
    out.remove(QLatin1Char('\r'));
    return out;
}

} // namespace

class PtyPauseTest : public QObject {
    Q_OBJECT

private slots:
    void backpressureBlocksChild();
    void resumeDrainsWithoutDataLoss();
    void drainKeepsUpWithBlockedLoopTurns();
};

// ── 1. pause() stops OUR reads AND blocks the child's forward progress (kernel backpressure) ────
void PtyPauseTest::backpressureBlocksChild()
{
    const QString progressPath = progressFilePath();
    const QString progressTmpPath = progressPath + QStringLiteral(".tmp");
    QFile::remove(progressPath);
    QFile::remove(progressTmpPath);

    PtySession pty;
    int readCount = 0;
    connect(&pty, &PtySession::bytesRead, this, [&readCount](const QByteArray &) { ++readCount; });

    QVERIFY(pty.start(24, 80, QStringLiteral("/bin/sh")));
    QVERIFY(pty.running());

    // An unbounded counter (like `yes`, but with a side channel): echoes to the pty (paused/read by
    // us) AND, right after, atomically renames a progress file into place — a completely separate
    // syscall the pty's backpressure has no direct say over, except that the shell can't reach that
    // statement at all while blocked on the echo before it.
    const QString cmd = QStringLiteral(
        "i=1; while true; do echo COUNT $i; printf '%s' \"$i\" > '%1'; mv -f '%1' '%2'; i=$((i+1)); done\n")
        .arg(progressTmpPath, progressPath);
    pty.writeBytes(cmd.toUtf8());

    // Let it run free for a bit: both our read count and the progress file must be growing.
    QTRY_VERIFY_WITH_TIMEOUT(readCount > 0, 10000);
    qint64 progress = -1;
    QTRY_VERIFY_WITH_TIMEOUT((progress = readProgressValue(progressPath)) >= 20, 10000);

    // ── Pause: stop reading. ─────────────────────────────────────────────────────────────────────
    pty.pause();
    QVERIFY(pty.paused());
    const int readCountAtPauseSettled = readCount; // bytesRead must never fire again from here on

    // The shell doesn't block the INSTANT we stop reading — it keeps running until the kernel's pty
    // buffer actually fills (how much headroom that buffer has is not this test's business, and how
    // long that takes depends on how much CPU the child actually gets — this suite runs tests in
    // parallel). Poll until two consecutive samples match (plateaued = genuinely blocked), with a
    // cap generous enough to survive heavy contention; it always gets there eventually.
    qint64 last = readProgressValue(progressPath);
    QVERIFY(last >= 0);
    bool plateaued = false;
    for (int waited = 0; waited < 30000 && !plateaued; waited += 250) {
        QTest::qWait(250);
        const qint64 now = readProgressValue(progressPath);
        plateaued = (now == last);
        last = now;
    }
    QVERIFY2(plateaued, "child's progress counter never plateaued — pause() did not block it");
    const qint64 progressAtPlateau = last;

    // Hold a bit longer: a genuinely blocked child stays blocked, and we must still see NO new
    // bytesRead at all (the notifier stayed disabled this whole time).
    QTest::qWait(500);
    QCOMPARE(readCount, readCountAtPauseSettled);
    QCOMPARE(readProgressValue(progressPath), progressAtPlateau);

    // ── Resume: reads and the child's progress must both continue. ─────────────────────────────
    pty.resume();
    QVERIFY(!pty.paused());
    QTRY_VERIFY_WITH_TIMEOUT(readCount > readCountAtPauseSettled, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(readProgressValue(progressPath) > progressAtPlateau, 10000);

    QFile::remove(progressPath);
    QFile::remove(progressTmpPath);
    // PtySession's destructor SIGHUPs this (our own, test-owned) child — nothing left running.
}

// ── 2. A bounded, checksummable sequence paused mid-stream still arrives intact after resume ────
void PtyPauseTest::resumeDrainsWithoutDataLoss()
{
    constexpr int kCount = 20000; // large enough to overflow the kernel pty buffer while paused

    PtySession pty;
    QByteArray captured;
    connect(&pty, &PtySession::bytesRead, this, [&captured](const QByteArray &b) { captured += b; });

    QVERIFY(pty.start(24, 80, QStringLiteral("/bin/sh")));

    // Pause BEFORE our own event loop gets even one chance to read any of this: writeBytes() only
    // puts bytes on the wire (the pty's write() is synchronous from our side), and pause() runs in
    // the same call stack with no intervening QCoreApplication::processEvents — so nothing of this
    // command's OWN output can have been read yet, whatever the shell's actual scheduling does
    // concurrently. This is the actual pause-mid-stream path: the kernel buffer fills and the child
    // blocks well before kCount is reached. See the file header for why the marker is spelled
    // "m1"+"m2" rather than a literal contiguous string.
    const QString cmd = QStringLiteral(
        "i=1; while [ $i -le %1 ]; do echo $i; i=$((i+1)); done; "
        "m1=\"PTYPAUSE\"; m2=\"-DONE\"; echo \"$m1$m2\"\n").arg(kCount);
    pty.writeBytes(cmd.toUtf8());
    pty.pause();
    const int sizeAtPause = captured.size();
    QTest::qWait(500);
    QCOMPARE(captured.size(), sizeAtPause); // frozen — proves the pause took effect before any of
                                             // this command's output could have been read

    pty.resume();
    QTRY_VERIFY_WITH_TIMEOUT(captured.contains("PTYPAUSE-DONE"), 30000);

    // Parse every all-digit line (after stripping terminal escape sequences — see stripAnsi — and
    // skipping any other non-digit noise, including the echoed command line itself if local echo
    // happened to still be on) and verify the EXACT 1..kCount run: no gaps, no duplicates, no
    // reordering, nothing lost to the pause.
    const QStringList lines = stripAnsi(QString::fromUtf8(captured)).split(QLatin1Char('\n'));
    static const QRegularExpression kAllDigits(QStringLiteral("^[0-9]+$"));
    QVector<qint64> nums;
    for (const QString &line : lines) {
        if (kAllDigits.match(line).hasMatch())
            nums.append(line.toLongLong());
    }

    if (nums.size() != kCount) {
        qWarning() << "DEBUG nums.size()=" << nums.size() << "first=" << (nums.isEmpty() ? -1 : nums.first())
                   << "last=" << (nums.isEmpty() ? -1 : nums.last());
        for (int i = 1; i < nums.size(); ++i) {
            if (nums[i] != nums[i - 1] + 1)
                qWarning() << "DEBUG gap/dup around index" << i << ":" << nums[i - 1] << "->" << nums[i];
        }
    }
    QCOMPARE(nums.size(), kCount);
    for (int i = 0; i < nums.size(); ++i) {
        if (nums[i] != i + 1) {
            QFAIL(qPrintable(QStringLiteral("sequence broken at index %1: got %2, want %3")
                .arg(i).arg(nums[i]).arg(i + 1)));
        }
    }
}

// ── 3. One notifier activation drains more than one pty read ────────────────────────────────────
// Regression for the 2026-10-13 bench stall (docs/evidence/solidterm/parser-thread-2026-10-13.md):
// on Wayland the GUI loop can phase-lock into ONE turn per vsync (each turn then blocks ~16 ms in Qt
// Quick's polishAndSync). A pty hands out ~4 KiB per read(2), so with one read per notifier
// activation a flood drained at ~250 KB/s and a 100 MB `cat` "stopped". The activation is fired by
// hand here (the notifier itself is disabled, so the event loop plays no part) while a writer keeps
// the pty full: the pre-fix PtySession read 4095 bytes per activation (measured), the drain reads
// until its time budget is spent.
void PtyPauseTest::drainKeepsUpWithBlockedLoopTurns()
{
    PtySession pty;
    qint64 got = 0;
    connect(&pty, &PtySession::bytesRead, this, [&](const QByteArray &b) {
        got += b.size();
        // What TerminalView does with a chunk costs time (libvterm parses a mixed flood at ~27 MB/s,
        // ~150 us per 4 KiB); meanwhile the writer refills the pty, so the drain finds more queued.
        QElapsedTimer busy;
        busy.start();
        while (busy.nsecsElapsed() < 150000 * (qint64(b.size()) + 4095) / 4096) {}
    });
    QVERIFY(pty.start(24, 80, QStringLiteral("/bin/sh")));
    // Type only once the login shell sits at its prompt (its startup files run first and may
    // discard typeahead): output arrived, then 300 ms of quiet.
    QTRY_VERIFY_WITH_TIMEOUT(got > 0, 10000);
    for (qint64 seen = -1; seen != got;) {
        seen = got;
        QTest::qWait(300);
    }
    auto *notifier = pty.findChild<QSocketNotifier *>();
    QVERIFY(notifier);
    notifier->setEnabled(false);
    pty.writeBytes(QByteArrayLiteral("yes 0123456789abcdefghijklmnopqrstuvwxyz\n"));
    QThread::msleep(300); // the writer fills the pty and blocks

    constexpr int kActivations = 5;
    const qint64 before = got;
    for (int i = 0; i < kActivations; ++i) {
        QVERIFY(QMetaObject::invokeMethod(notifier, "activated", Qt::DirectConnection,
                                          Q_ARG(QSocketDescriptor, QSocketDescriptor(notifier->socket())),
                                          Q_ARG(QSocketNotifier::Type, QSocketNotifier::Read)));
    }
    const qint64 perActivation = (got - before) / kActivations;
    qInfo("bytes drained per notifier activation: %lld", perActivation);
    pty.writeBytes(QByteArrayLiteral("\x03")); // ^C the writer
    QVERIFY2(perActivation > 64 * 1024, "one activation reads one pty chunk again (the bench stall)");
}

QTEST_GUILESS_MAIN(PtyPauseTest)
#include "ptypause_test.moc"
