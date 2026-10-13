#include "tmuxselftest.h"

#include "terminalpanes.h"
#include "terminaltabs.h"
#include "terminalview.h"
#include "termscreen.h"
#include "tmux/seed.h"
#include "tmuxsession.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QPointer>
#include <QProcess>
#include <QQuickWindow>
#include <QTimer>
#include <QtGui/qpa/qwindowsysteminterface.h>

#include <functional>
#include <vterm.h>

namespace {

struct Ctx {
    QPointer<QQuickWindow> window;
    QPointer<TerminalTabs> tabs;
    QPointer<TmuxSession> session;
    QString mode;
    int failures = 0;
    QStringList titlesSeen;            // every tab title the model showed
};
Ctx *g = nullptr;

void check(bool ok, const QString &what)
{
    if (ok) {
        qInfo().noquote() << "solidterm: tmuxtest ok:" << what;
    } else {
        qWarning().noquote() << "solidterm: tmuxtest FAIL:" << what;
        ++g->failures;
    }
}

// Poll `cond` every 25 ms; when it holds (or after timeoutMs) record the check and continue.
void waitFor(std::function<bool()> cond, int timeoutMs, const QString &what, std::function<void()> next)
{
    auto *timer = new QTimer(qApp);
    auto *clock = new QElapsedTimer;
    clock->start();
    timer->setInterval(25);
    QObject::connect(timer, &QTimer::timeout, qApp, [=] {
        const bool ok = cond();
        if (!ok && clock->elapsed() < timeoutMs)
            return;
        timer->stop();
        timer->deleteLater();
        delete clock;
        check(ok, what + (ok ? QString() : QStringLiteral(" (timed out)")));
        next();
    });
    timer->start();
}

void finishTest()
{
    const bool ok = g->failures == 0;
    qInfo().noquote() << "solidterm: tmuxtest" << g->mode << (ok ? "PASS" : "FAIL") << "(" << g->failures << "failure(s) )";
    QCoreApplication::exit(ok ? 0 : 1);
}

TerminalView *view(int pane)
{
    if (!g->tabs)
        return nullptr;
    for (int w : g->tabs->tmuxWindows())
        if (TerminalPanes *p = g->tabs->tmuxTab(w, false))
            if (TerminalView *v = p->tmuxView(pane))
                return v;
    return nullptr;
}

QString dump(int pane)
{
    TerminalView *v = view(pane);
    return v ? v->debugDump() : QString();
}

int count(const QString &hay, const QString &needle)
{
    return needle.isEmpty() ? 0 : int(hay.count(needle));
}

QStringList lines(int pane) { return dump(pane).split(QLatin1Char('\n')); }

int sbCount(int pane)
{
    for (const QString &l : lines(pane))
        if (l.startsWith(QLatin1String("sb ")))
            return l.mid(3).toInt();
    return -1;
}

int rowOf(int pane, const QString &needle) // absolute row (scrollback first), -1 none
{
    const QStringList ls = lines(pane);
    for (int r = 0; r < ls.size(); ++r)
        if (ls[r].contains(needle))
            return r;
    return -1;
}

// The view's TermScreen must equal a bare TermScreen fed the very bytes its feed pipe carried
// (TmuxSession::tee), resized at the same stream points as the view (TerminalView::debugGridLog):
// the pipe → TermWorker → TermScreen path loses and reorders nothing.
QString oracleDump(int pane)
{
    TerminalView *v = view(pane);
    if (!v || !g->session)
        return QStringLiteral("<no view>");
    const QList<QList<qint64>> log = v->debugGridLog();
    const QByteArray bytes = g->session->tee(pane);
    if (log.isEmpty())
        return QStringLiteral("<no grid log>");
    VTerm *vt = TermScreen::newVTerm(int(log[0][1]), int(log[0][2]));
    vterm_set_utf8(vt, 1);
    TermScreen screen;
    screen.setScrollbackLimit(v->scrollbackLimit());
    screen.attach(vt, nullptr, nullptr);
    screen.reset(true);
    qint64 at = 0;
    for (int k = 1; k <= log.size(); ++k) {
        const qint64 upto = k < log.size() ? qMin<qint64>(log[k][0], bytes.size()) : bytes.size();
        if (upto > at)
            vterm_input_write(vt, bytes.constData() + at, size_t(upto - at));
        at = qMax(at, upto);
        if (k < log.size())
            vterm_set_size(vt, int(log[k][1]), int(log[k][2]));
    }
    const QString out = TerminalView::dumpScreen(screen);
    vterm_free(vt);
    return out;
}

void oracleCheck(int pane, std::function<void()> next)
{
    waitFor([pane] { return view(pane) && dump(pane) == oracleDump(pane); }, 3000,
            QStringLiteral("pane %%1: TermScreen equals an oracle fed the same bytes (and resizes)").arg(pane), [pane, next] {
                if (dump(pane) != oracleDump(pane))
                    qWarning().noquote() << "view:\n" << dump(pane) << "\noracle:\n" << oracleDump(pane);
                next();
            });
}

// Cell-exact layout (2.3): every view sits at its layout cells, one header row above it, with the
// grid the layout gives it.
bool geometryOk(TerminalPanes *p, QString *why)
{
    if (!p)
        return false;
    for (const QVariant &e : p->debugTmuxGeometry()) {
        const QVariantMap m = e.toMap();
        if (!m.value(QStringLiteral("visible")).toBool())
            continue;
        const qreal cw = m[QStringLiteral("cellW")].toReal(), ch = m[QStringLiteral("cellH")].toReal();
        const int cx = m[QStringLiteral("cx")].toInt(), cy = m[QStringLiteral("cy")].toInt();
        const int cols = m[QStringLiteral("cols")].toInt(), rows = m[QStringLiteral("rows")].toInt();
        const bool ok = qAbs(m[QStringLiteral("x")].toReal() - cx * cw) < 0.5
            && qAbs(m[QStringLiteral("y")].toReal() - (cy + 1) * ch) < 0.5
            && qAbs(m[QStringLiteral("w")].toReal() - cols * cw) < 0.5
            && qAbs(m[QStringLiteral("h")].toReal() - rows * ch) < 0.5
            && m[QStringLiteral("gridCols")].toInt() == cols && m[QStringLiteral("gridRows")].toInt() == rows;
        if (!ok) {
            if (why)
                *why = QStringLiteral("pane %%%1 at cells %2,%3 %4x%5: view %6,%7 %8x%9 grid %10x%11 (cell %12x%13)")
                           .arg(m[QStringLiteral("pane")].toInt()).arg(cx).arg(cy).arg(cols).arg(rows)
                           .arg(m[QStringLiteral("x")].toReal()).arg(m[QStringLiteral("y")].toReal())
                           .arg(m[QStringLiteral("w")].toReal()).arg(m[QStringLiteral("h")].toReal())
                           .arg(m[QStringLiteral("gridCols")].toInt()).arg(m[QStringLiteral("gridRows")].toInt())
                           .arg(cw).arg(ch);
            return false;
        }
    }
    return true;
}

QVariantMap geomOf(int pane)
{
    for (int w : g->tabs->tmuxWindows())
        if (TerminalPanes *p = g->tabs->tmuxTab(w, false))
            for (const QVariant &e : p->debugTmuxGeometry())
                if (e.toMap().value(QStringLiteral("pane")).toInt() == pane)
                    return e.toMap();
    return {};
}

int gi(int pane, const char *key) { return geomOf(pane).value(QLatin1String(key)).toInt(); }

QString readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

void closeAndDetach(std::function<void()> after)
{
    // The window closes like a user closing it: TerminalTabs goes, the session detaches (5.1).
    g->window->close();
    waitFor([] { return g->session.isNull(); }, 5000, QStringLiteral("closing the window detaches and the client ends"), after);
}

// ─── fake scenario ──────────────────────────────────────────────────────────────────────────────

void fakeEnd()
{
    // Phase 1 is read-only: a key typed into a pane reaches nothing.
    if (g->window) {
        QWindowSystemInterface::handleKeyEvent(g->window, QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, QStringLiteral("a"));
        QWindowSystemInterface::handleKeyEvent(g->window, QEvent::KeyRelease, Qt::Key_A, Qt::NoModifier, QStringLiteral("a"));
        QCoreApplication::processEvents();
    }
    closeAndDetach([] {
        const QString log = readFile(qEnvironmentVariable("FAKE_TMUX_LOG"));
        check(log.contains(QLatin1String("detach-client")), QStringLiteral("the fake tmux received detach-client"));
        check(!log.contains(QLatin1String("send-keys")), QStringLiteral("read-only: nothing was typed into tmux"));
        check(log.contains(QLatin1String("refresh-client -f pause-after=5")), QStringLiteral("pause-after=5 was set on attach"));
        finishTest();
    });
}

void fakeStep4()
{
    g->session->sendTestStep(QStringLiteral("step 4")); // %pause %1
    waitFor([] { return g->session->reseedsCompleted() == 1 && count(dump(1), QStringLiteral("after-pause-1")) == 1; }, 5000,
            QStringLiteral("%pause %1 → one continue+snapshot group → reseeded"), [] {
                const int mark = rowOf(1, QStringLiteral("-- output skipped"));
                check(mark >= 0 && mark < sbCount(1), QStringLiteral("the reseed marker row is in the scrollback"));
                check(rowOf(1, QStringLiteral("after-seed")) >= 0 && rowOf(1, QStringLiteral("after-seed")) < mark,
                      QStringLiteral("output from before the pause stays above the marker"));
                fakeEnd();
            });
}

void fakeStep3()
{
    g->session->sendTestStep(QStringLiteral("step 3")); // rename @2, add @3, close @2
    waitFor([] {
        const QList<int> w = g->tabs->tmuxWindows();
        return w.contains(3) && !w.contains(2) && count(dump(6), QStringLiteral("third window")) == 1;
    }, 5000, QStringLiteral("%window-add @3 (no layout: SolidTerm asks list-windows) opens a tab; %unlinked-window-close @2 removes its tab"), [] {
        check(g->tabs->count() == 2, QStringLiteral("two tabs again"));
        check(g->titlesSeen.contains(QStringLiteral("renamed logs")), QStringLiteral("%window-renamed renamed the tab (no OSC title)"));
        check(g->tabs->tmuxTabName(3) == QStringLiteral("six") || g->tabs->tmuxTabName(3) == QStringLiteral("third"),
              QStringLiteral("the new tab is named: ") + g->tabs->tmuxTabName(3));
        check(!view(4), QStringLiteral("the closed window's view is gone"));
        oracleCheck(6, fakeStep4);
    });
}

void fakeStep2()
{
    g->session->sendTestStep(QStringLiteral("step 2")); // @1 → {[%1,%5],%2}
    waitFor([] { return view(5) && !view(3) && count(dump(5), QStringLiteral("new pane five")) == 1; }, 5000,
            QStringLiteral("%layout-change re-splits: %3 gone, new %5 shows its output"), [] {
                QString why;
                check(geometryOk(g->tabs->tmuxTab(1, false), &why), QStringLiteral("re-split geometry is cell-exact ") + why);
                check(gi(1, "cx") == 0 && gi(5, "cx") == 0 && gi(5, "cy") == gi(1, "cy") + gi(1, "rows") + 1
                          && gi(2, "cx") == gi(1, "cols") + 1,
                      QStringLiteral("%1 above %5 on the left, %2 on the right"));
                check(count(dump(1), QStringLiteral("after-seed")) == 1, QStringLiteral("%1 kept its view (content intact) across the re-split"));
                fakeStep3();
            });
}

void fakeStep1()
{
    g->session->sendTestStep(QStringLiteral("step 1")); // tricky bytes on %2
    waitFor([] { return count(dump(2), QStringLiteral(" end")) == 1; }, 5000, QStringLiteral("step 1 output reaches %2"), [] {
        const QString d = dump(2);
        check(d.contains(QStringLiteral("C:\\Users\\x")), QStringLiteral("backslashes decoded"));
        check(d.contains(QStringLiteral("caf\u00e9 \U0001F680 red end")),
              QStringLiteral("UTF-8 and an escape sequence cut across %output lines reassemble"));
        oracleCheck(2, fakeStep2);
    });
}

void fakeStart()
{
    waitFor([] {
        return g->tabs && g->tabs->count() == 2 && g->session && g->session->seedsCompleted() == 4 && view(1) && view(4)
            && count(dump(1), QStringLiteral("after-seed")) == 1;
    }, 10000, QStringLiteral("attached: two tabs, four panes seeded"), [] {
        if (!g->tabs || g->tabs->count() != 2 || !view(1)) {
            finishTest();
            return;
        }
        QString why;
        check(geometryOk(g->tabs->tmuxTab(1, false), &why), QStringLiteral("window @1 geometry is cell-exact ") + why);
        check(geometryOk(g->tabs->tmuxTab(2, false), &why), QStringLiteral("window @2 geometry is cell-exact ") + why);
        check(gi(1, "cx") == 0 && gi(2, "cx") == gi(1, "cols") + 1 && gi(3, "cx") == gi(2, "cx")
                  && gi(3, "cy") == gi(2, "cy") + gi(2, "rows") + 1,
              QStringLiteral("{%1,[%2,%3]}: %1 left, %2 over %3 on the right"));
        check(g->tabs->tmuxWindows() == QList<int>({ 1, 2 }), QStringLiteral("tabs in window order"));
        check(g->tabs->tmuxTabName(2) == QStringLiteral("logs"), QStringLiteral("a pane without a title shows the window name: ") + g->tabs->tmuxTabName(2));
        const QString d1 = dump(1);
        check(count(d1, QStringLiteral("probe-gap")) == 1 && count(d1, QStringLiteral("raced")) == 1,
              QStringLiteral("output racing the seed shows exactly once"));
        check(rowOf(1, QStringLiteral("hist-00")) >= 0 && rowOf(1, QStringLiteral("hist-00")) < sbCount(1),
              QStringLiteral("tmux's history is in our scrollback"));
        check(g->session->tee(1).startsWith("\x1b" "c"), QStringLiteral("the first bytes into %1's pipe are its seed"));
        check(dump(3).contains(QStringLiteral("alt 1")) && dump(3).contains(QStringLiteral("ALT-TOP")),
              QStringLiteral("an alternate-screen pane comes back on the alternate screen"));
        check(view(1)->readOnly(), QStringLiteral("panes are read-only in phase 1"));
        check(g->window->title().contains(QStringLiteral("read-only (phase 1)")) && g->window->title().startsWith(QStringLiteral("work")),
              QStringLiteral("window title: ") + g->window->title());
        oracleCheck(1, [] { oracleCheck(3, [] { oracleCheck(4, fakeStep1); }); });
    });
}

// ─── real tmux ──────────────────────────────────────────────────────────────────────────────────

void tmuxCmd(const QStringList &args)
{
    QProcess::execute(qEnvironmentVariable("SOLIDTERM_TMUX_BIN"), args);
}

QList<TerminalView *> viewsOf(int window)
{
    TerminalPanes *p = g->tabs ? g->tabs->tmuxTab(window, false) : nullptr;
    return p ? p->views() : QList<TerminalView *>();
}

bool anyViewHas(const QString &needle)
{
    for (int w : g->tabs->tmuxWindows())
        for (TerminalView *v : viewsOf(w))
            if (v->debugDump().contains(needle))
                return true;
    return false;
}

void realStart()
{
    waitFor([] {
        return g->tabs && g->tabs->count() == 2 && g->session && g->session->seedsCompleted() >= 3
            && anyViewHas(QStringLiteral("left-marker")) && anyViewHas(QStringLiteral("right-marker"))
            && anyViewHas(QStringLiteral("second-marker"));
    }, 10000, QStringLiteral("real: attached, two tabs, the three panes seeded with their text"), [] {
        if (!g->tabs || g->tabs->count() != 2) {
            finishTest();
            return;
        }
        const int w0 = g->tabs->tmuxWindows().value(0);
        QString why;
        check(geometryOk(g->tabs->tmuxTab(w0, false), &why), QStringLiteral("real: geometry is cell-exact ") + why);
        const QVariantList geo = g->tabs->tmuxTab(w0, false)->debugTmuxGeometry();
        check(geo.size() == 2 && geo[0].toMap().value(QStringLiteral("cy")) == geo[1].toMap().value(QStringLiteral("cy"))
                  && geo[1].toMap().value(QStringLiteral("cx")).toInt() > geo[0].toMap().value(QStringLiteral("cx")).toInt(),
              QStringLiteral("real: the split shows side by side"));
        tmuxCmd({ QStringLiteral("send-keys"), QStringLiteral("-t"), QStringLiteral("real:0.1"),
                  QStringLiteral("printf 'live-%s\\n' marker"), QStringLiteral("Enter") });
        waitFor([] { return anyViewHas(QStringLiteral("live-marker")); }, 5000, QStringLiteral("real: live output after attach"), [] {
            tmuxCmd({ QStringLiteral("split-window"), QStringLiteral("-v"), QStringLiteral("-t"), QStringLiteral("real:0.1") });
            waitFor([] {
                const int w0 = g->tabs->tmuxWindows().value(0);
                return viewsOf(w0).size() == 3 && geometryOk(g->tabs->tmuxTab(w0, false), nullptr);
            }, 5000, QStringLiteral("real: an outside split adds a cell-exact third pane"), [] {
                tmuxCmd({ QStringLiteral("new-window"), QStringLiteral("-d"), QStringLiteral("-t"), QStringLiteral("real:"), QStringLiteral("-n"), QStringLiteral("third") });
                waitFor([] { return g->tabs->count() == 3; }, 5000, QStringLiteral("real: an outside new-window opens a tab"), [] {
                    tmuxCmd({ QStringLiteral("kill-window"), QStringLiteral("-t"), QStringLiteral("real:third") });
                    waitFor([] { return g->tabs->count() == 2; }, 5000, QStringLiteral("real: killing it closes the tab"), [] {
                        oracleCheck(viewsOf(g->tabs->tmuxWindows().value(1)).value(0) ? viewsOf(g->tabs->tmuxWindows().value(1)).value(0)->tmuxPane() : -1, [] {
                            closeAndDetach(finishTest);
                        });
                    });
                });
            });
        });
    });
}

} // namespace

