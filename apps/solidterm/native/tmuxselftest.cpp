#include "tmuxselftest.h"

#include "terminalpanes.h"
#include "terminaltabs.h"
#include "terminalview.h"
#include "termscreen.h"
#include "tmux/seed.h"
#include "tmuxsession.h"
#include "termconfig.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QKeyEvent>
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
    QString banner;                    // the last tmuxBanner text
    bool bannerReattach = false;
    QVariantList sessions;             // the last tmuxSessionsChanged
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
    // A key typed into the window goes to tmux (phase 2) as a literal.
    if (g->window) {
        QWindowSystemInterface::handleKeyEvent(g->window, QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, QStringLiteral("a"));
        QWindowSystemInterface::handleKeyEvent(g->window, QEvent::KeyRelease, Qt::Key_A, Qt::NoModifier, QStringLiteral("a"));
        QCoreApplication::processEvents();
    }
    closeAndDetach([] {
        const QString log = readFile(qEnvironmentVariable("FAKE_TMUX_LOG"));
        check(log.contains(QLatin1String("detach-client")), QStringLiteral("the fake tmux received detach-client"));
        check(log.contains(QLatin1String(" -l -- 'a'")), QStringLiteral("a typed key went to tmux as send-keys -l"));
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
        check(!view(1)->readOnly(), QStringLiteral("panes accept input"));
        check(g->window->title().startsWith(QStringLiteral("work — ")) && !g->window->title().contains(QStringLiteral("read-only")),
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

// ─── input (phase 2) ────────────────────────────────────────────────────────────────────────────

void key(int k, Qt::KeyboardModifiers m, const QString &text = QString())
{
    QKeyEvent press(QEvent::KeyPress, k, m, text);
    QCoreApplication::sendEvent(g->window, &press); // delivered synchronously to the focused pane
    QKeyEvent release(QEvent::KeyRelease, k, m, text);
    QCoreApplication::sendEvent(g->window, &release);
}

void typeText(const QString &s)
{
    for (const QChar c : s) {
        // Qt's key codes for ASCII are the upper-case characters; anything else is Key_unknown + text.
        const ushort u = c.unicode();
        const int k = u < 128 ? int(QChar(u).toUpper().unicode()) : int(Qt::Key_unknown);
        key(k, c.isUpper() ? Qt::ShiftModifier : Qt::NoModifier, QString(c));
    }
}

void focusPane(int pane)
{
    for (int w : g->tabs->tmuxWindows())
        if (TerminalPanes *p = g->tabs->tmuxTab(w, false)) {
            const QList<TerminalView *> vs = p->views();
            for (int i = 0; i < vs.size(); ++i)
                if (vs[i]->tmuxPane() == pane)
                    p->focusLeafByIndex(i);
        }
    QCoreApplication::processEvents();
}

QStringList logLines()
{
    return readFile(qEnvironmentVariable("FAKE_TMUX_LOG")).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

// The bytes of every "send-keys -t '%P' -H …" line from `from` on, concatenated; `maxChunk` gets the
// largest per-command payload.
QByteArray hexBytes(const QStringList &lines, int from, int pane, int *maxChunk = nullptr)
{
    const QString head = QStringLiteral("send-keys -t '%%1' -H ").arg(pane);
    QByteArray out;
    for (int i = from; i < lines.size(); ++i) {
        if (!lines[i].startsWith(head))
            continue;
        const QStringList hex = lines[i].mid(head.size()).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (maxChunk)
            *maxChunk = qMax(*maxChunk, int(hex.size()));
        for (const QString &h : hex)
            out.append(char(h.toInt(nullptr, 16)));
    }
    return out;
}

void inputEnd()
{
    closeAndDetach(finishTest);
}

void inputLazySelect()
{
    // tmux's active pane is %1. Focusing %2 sends nothing; the first input to it is preceded by ONE
    // select-pane (2.4: lazily, so focus-follows-mouse does not send one per hover); focusing %1
    // again without typing sends nothing either.
    const int mark = int(logLines().size());
    focusPane(2);
    focusPane(1);
    focusPane(2);
    typeText(QStringLiteral("ab"));
    QCoreApplication::processEvents();
    typeText(QStringLiteral("c"));
    QCoreApplication::processEvents();
    const QStringList want = {
        QStringLiteral("select-pane -t '%2'"),
        QStringLiteral("send-keys -t '%2' -l -- 'ab'"),
        QStringLiteral("send-keys -t '%2' -l -- 'c'"),
    };
    waitFor([mark, want] { return logLines().size() >= mark + want.size(); }, 3000, QStringLiteral("typing into %2 reached tmux"), [mark, want] {
        QStringList got;
        for (const QString &l : logLines().mid(mark))
            if (!l.startsWith(QLatin1String("send-keys -t '%1' -H "))) // %1's focus out/in reports
                got.append(l);
        check(got == want, QStringLiteral("select-pane goes out once, lazily, before the first input to a pane tmux does not have active"));
        if (got != want)
            qWarning() << got;
        inputEnd();
    });
}

void inputQueries()
{
    const int mark = int(logLines().size());
    g->session->sendTestStep(QStringLiteral("step query"));
    waitFor([] { return dump(2).contains(QStringLiteral("queries-done")); }, 5000, QStringLiteral("the query output reached %2"), [mark] {
        QCoreApplication::processEvents();
        QTimer::singleShot(300, qApp, [mark] {
            const QStringList lines = logLines();
            check(hexBytes(lines, mark, 2).isEmpty(),
                  QStringLiteral("DA/DSR/DECRQM/OSC 11 queries in the output were NOT answered by SolidTerm (tmux answers them)"));
            inputLazySelect();
        });
    });
}

void inputFocus()
{
    g->session->sendTestStep(QStringLiteral("step focus"));
    waitFor([] { return dump(1).contains(QStringLiteral("focus-on")); }, 5000, QStringLiteral("%1 turned focus reporting on"), [] {
        const int mark = int(logLines().size());
        focusPane(2);
        focusPane(1);
        waitFor([mark] { return hexBytes(logLines(), mark, 1) == QByteArray("\x1b[O\x1b[I"); }, 3000,
                QStringLiteral("focus out / in of %1 went as send-keys -H 1b 5b 4f / 1b 5b 49"), inputQueries);
    });
}

void inputMouse()
{
    TerminalView *v = view(1);
    const int mark = int(logLines().size());
    // Cell (row 2, col 5) → SGR ESC[<b;6;3M. Real input through the window, as in other self-tests.
    const QPointF p = v->mapToScene(v->debugCellCenter(2, 5));
    const QPointF gp = g->window->mapToGlobal(p.toPoint());
    QWindowSystemInterface::handleMouseEvent(g->window, p, gp, Qt::LeftButton, Qt::LeftButton, QEvent::MouseButtonPress, Qt::NoModifier);
    QWindowSystemInterface::handleMouseEvent(g->window, p, gp, Qt::NoButton, Qt::LeftButton, QEvent::MouseButtonRelease, Qt::NoModifier);
    QWindowSystemInterface::handleWheelEvent(g->window, p, gp, QPoint(0, 0), QPoint(0, 120));
    QCoreApplication::processEvents();
    const QByteArray want("\x1b[<0;6;3M\x1b[<0;6;3m\x1b[<64;6;3M");
    waitFor([mark, want] { return hexBytes(logLines(), mark, 1) == want; }, 3000,
            QStringLiteral("click and wheel in a mouse-reporting pane went as SGR reports via send-keys -H"), [mark, want] {
                const QByteArray got = hexBytes(logLines(), mark, 1);
                if (got != want)
                    qWarning() << "mouse bytes" << got;
                inputFocus();
            });
}

void inputPaste()
{
    // 100 KB with newlines and UTF-8; %1 has bracketed paste on → wrapped; ≤ 1 KiB per command.
    QString text;
    for (int i = 0; text.size() < 100000; ++i)
        text += QStringLiteral("paste line %1 café 'q' \"d\" $x;\n").arg(i);
    const int mark = int(logLines().size());
    view(1)->pasteText(text);
    const QByteArray want = QByteArray("\x1b[200~") + text.toUtf8() + QByteArray("\x1b[201~");
    waitFor([mark, want] { return hexBytes(logLines(), mark, 1).size() >= want.size(); }, 10000,
            QStringLiteral("a 100 KB paste reached tmux"), [mark, want] {
                int maxChunk = 0;
                const QByteArray got = hexBytes(logLines(), mark, 1, &maxChunk);
                check(got == want, QStringLiteral("the paste arrived byte-exact, bracketed (ESC[200~ … ESC[201~)"));
                check(maxChunk <= 1024 && maxChunk > 0, QStringLiteral("each send-keys -H carries at most 1 KiB (largest: %1)").arg(maxChunk));
                inputMouse();
            });
}

void inputKeys()
{
    focusPane(1);
    check(view(1) && view(1)->hasActiveFocus(), QStringLiteral("pane %1 has keyboard focus"));
    const int mark = int(logLines().size());
    typeText(QStringLiteral("ls -la"));
    key(Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
    key(Qt::Key_Up, Qt::NoModifier);
    key(Qt::Key_Home, Qt::NoModifier);
    key(Qt::Key_F5, Qt::ShiftModifier);
    key(Qt::Key_Left, Qt::ControlModifier);
    key(Qt::Key_X, Qt::AltModifier, QStringLiteral("x"));
    key(Qt::Key_C, Qt::ControlModifier, QStringLiteral("\x03"));
    key(Qt::Key_Backspace, Qt::ControlModifier, QStringLiteral("\x08"));
    key(Qt::Key_Backtab, Qt::ShiftModifier);
    key(Qt::Key_PageDown, Qt::NoModifier);
    key(Qt::Key_Escape, Qt::NoModifier, QStringLiteral("\x1b"));
    typeText(QStringLiteral("it's -x é"));
    {
        QInputMethodEvent ime;
        ime.setCommitString(QStringLiteral("日本"));
        QCoreApplication::sendEvent(view(1), &ime);
    }
    QCoreApplication::processEvents(); // the coalesced literal is flushed on the next turn
    const QStringList want = {
        QStringLiteral("send-keys -t '%1' -l -- 'ls -la'"),
        QStringLiteral("send-keys -t '%1' 'Enter'"),
        QStringLiteral("send-keys -t '%1' 'Up'"),
        QStringLiteral("send-keys -t '%1' 'Home'"),
        QStringLiteral("send-keys -t '%1' 'S-F5'"),
        QStringLiteral("send-keys -t '%1' 'C-Left'"),
        QStringLiteral("send-keys -t '%1' 'M-x'"),
        QStringLiteral("send-keys -t '%1' 'C-c'"),
        QStringLiteral("send-keys -t '%1' 'C-h'"),
        QStringLiteral("send-keys -t '%1' 'BTab'"),
        QStringLiteral("send-keys -t '%1' 'NPage'"),
        QStringLiteral("send-keys -t '%1' 'Escape'"),
        QStringLiteral("send-keys -t '%1' -l -- 'it'\"'\"'s -x é日本'"),
    };
    waitFor([mark, want] { return logLines().size() >= mark + want.size(); }, 3000, QStringLiteral("the key corpus reached tmux"), [mark, want] {
        const QStringList got = logLines().mid(mark, want.size());
        check(got == want, QStringLiteral("key corpus: exact command lines (names, coalesced -l, quoting, IME commit)"));
        if (got != want)
            for (int i = 0; i < qMax(got.size(), want.size()); ++i)
                qWarning().noquote() << (got.value(i) == want.value(i) ? "   " : "!! ") << got.value(i) << "| want" << want.value(i);
        inputPaste();
    });
}

void inputStart()
{
    waitFor([] { return g->tabs && g->tabs->count() == 1 && g->session && g->session->seedsCompleted() == 2 && view(1) && view(2); },
            10000, QStringLiteral("input: attached, two panes seeded"), [] {
        if (!view(1)) {
            finishTest();
            return;
        }
        check(!view(1)->readOnly(), QStringLiteral("panes accept input by default"));
        // OSC 10/11: each pane reports its colours (tmux answers programs' queries with them).
        waitFor([] {
            const QString log = readFile(qEnvironmentVariable("FAKE_TMUX_LOG"));
            for (int pane : { 1, 2 })
                for (const char *osc : { "10", "11" })
                    if (!log.contains(QStringLiteral("refresh-client -r '%") + QString::number(pane) + QStringLiteral(":\x1b]")
                                      + QLatin1String(osc) + QStringLiteral(";rgb:")))
                        return false;
            return true;
        }, 3000, QStringLiteral("OSC 10 and 11 colours reported for both panes (refresh-client -r '%P:ESC]1x;rgb:…ESC\\')"), inputKeys);
    });
}

// ─── input against a real tmux ──────────────────────────────────────────────────────────────────

TerminalView *realShell()
{
    const QList<TerminalView *> v = viewsOf(g->tabs->tmuxWindows().value(0));
    return v.value(0);
}

void realInputFail()
{
    if (g->failures && realShell())
        qWarning().noquote() << "shell pane:\n" << realShell()->debugDump();
}

void realInputStart()
{
    waitFor([] { return g->tabs && g->tabs->count() >= 1 && realShell() && realShell()->debugDump().contains(QStringLiteral("$")); },
            10000, QStringLiteral("real-input: attached, a shell prompt"), [] {
        TerminalView *v = realShell();
        if (!v) {
            finishTest();
            return;
        }
        // tmux made the other pane active (the last split): pick ours. The window's own initial
        // focus pass is queued and may still land after the attach, so insist until it holds.
        auto *stable = new int(0);
        waitFor([stable] {
            TerminalView *v = realShell();
            if (!v->hasActiveFocus()) {
                *stable = 0;
                focusPane(v->tmuxPane());
                return false;
            }
            return ++*stable >= 4; // 100 ms in a row
        }, 5000, QStringLiteral("real-input: the shell pane has keyboard focus"), [stable] {
        delete stable;
        typeText(QStringLiteral("echo typed-$((6*7))"));
        key(Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
        waitFor([] { return realShell()->debugDump().contains(QStringLiteral("\ntyped-42\n")); }, 5000,
                QStringLiteral("real-input: keys typed in SolidTerm ran in the tmux shell (typed-42 came back)"), [] {
            const QString script = qEnvironmentVariable("SOLIDTERM_TMUXTEST_MOUSEECHO");
            typeText(QStringLiteral("python3 ") + script);
            key(Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
            waitFor([] { return realShell()->debugDump().contains(QStringLiteral("READY")) && realShell()->debugDump().contains(QStringLiteral("alt 1")); },
                    5000, QStringLiteral("real-input: a vim/less-style program (alt screen, SGR mouse, bracketed paste) is up"), [] {
                TerminalView *v = realShell();
                const QPointF p = v->mapToScene(v->debugCellCenter(4, 7));
                const QPointF gp = g->window->mapToGlobal(p.toPoint());
                QWindowSystemInterface::handleMouseEvent(g->window, p, gp, Qt::LeftButton, Qt::LeftButton, QEvent::MouseButtonPress, Qt::NoModifier);
                QWindowSystemInterface::handleMouseEvent(g->window, p, gp, Qt::NoButton, Qt::LeftButton, QEvent::MouseButtonRelease, Qt::NoModifier);
                QWindowSystemInterface::handleWheelEvent(g->window, p, gp, QPoint(0, 0), QPoint(0, -120));
                QCoreApplication::processEvents();
                waitFor([] {
                    const QString d = realShell()->debugDump();
                    return d.contains(QStringLiteral("\\x1b[<0;8;5M")) && d.contains(QStringLiteral("\\x1b[<0;8;5m")) && d.contains(QStringLiteral("\\x1b[<65;8;5M"));
                }, 5000, QStringLiteral("real-input: click and wheel reached the program as SGR mouse reports"), [] {
                    realShell()->pasteText(QStringLiteral("pasted é"));
                    waitFor([] { return realShell()->debugDump().contains(QStringLiteral("\\x1b[200~pasted \\xc3\\xa9\\x1b[201~")); }, 5000,
                            QStringLiteral("real-input: a paste reached the program bracketed"), [] {
                        typeText(QStringLiteral("q"));
                        waitFor([] { return realShell()->debugDump().contains(QStringLiteral("alt 0")); }, 5000,
                                QStringLiteral("real-input: the program quit (back on the normal screen)"), [] {
                            realInputFail();
                            closeAndDetach(finishTest);
                        });
                    });
                });
            });
        });
        });
    });
}

// ─── structure (phase 3) ────────────────────────────────────────────────────────────────────────

int windowOf(int pane)
{
    for (int w : g->tabs->tmuxWindows())
        if (TerminalPanes *p = g->tabs->tmuxTab(w, false))
            if (p->tmuxView(pane))
                return w;
    return -1;
}

QList<int> panesOf(int window)
{
    QList<int> out;
    TerminalPanes *p = g->tabs->tmuxTab(window, false);
    if (p)
        for (TerminalView *v : p->views())
            out.append(v->tmuxPane());
    return out;
}

int leafIndex(int pane)
{
    return panesOf(windowOf(pane)).indexOf(pane);
}

bool logHas(int from, const QString &line)
{
    return logLines().mid(from).contains(line);
}

// Each step: run `act`, wait for `line` in the fake's log and for `cond`, check, then `next`.
void stepCmd(const QString &what, std::function<void()> act, const QString &line, std::function<bool()> cond,
             std::function<void()> next)
{
    const int mark = int(logLines().size());
    act();
    waitFor([mark, line, cond] { return (line.isEmpty() || logHas(mark, line)) && cond(); }, 5000, what, [mark, line, cond, next] {
        if (!((line.isEmpty() || logHas(mark, line)) && cond())) { // say what the model looks like
            QStringList state;
            for (int w : g->tabs->tmuxWindows()) {
                QStringList ps;
                for (int p : panesOf(w))
                    ps << QString::number(p);
                state << QStringLiteral("@%1%2[%3]").arg(w).arg(g->tabs->tmuxTabHidden(w) ? QStringLiteral("(hidden)") : QString()).arg(ps.join(QLatin1Char(',')));
            }
            qWarning().noquote() << "  model:" << state.join(QLatin1Char(' ')) << "active @" << g->tabs->activeTmuxWindow()
                                 << "hidden" << g->tabs->hiddenPanes().size() << "| sent since:" << logLines().mid(mark).filter(QStringLiteral("-")).join(QStringLiteral(" / ")).left(400);
        }
        next();
    });
}

QPointer<TerminalView> g_moved; // a view that must survive moves between windows

void structEnd()
{
    // Hiding the last visible tab: the OS window closes = detach; the session lives on.
    const int mark = int(logLines().size());
    const int visible = g->tabs->count();
    for (int i = visible - 1; i >= 0; --i)
        g->tabs->closeTab(i);
    waitFor([] { return g->session.isNull(); }, 5000,
            QStringLiteral("hiding every tab closes the window, which detaches (no kill sent)"), [mark] {
        const QStringList lines = logLines().mid(mark);
        check(lines.contains(QStringLiteral("detach-client")) && !lines.join(QLatin1Char('\n')).contains(QLatin1String("kill-")),
              QStringLiteral("the log shows detach-client and no kill"));
        finishTest();
    });
}

void structDcs()
{
    stepCmd(QStringLiteral("DCS tmux; passthrough is unwrapped: the inner OSC 2 set the pane title"),
            [] { g->session->sendTestStep(QStringLiteral("step dcs")); }, QString(),
            [] { return view(1) && view(1)->title() == QStringLiteral("from-passthrough") && dump(1).contains(QStringLiteral("after-dcs")); },
            [] {
                if (view(1) && view(1)->title() != QStringLiteral("from-passthrough"))
                    qWarning().noquote() << "  %1 title:" << view(1)->title() << "\n" << dump(1).right(400);
                check(!dump(1).contains(QStringLiteral("tmux;")) && !dump(1).contains(QStringLiteral("from-passthrough")),
                      QStringLiteral("no passthrough bytes leaked onto the screen"));
                structEnd();
            });
}

void structMode()
{
    stepCmd(QStringLiteral("%pane-mode-changed → the header shows a copy-mode badge"),
            [] { g->session->sendTestStep(QStringLiteral("step mode")); }, QString(),
            [] { return geomOf(1).value(QStringLiteral("badge")).toString().contains(QStringLiteral("copy-mode")); }, [] {
        focusPane(1);
        stepCmd(QStringLiteral("Leave tmux mode → send-keys -X cancel → the badge goes"),
                [] { g->tabs->leaveTmuxModeFocused(); }, QStringLiteral("send-keys -X -t '%1' 'cancel'"),
                [] { return geomOf(1).value(QStringLiteral("badge")).toString().isEmpty(); }, structDcs);
    });
}

void structKill()
{
    // An explicit kill: kill-pane / kill-window, and the pane/tab is gone (not hidden).
    int victim = -1;
    for (int p : panesOf(windowOf(1)))
        if (p != 1 && p != 2)
            victim = p;
    focusPane(victim);
    stepCmd(QStringLiteral("Kill pane → kill-pane; the pane is gone, not hidden"),
            [] { g->tabs->killFocused(); }, QStringLiteral("kill-pane -t '%%1'").arg(victim),
            [victim] { return !view(victim) && g->tabs->hiddenPanes().isEmpty(); }, [] {
        g->tabs->selectTab(1); // kill the OTHER tab (@2), keeping %1 for the next steps
        QCoreApplication::processEvents();
        const int tabsBefore = g->tabs->count();
        const int w = g->tabs->activeTmuxWindow();
        stepCmd(QStringLiteral("Kill tab → kill-window; the tab is gone"),
                [] { g->tabs->killTab(); }, QStringLiteral("kill-window -t '@%1'").arg(w),
                [tabsBefore, w] { return g->tabs->count() == tabsBefore - 1 && !g->tabs->tmuxWindows().contains(w); }, structMode);
    });
}

void structHideTab()
{
    const int tabsBefore = g->tabs->count();
    const int mark = int(logLines().size());
    g->tabs->closeTab(tabsBefore - 1); // the last tab (@2)
    QCoreApplication::processEvents();
    check(g->tabs->count() == tabsBefore - 1 && g->tabs->hiddenPanes().size() == 1,
          QStringLiteral("closing a tab hides it (one hidden entry)"));
    check(!logLines().mid(mark).join(QLatin1Char('\n')).contains(QLatin1String("kill")), QStringLiteral("hiding a tab sends no kill"));
    const int w = g->tabs->hiddenPanes().value(0).toMap().value(QStringLiteral("window")).toInt();
    g->tabs->restoreHidden(w);
    QCoreApplication::processEvents();
    check(g->tabs->count() == tabsBefore && g->tabs->hiddenPanes().isEmpty() && g->tabs->activeTmuxWindow() == w,
          QStringLiteral("Show hidden brings the tab back, selected"));
    g->tabs->selectTab(0);
    QCoreApplication::processEvents();
    structKill();
}

void structHidePane()
{
    // Close (= hide) the moved pane: break-pane -d into a hidden window; the view survives.
    const int pane = g_moved ? g_moved->tmuxPane() : -1;
    const int origin = windowOf(pane);
    const int tabsBefore = g->tabs->count();
    focusPane(pane);
    stepCmd(QStringLiteral("Hide pane → break-pane -d; it leaves the window into a hidden tab"),
            [] { g->tabs->closeFocused(); }, QStringLiteral("break-pane -d -s '%%1'").arg(pane),
            [pane, origin, tabsBefore] {
                return windowOf(pane) >= 0 && windowOf(pane) != origin && g->tabs->count() == tabsBefore
                    && g->tabs->hiddenPanes().size() == 1;
            }, [pane, origin] {
        check(view(pane) == g_moved.data(), QStringLiteral("the hidden pane keeps its view (content, scrollback)"));
        const int hw = windowOf(pane);
        stepCmd(QStringLiteral("Show hidden → join-pane back into its window, same view"),
                [hw] { g->tabs->restoreHidden(hw); }, QStringLiteral("join-pane -s '%%1' -t '@%2'").arg(pane).arg(origin),
                [pane, origin] { return windowOf(pane) == origin && g->tabs->hiddenPanes().isEmpty(); }, [pane] {
                    check(view(pane) == g_moved.data() && dump(pane).contains(QStringLiteral("-marker")),
                          QStringLiteral("rejoined with the same view and its output"));
                    structHideTab();
                });
    });
}

void structNewTab()
{
    const int tabsBefore = g->tabs->count();
    stepCmd(QStringLiteral("New tab → new-window -a; the tab appears and is selected"),
            [] { g->tabs->newTab(); }, QStringLiteral("new-window -a -t '@1' -c '#{pane_current_path}'"),
            [tabsBefore] { return g->tabs->count() == tabsBefore + 1 && g->tabs->activeTmuxWindow() == g->tabs->tmuxWindows().last(); }, [] {
        const int created = g->tabs->tmuxWindows().last();
        stepCmd(QStringLiteral("Selecting tab 0 → select-window, and the echo does not switch back"),
                [] { g->tabs->selectTab(0); }, QStringLiteral("select-window -t '@1'"),
                [] { return g->tabs->activeTmuxWindow() == 1; }, [created] {
            QTimer::singleShot(300, qApp, [created] {
                check(g->tabs->activeTmuxWindow() == 1, QStringLiteral("still on @1 after the echo"));
                const int w = created;
                stepCmd(QStringLiteral("…and the new window is killed"), [w] { g->session->killWindow(w); }, QString(),
                        [w] { return !g->tabs->tmuxWindows().contains(w); }, structHidePane);
            });
        });
    });
}

void structResize()
{
    // The tree is {%2, [%1, %n]} now: drag the divider after %2 by +5 cells → resize-pane -x.
    const int cols = gi(2, "cols");
    const qreal cw = geomOf(2).value(QStringLiteral("cellW")).toReal();
    stepCmd(QStringLiteral("Divider drag → resize-pane -x; tmux's new layout is applied"),
            [cw] { g->tabs->tmuxTab(1, false)->debugTmuxDragDivider(2, 5 * cw); },
            QStringLiteral("resize-pane -t '%2' -x %1").arg(cols + 5),
            [cols] { return gi(2, "cols") == cols + 5; }, structNewTab);
}

void structZoom()
{
    focusPane(1);
    stepCmd(QStringLiteral("Zoom → resize-pane -Z; only %1 is shown, full size"),
            [] { g->tabs->toggleZoom(); }, QStringLiteral("resize-pane -Z -t '%1'"),
            [] {
                for (int p : panesOf(1))
                    if (geomOf(p).value(QStringLiteral("visible")).toBool() != (p == 1))
                        return false;
                return true;
            }, [] {
        stepCmd(QStringLiteral("Zoom again → back to the split"),
                [] { g->tabs->toggleZoom(); }, QString(),
                [] {
                    for (int p : panesOf(1))
                        if (!geomOf(p).value(QStringLiteral("visible")).toBool())
                            return false;
                    return true;
                }, structResize);
    });
}

void structDrag()
{
    const int n = g_moved ? g_moved->tmuxPane() : -1;
    // Drag the new pane to the BOTTOM of %2: move-pane -s %n -t %2 -v (the view rides along).
    stepCmd(QStringLiteral("Drop a pane on another's bottom → move-pane -v; same view after the move"),
            [n] { g->tabs->tmuxTab(1, false)->debugDragLeaf(leafIndex(n), leafIndex(2), 5 /*ZoneBottom*/); },
            QStringLiteral("move-pane -s '%%1' -t '%2' -v").arg(n),
            [n] { return geomOf(n).value(QStringLiteral("cx")) == geomOf(2).value(QStringLiteral("cx"))
                         && gi(n, "cy") > gi(2, "cy"); }, [n] {
        check(view(n) == g_moved.data() && dump(n).contains(QStringLiteral("pane-%1-marker").arg(n)),
              QStringLiteral("the moved pane kept its view and output"));
        // Centre drop of %1 onto %2: swap-pane.
        const int x1 = gi(1, "cx"), x2 = gi(2, "cx");
        stepCmd(QStringLiteral("Drop a pane on another's centre → swap-pane"),
                [] { g->tabs->tmuxTab(1, false)->debugDragLeaf(leafIndex(1), leafIndex(2), 1 /*ZoneCenter*/); },
                QStringLiteral("swap-pane -s '%1' -t '%2'"),
                [x1, x2] { return gi(1, "cx") == x2 && gi(2, "cx") == x1; }, structZoom);
    });
}

void structSplit()
{
    focusPane(1);
    const QList<int> before = panesOf(1);
    stepCmd(QStringLiteral("Split right → split-window -h; the new pane arrives with the layout"),
            [] { g->tabs->split(Qt::Horizontal); }, QStringLiteral("split-window -h -t '%1' -c '#{pane_current_path}'"),
            [before] { return panesOf(1).size() == before.size() + 1; }, [before] {
        int n = -1;
        for (int p : panesOf(1))
            if (!before.contains(p))
                n = p;
        QString why;
        check(geometryOk(g->tabs->tmuxTab(1, false), &why), QStringLiteral("the split is cell-exact ") + why);
        g->session->sendTestStep(QStringLiteral("step mark"));
        waitFor([n] { return dump(n).contains(QStringLiteral("pane-%1-marker").arg(n)); }, 3000,
                QStringLiteral("the new pane shows its output"), [n] {
            g_moved = view(n);
            structDrag();
        });
    });
}

void structStart()
{
    waitFor([] { return g->tabs && g->tabs->count() == 2 && g->session && g->session->seedsCompleted() == 3 && view(1) && view(3); },
            10000, QStringLiteral("struct: attached, two tabs, three panes"), [] {
        if (!view(1)) {
            finishTest();
            return;
        }
        structSplit();
    });
}

// ─── structure against a real tmux ──────────────────────────────────────────────────────────────

void realStructEnd()
{
    closeAndDetach(finishTest);
}

void realStructMode()
{
    tmuxCmd({ QStringLiteral("copy-mode"), QStringLiteral("-t"), QStringLiteral("%0") });
    waitFor([] { return geomOf(0).value(QStringLiteral("badge")).toString().contains(QStringLiteral("copy-mode")); }, 5000,
            QStringLiteral("real-struct: copy-mode entered from outside shows the header badge"), [] {
        focusPane(0);
        g->tabs->leaveTmuxModeFocused();
        waitFor([] { return geomOf(0).value(QStringLiteral("badge")).toString().isEmpty(); }, 5000,
                QStringLiteral("real-struct: Leave tmux mode (send-keys -X cancel) clears it"), realStructEnd);
    });
}

void realStructDcs()
{
    tmuxCmd({ QStringLiteral("send-keys"), QStringLiteral("-t"), QStringLiteral("%0"),
              QStringLiteral("printf '\\033Ptmux;\\033\\033]2;passthru-title\\007\\033\\\\'; echo dcs-done"), QStringLiteral("Enter") });
    waitFor([] { return view(0) && view(0)->title() == QStringLiteral("passthru-title"); }, 5000,
            QStringLiteral("real-struct: a program's DCS tmux; passthrough is unwrapped (inner OSC 2 → title)"), realStructMode);
}

void realStructTab()
{
    const int before = g->tabs->count();
    g->tabs->newTab();
    waitFor([before] { return g->tabs->count() == before + 1 && g->tabs->activeTmuxWindow() == g->tabs->tmuxWindows().last(); }, 5000,
            QStringLiteral("real-struct: New tab → new-window; the tab appears selected"), [before] {
        g->tabs->killTab();
        waitFor([before] { return g->tabs->count() == before; }, 5000, QStringLiteral("real-struct: Kill tab → kill-window"), [] {
            g->tabs->selectTab(0);
            realStructDcs();
        });
    });
}

void realStructResize()
{
    const int w0 = g->tabs->tmuxWindows().value(0);
    const int cols = gi(0, "cols");
    const qreal cw = geomOf(0).value(QStringLiteral("cellW")).toReal();
    g->tabs->tmuxTab(w0, false)->debugTmuxDragDivider(0, 4 * cw);
    waitFor([cols] { return gi(0, "cols") == cols + 4; }, 5000,
            QStringLiteral("real-struct: a divider drag resizes the pane by 4 columns (resize-pane -x)"), [] {
        QString why;
        check(geometryOk(g->tabs->tmuxTab(g->tabs->tmuxWindows().value(0), false), &why), QStringLiteral("real-struct: still cell-exact ") + why);
        realStructTab();
    });
}

void realStructZoom()
{
    focusPane(0);
    g->tabs->toggleZoom();
    waitFor([] {
        for (int p : panesOf(g->tabs->tmuxWindows().value(0)))
            if (geomOf(p).value(QStringLiteral("visible")).toBool() != (p == 0))
                return false;
        return true;
    }, 5000, QStringLiteral("real-struct: zoom (resize-pane -Z) shows only that pane"), [] {
        g->tabs->toggleZoom();
        waitFor([] {
            for (int p : panesOf(g->tabs->tmuxWindows().value(0)))
                if (!geomOf(p).value(QStringLiteral("visible")).toBool())
                    return false;
            return true;
        }, 5000, QStringLiteral("real-struct: unzoom"), realStructResize);
    });
}

void realStructHide(int n)
{
    const int w0 = g->tabs->tmuxWindows().value(0);
    focusPane(n);
    g->tabs->closeFocused();
    waitFor([n, w0] { return windowOf(n) != w0 && g->tabs->hiddenPanes().size() == 1 && g->tabs->count() == 2; }, 5000,
            QStringLiteral("real-struct: Hide pane → break-pane -d into a hidden window, the tab count unchanged"), [n, w0] {
        check(view(n) == g_moved.data(), QStringLiteral("real-struct: the hidden pane kept its view"));
        g->tabs->restoreHidden(windowOf(n));
        waitFor([n, w0] { return windowOf(n) == w0 && g->tabs->hiddenPanes().isEmpty(); }, 5000,
                QStringLiteral("real-struct: Show hidden → join-pane, back in its window"), [n] {
            check(view(n) == g_moved.data(), QStringLiteral("real-struct: back with the same view"));
            check(view(n) && view(n)->debugDump().contains(QStringLiteral("split-marker")),
                  QStringLiteral("real-struct: its output intact"));
            if (!view(n) || !view(n)->debugDump().contains(QStringLiteral("split-marker")))
                qWarning().noquote() << (view(n) ? view(n)->debugDump() : QStringLiteral("<no view>"));
            // Then kill it explicitly.
            focusPane(n);
            g->tabs->killFocused();
            waitFor([n] { return !view(n); }, 5000, QStringLiteral("real-struct: Kill pane → kill-pane"), realStructZoom);
        });
    });
}

void realStructStart()
{
    waitFor([] { return g->tabs && g->tabs->count() == 2 && g->session && g->session->seedsCompleted() >= 3 && view(0) && view(1); },
            10000, QStringLiteral("real-struct: attached"), [] {
        if (!view(0)) {
            finishTest();
            return;
        }
        const int w0 = g->tabs->tmuxWindows().value(0);
        const QList<int> before = panesOf(w0);
        focusPane(1);
        g->tabs->split(Qt::Vertical);
        waitFor([w0, before] { return panesOf(w0).size() == before.size() + 1; }, 5000,
                QStringLiteral("real-struct: Split down → split-window -v; the new pane comes with tmux's layout"), [w0, before] {
            int n = -1;
            for (int p : panesOf(w0))
                if (!before.contains(p))
                    n = p;
            QString why;
            check(geometryOk(g->tabs->tmuxTab(w0, false), &why), QStringLiteral("real-struct: cell-exact ") + why);
            g_moved = view(n);
            tmuxCmd({ QStringLiteral("send-keys"), QStringLiteral("-t"), QStringLiteral("%%1").arg(n),
                      QStringLiteral("printf 'split-%s\\n' marker"), QStringLiteral("Enter") });
            waitFor([n] { return view(n) && view(n)->debugDump().contains(QStringLiteral("split-marker")); }, 5000,
                    QStringLiteral("real-struct: the new pane runs a shell"), [n] { realStructHide(n); });
        });
    });
}

// ─── lifecycle (phase 4) ────────────────────────────────────────────────────────────────────────

QQuickWindow *otherWindow()
{
    for (QWindow *w : QGuiApplication::topLevelWindows())
        if (auto *qw = qobject_cast<QQuickWindow *>(w); qw && qw != g->window && qw->isVisible())
            return qw;
    return nullptr;
}

TerminalTabs *tabsOf(QQuickWindow *w)
{
    QList<QQuickItem *> stack { w->contentItem() };
    while (!stack.isEmpty()) {
        QQuickItem *it = stack.takeLast();
        if (auto *t = qobject_cast<TerminalTabs *>(it))
            return t;
        for (QQuickItem *k : it->childItems())
            stack.append(k);
    }
    return nullptr;
}

int countLines(const QString &prefix)
{
    int n = 0;
    for (const QString &l : logLines())
        n += l.startsWith(prefix) ? 1 : 0;
    return n;
}

void lifeKill()
{
    // Preference "closing a tmux window kills the session": kill-session, then the client ends.
    TmuxSession::config()->set(QStringLiteral("tmuxCloseAction"), QStringLiteral("kill"));
    closeAndDetach([] {
        check(logLines().join(QLatin1Char('\n')).contains(QLatin1String("kill-session -t '$1'")),
              QStringLiteral("with tmuxCloseAction=kill, closing the window sends kill-session"));
        TmuxSession::config()->set(QStringLiteral("tmuxCloseAction"), QStringLiteral("detach")); // the shared test config
        TmuxSession::config()->set(QStringLiteral("tmuxPauseAfter"), 5);
        finishTest();
    });
}

void lifeServerExit()
{
    g->session->sendTestStep(QStringLiteral("step serverexit"));
    waitFor([] { return !g->banner.isEmpty(); }, 5000, QStringLiteral("the server exiting shows a banner"), [] {
        check(g->banner.contains(QStringLiteral("server exited unexpectedly")) && g->bannerReattach,
              QStringLiteral("…naming the reason, offering Reattach: ") + g->banner);
        check(g->window && g->window->isVisible() && g->tabs->count() == 1, QStringLiteral("the window and its tab stay"));
        check(view(1) && view(1)->readOnly() && dump(1).contains(QStringLiteral("life-marker")),
              QStringLiteral("the pane keeps its content, read-only"));
        const int argvBefore = countLines(QStringLiteral("# argv:"));
        g->tabs->tmuxReattach();
        waitFor([argvBefore] {
            return countLines(QStringLiteral("# argv:")) == argvBefore + 1 && g->banner.isEmpty() && g->tabs->tmuxSession()
                && g->tabs->tmuxSession()->seedsCompleted() == 1 && view(1) && !view(1)->readOnly();
        }, 8000, QStringLiteral("Reattach: a new control client, the banner goes, the pane is live again"), [] {
            g->session = g->tabs->tmuxSession();
            lifeKill();
        });
    });
}

void lifePrefs()
{
    const int mark = int(logLines().size());
    TmuxSession::config()->set(QStringLiteral("tmuxPauseAfter"), 1);
    waitFor([mark] { return logHas(mark, QStringLiteral("refresh-client -f pause-after=1")); }, 3000,
            QStringLiteral("changing the pause-after preference applies live (refresh-client -f pause-after=1)"), lifeServerExit);
}

void lifePicker()
{
    g->tabs->refreshTmuxSessions();
    waitFor([] { return g->sessions.size() == 2; }, 5000, QStringLiteral("the picker lists the server's sessions (list-sessions)"), [] {
        const QVariantMap a = g->sessions.value(0).toMap(), b = g->sessions.value(1).toMap();
        check(a.value(QStringLiteral("name")) == QStringLiteral("other") && a.value(QStringLiteral("ours")).toBool()
                  && b.value(QStringLiteral("name")) == QStringLiteral("third") && !b.value(QStringLiteral("ours")).toBool(),
              QStringLiteral("…with SolidTerm's own (@solidterm) marked"));
        g->tabs->openTmuxSession(QStringLiteral("other"));
        waitFor([] {
            QQuickWindow *w = otherWindow();
            TerminalTabs *t = w ? tabsOf(w) : nullptr;
            return t && t->count() == 1 && logLines().contains(QStringLiteral("# argv: -C attach-session -t =other"));
        }, 8000, QStringLiteral("Attach from the picker opens a new window on that session (attach-session -t =other)"), [] {
            const int windows = int(QGuiApplication::topLevelWindows().size());
            g->tabs->openTmuxSession(QStringLiteral("other"));
            QTimer::singleShot(500, qApp, [windows] {
                check(int(QGuiApplication::topLevelWindows().size()) == windows && countLines(QStringLiteral("# argv: -C attach-session")) == 1,
                      QStringLiteral("asking again raises that window instead of attaching twice"));
                QQuickWindow *w = otherWindow();
                if (w)
                    w->close();
                waitFor([] { return !otherWindow() && !TmuxSession::find(QStringLiteral("other")); }, 5000,
                        QStringLiteral("closing it detaches it"), lifePrefs);
            });
        });
    });
}

void lifeStart()
{
    waitFor([] { return g->tabs && g->tabs->count() == 1 && g->session && g->session->seedsCompleted() == 1 && view(1); },
            10000, QStringLiteral("life: attached"), [] {
        if (!view(1)) {
            finishTest();
            return;
        }
        waitFor([] { return logLines().contains(QStringLiteral("set-option -t '$1' @solidterm 1")); }, 3000,
                QStringLiteral("a session SolidTerm created is marked @solidterm (decision 3)"), lifePicker);
    });
}

void gateStart(const QString &want)
{
    waitFor([] { return !g->banner.isEmpty(); }, 8000, QStringLiteral("a banner explains why there is no tmux session"), [want] {
        check(g->banner.contains(want), QStringLiteral("…saying: ") + g->banner);
        check(!g->bannerReattach && g->tabs->count() == 0, QStringLiteral("…with no tabs and no Reattach"));
        check(countLines(QStringLiteral("# argv:")) == 0, QStringLiteral("no control client was started"));
        if (const QString shot = qEnvironmentVariable("SOLIDTERM_TMUXTEST_SHOT"); !shot.isEmpty())
            g->window->grabWindow().save(shot); // to look at the banner
        closeAndDetach(finishTest);
    });
}

// ─── lifecycle against a real tmux ──────────────────────────────────────────────────────────────

void realLifeStart()
{
    waitFor([] { return g->tabs && g->tabs->count() == 1 && g->session && g->session->seedsCompleted() == 1; }, 10000,
            QStringLiteral("real-life: attached to a session that did not exist (created)"), [] {
        waitFor([] {
            QProcess p;
            p.start(qEnvironmentVariable("SOLIDTERM_TMUX_BIN"),
                    { QStringLiteral("show-options"), QStringLiteral("-t"), QStringLiteral("created"), QStringLiteral("-v"), QStringLiteral("@solidterm") });
            p.waitForFinished(2000);
            return p.readAllStandardOutput().trimmed() == "1";
        }, 5000, QStringLiteral("real-life: it is marked @solidterm (SolidTerm created it)"), [] {
            tmuxCmd({ QStringLiteral("kill-server") });
            waitFor([] { return !g->banner.isEmpty(); }, 5000, QStringLiteral("real-life: kill-server shows the banner"), [] {
                check(g->bannerReattach && g->window->isVisible() && g->banner.contains(QStringLiteral("the tmux server is gone")),
                      QStringLiteral("real-life: …saying the server is gone, with Reattach; the window stays: ") + g->banner);
                closeAndDetach(finishTest);
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
    // A fresh fake-tmux log for this run (the fake appends: one-shot commands and control clients).
    if (const QString log = qEnvironmentVariable("FAKE_TMUX_LOG"); !log.isEmpty())
        QFile(log).resize(0);
    g->session = g->tabs->tmuxSession();
    g->session->setTee(true); // before the attach starts (it is queued): every byte, from the first
    QGuiApplication::setQuitOnLastWindowClosed(false); // the verdict comes after the window closes
    QObject::connect(g->tabs, &TerminalTabs::tmuxBanner, qApp, [](const QString &text, bool reattach) {
        g->banner = text;
        g->bannerReattach = reattach;
    });
    QObject::connect(g->tabs, &TerminalTabs::tmuxSessionsChanged, qApp, [](const QVariantList &list) { g->sessions = list; });
    QObject::connect(g->tabs, &TerminalTabs::tabsChanged, qApp, [](const QStringList &titles, int) {
        for (const QString &t : titles)
            if (!g->titlesSeen.contains(t))
                g->titlesSeen.append(t);
    });
    if (mode == QLatin1String("real"))
        QTimer::singleShot(0, qApp, realStart);
    else if (mode == QLatin1String("input"))
        QTimer::singleShot(0, qApp, inputStart);
    else if (mode == QLatin1String("life"))
        QTimer::singleShot(0, qApp, lifeStart);
    else if (mode == QLatin1String("oldversion"))
        QTimer::singleShot(0, qApp, [] { gateStart(QStringLiteral("too old: SolidTerm needs tmux 3.5")); });
    else if (mode == QLatin1String("nobinary"))
        QTimer::singleShot(0, qApp, [] { gateStart(QStringLiteral("tmux was not found")); });
    else if (mode == QLatin1String("real-life"))
        QTimer::singleShot(0, qApp, realLifeStart);
    else if (mode == QLatin1String("real-struct"))
        QTimer::singleShot(0, qApp, realStructStart);
    else if (mode == QLatin1String("struct"))
        QTimer::singleShot(0, qApp, structStart);
    else if (mode == QLatin1String("real-input"))
        QTimer::singleShot(0, qApp, realInputStart);
    else
        QTimer::singleShot(0, qApp, fakeStart);
}

} // namespace TmuxSelfTest
