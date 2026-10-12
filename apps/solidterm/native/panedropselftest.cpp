#include "panedropselftest.h"

#include "terminalpanes.h"
#include "terminaltabs.h"
#include "terminalview.h"

#include <QCoreApplication>
#include <QFile>
#include <QGuiApplication>
#include <QMimeData>
#include <QPointer>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QTimer>

namespace {

struct State {
    QString pidLog;
    QString shot;
    QPointer<QQuickWindow> winA;
    QPointer<TerminalTabs> tabsA;
    QPointer<TerminalView> view; // the pane that travels
    int failures = 0;
};

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

QList<QQuickWindow *> terminalWindows()
{
    QList<QQuickWindow *> out;
    for (QWindow *w : QGuiApplication::topLevelWindows())
        if (auto *qw = qobject_cast<QQuickWindow *>(w))
            if (qw->isVisible() && tabsOf(qw))
                out.append(qw);
    return out;
}

void check(State *s, bool ok, const char *what)
{
    qInfo("solidterm xdrop: %s %s", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++s->failures;
}

void grabAll(State *s, const QString &stage)
{
    if (s->shot.isEmpty())
        return;
    int i = 0;
    for (QQuickWindow *w : terminalWindows())
        w->grabWindow().save(QStringLiteral("%1-%2-%3.png").arg(s->shot, stage).arg(i++));
}

void markPid(State *s, const char *tag)
{
    if (s->view)
        s->view->sendText(QStringLiteral("echo %1:$$ | tee -a '%2'\r").arg(QLatin1String(tag), s->pidLog));
}

} // namespace

void PaneDropSelfTest::start(const QString &pidLog, const QString &shotPrefix)
{
    auto *s = new State;
    s->pidLog = pidLog;
    s->shot = shotPrefix;
    QFile::remove(pidLog);

    QTimer::singleShot(1500, qApp, [s] {
        const QList<QQuickWindow *> wins = terminalWindows();
        s->winA = wins.value(0);
        s->tabsA = s->winA ? tabsOf(s->winA) : nullptr;
        if (!s->tabsA) {
            qWarning("solidterm xdrop: no window");
            QCoreApplication::exit(1);
            return;
        }
        s->tabsA->split(Qt::Horizontal);
    });
    QTimer::singleShot(2600, qApp, [s] {
        s->view = s->tabsA->activePanes()->focusedView();
        check(s, s->tabsA->activePanes()->count() == 2, "window A split into 2 panes");
        markPid(s, "before");
    });
    // A drag nobody accepted (Esc / dropped outside every SolidTerm window): its own window.
    QTimer::singleShot(3600, qApp, [s] {
        delete TerminalPanes::makeDragMime(s->view);
        TerminalPanes::finishDrag(s->view);
    });
    QTimer::singleShot(4400, qApp, [s] {
        const QList<QQuickWindow *> wins = terminalWindows();
        check(s, wins.size() == 2, "unaccepted drag detached the pane into a 2nd window");
        check(s, s->tabsA->activePanes()->count() == 1, "window A kept 1 pane");
        TerminalPanes *owner = TerminalPanes::ownerOf(s->view);
        check(s, owner && owner->window() != s->winA.data(), "the pane lives in window B");
        grabAll(s, QStringLiteral("1-detached"));
        // Cross-window drop: B's pane onto the RIGHT zone of A's first pane.
        TerminalPanes::debugReattachLastToFirst(TerminalPanes::ZoneRight,
            s->shot.isEmpty() ? QString() : s->shot + QStringLiteral("-2-hover.png"));
    });
    QTimer::singleShot(5400, qApp, [s] {
        const QList<QQuickWindow *> wins = terminalWindows();
        check(s, wins.size() == 1, "window B closed once empty");
        check(s, TerminalPanes::ownerOf(s->view) == s->tabsA->activePanes(), "the SAME view now lives in A's split tree");
        check(s, s->tabsA->activePanes()->count() == 2, "window A has 2 panes again");
        check(s, s->tabsA->activePanes()->viewAt(1) == s->view.data(), "dropped on the right side of pane 1");
        markPid(s, "after");
        grabAll(s, QStringLiteral("3-reattached"));
    });
    // Tab-strip drop: becomes a new tab of A.
    QTimer::singleShot(6400, qApp, [s] {
        TerminalPanes *panes = s->tabsA->activePanes();
        const qreal top = panes->mapToScene(QPointF(0, 0)).y();
        if (top < 8) {
            qInfo("solidterm xdrop: SKIP tab-strip drop (no chrome above the panes)");
            return;
        }
        TerminalPanes::debugSyntheticDrag(s->view, s->winA, QPointF(s->winA->width() / 2.0, top / 2), true);
    });
    QTimer::singleShot(7200, qApp, [s] {
        if (s->tabsA->tabCount() > 1) {
            check(s, s->tabsA->tabCount() == 2, "tab-strip drop opened a 2nd tab");
            check(s, TerminalPanes::ownerOf(s->view) == s->tabsA->activePanes()
                     && s->tabsA->activePanes()->count() == 1, "the pane is that tab's only pane");
            grabAll(s, QStringLiteral("4-newtab"));
        }
        // Onto its own pane: nothing happens (no detach, no new window).
        const int tabs = s->tabsA->tabCount();
        QQuickItem *v = s->view;
        TerminalPanes::debugSyntheticDrag(s->view, s->winA, v->mapToScene(QPointF(v->width() / 2, v->height() / 2)), true);
        check(s, terminalWindows().size() == 1 && s->tabsA->tabCount() == tabs
                 && TerminalPanes::ownerOf(s->view), "drop on its own pane is a no-op");
    });
    QTimer::singleShot(8200, qApp, [s] {
        QFile f(s->pidLog);
        QString log;
        if (f.open(QIODevice::ReadOnly))
            log = QString::fromUtf8(f.readAll());
        const auto before = QRegularExpression(QStringLiteral("before:(\\d+)")).match(log);
        const auto after = QRegularExpression(QStringLiteral("after:(\\d+)")).match(log);
        check(s, before.hasMatch() && after.hasMatch() && before.captured(1) == after.captured(1),
              "the shell survived the move (same PID before and after)");
        qInfo("solidterm xdrop: shell pid before=%s after=%s", qPrintable(before.captured(1)), qPrintable(after.captured(1)));
        qInfo("solidterm xdrop: %s (%d failure(s))", s->failures ? "FAILED" : "OK", s->failures);
        QCoreApplication::exit(s->failures ? 1 : 0);
    });
}