namespace TmuxSelfTest {

void start(QQuickWindow *window, const QString &mode)
{
    g = new Ctx;
    g->window = window;
    g->mode = mode;
    QList<QQuickItem *> stack { window->contentItem() };
    while (!stack.isEmpty() && !g->tabs) {
        QQuickItem *it = stack.takeLast();
        g->tabs = qobject_cast<TerminalTabs *>(it);
        for (QQuickItem *k : it->childItems())
            stack.append(k);
    }
    if (!g->tabs || !g->tabs->tmuxSession()) {
        qWarning("solidterm: tmuxtest: no tmux-mode TerminalTabs (run with --tmux)");
        QTimer::singleShot(0, qApp, [] { QCoreApplication::exit(1); });
        return;
    }
    g->session = g->tabs->tmuxSession();
    g->session->setTee(true); // before the attach starts (it is queued): every byte, from the first
    QGuiApplication::setQuitOnLastWindowClosed(false); // the verdict comes after the window closes
    QObject::connect(g->tabs, &TerminalTabs::tabsChanged, qApp, [](const QStringList &titles, int) {
        for (const QString &t : titles)
            if (!g->titlesSeen.contains(t))
                g->titlesSeen.append(t);
    });
    if (mode == QLatin1String("real"))
        QTimer::singleShot(0, qApp, realStart);
    else
        QTimer::singleShot(0, qApp, fakeStart);
}

} // namespace TmuxSelfTest
