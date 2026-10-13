// solidterm — a real, daily-driver terminal (owner 2026-07-22: tilix-like, "esse terminal eu
// pretendo usar mesmo"). The app IS the two-way embed story in production shape: its own C++
// (PtySession + the libvterm-backed TerminalView) registered as the `SolidTerm` QML module and
// consumed from the solid TSX UI via `import { TerminalView } from "qml:SolidTerm"`; the solid
// runtime hosted through libsolidqml's embed surface. Rendering rides the Quick scene graph
// (GL/RHI accelerated).
//
// App dir resolution: $SOLIDTERM_DIR > <bindir>/../share/solidterm > the source tree (dev).
#include "embed/solidqmlembed.h"
#include "native/singleinstance.h"
#include "native/systemtheme.h"
#ifdef HAVE_BACKGROUND_EFFECT
#include "native/windowblur.h"
#endif
#include "native/keyrecorder.h"
#include "native/panedropselftest.h"
#include "native/tmuxselftest.h"
#include "native/paneheader.h"
#include "native/termconfig.h"
#include "native/terminalpanes.h"
#include "native/terminaltabs.h"
#include "native/terminalview.h"
#include "native/perftrace.h"
#include "native/tmuxsession.h"

#include <QApplication>
#include <QDir>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickWindow>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QTimer>
#include <QElapsedTimer>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>
#include <QGuiApplication>
#include <QClipboard>
#include <QSurfaceFormat>
#include <QtGui/qpa/qwindowsysteminterface.h>

// App-level key filter: F11 toggles fullscreen on the FOCUSED window (one process serves every
// window). Installed on the QApplication so it's caught before the focused TerminalView consumes it
// (no Q_OBJECT → no moc; eventFilter is a plain virtual).
class KeyFilter : public QObject {
public:
    bool eventFilter(QObject *obj, QEvent *event) override {
        auto *window = qobject_cast<QQuickWindow *>(QGuiApplication::focusWindow());
        if (event->type() == QEvent::KeyPress && window) {
            auto *ke = static_cast<QKeyEvent *>(event);
            if (ke->key() == Qt::Key_F11) {
                if (window->visibility() == QWindow::FullScreen)
                    window->showNormal();
                else
                    window->showFullScreen();
                return true;
            }
        }
        return QObject::eventFilter(obj, event);
    }
};

// Process-wide pieces every window shares (one process, N windows).
struct AppCtx {
    QQmlApplicationEngine *engine = nullptr;
    SystemTheme *sysTheme = nullptr;
    TermConfig *config = nullptr;
    QQmlComponent *windowComponent = nullptr; // compiled once; every further window instantiates it
    QUrl windowUrl;
};
static AppCtx g_app;

// Per-window wiring: the clear colour follows translucency, and the compositor blur follows the
// "blur" preference. Applied to the first window, forwarded windows and detached-pane windows alike.
static void wireWindow(QQuickWindow *window)
{
    // Closing a window (title-bar ✕, compositor kill) only HIDES a QQuickWindow. With one process
    // serving every window that would leave its shells running invisibly — destroy it instead: the
    // panes go with it and each PtySession hangs up its shell. (The engine drops a destroyed root
    // from rootObjects() by itself; the app still quits when the last window goes.)
    QObject::connect(window, &QQuickWindow::closing, window, [window] { window->deleteLater(); });
    // Bench probe (SOLIDTERM_PERF): stamp every presented frame, on the render thread.
    if (PerfTrace::on)
        QObject::connect(window, &QQuickWindow::frameSwapped, window, [] { PerfTrace::frameSwapped(); },
                         Qt::DirectConnection);
    SystemTheme *sysTheme = g_app.sysTheme;
    // Clear to transparent only when translucent (uiOpacity < 1) so the desktop shows through the
    // transparent app root; otherwise clear to the solid window colour. Tracks live changes.
    const auto retint = [window, sysTheme] {
        window->setColor(sysTheme->uiOpacity() < 1.0 ? QColor(Qt::transparent) : sysTheme->window());
    };
    retint();
    QObject::connect(sysTheme, &SystemTheme::changed, window, retint);
#ifdef HAVE_BACKGROUND_EFFECT
    // Blur behind the translucent window, done by the compositor (ext-background-effect-v1).
    // On while the "blur" strength (the preferences slider) is above 0, live, with that
    // strength where the compositor accepts one; inert on a compositor without the protocol.
    TermConfig *config = g_app.config;
    auto *blur = new WindowBlur(window);
    const auto applyBlur = [blur, config] {
        const int strength = config->getInt(QStringLiteral("blur"), 0);
        blur->setStrength(strength); // scales the blur where the compositor takes a strength
        blur->setEnabled(strength > 0);
    };
    applyBlur();
    QObject::connect(config, &TermConfig::changed, blur, [applyBlur](const QString &key) {
        if (key == QLatin1String("blur"))
            applyBlur();
    });
    if (qEnvironmentVariableIsSet("SOLIDTERM_BLURINFO"))
        qInfo("solidterm: background blur %s", blur->isSupported() ? "supported" : "NOT supported by the compositor");
#endif
}

// `--tmux [session]` / `--tmux-attach <session>` in an argument list (docs/solidterm-tmux-control-mode.md 8).
static bool tmuxArgs(const QStringList &args, QString *name, bool *attachOnly)
{
    for (qsizetype i = 0; i < args.size(); ++i) {
        const bool attach = args[i] == QLatin1String("--tmux-attach");
        if (args[i] != QLatin1String("--tmux") && !attach)
            continue;
        const bool hasName = i + 1 < args.size() && !args[i + 1].startsWith(QLatin1Char('-'));
        if (attach && !hasName)
            return false;
        *name = hasName ? args[i + 1] : QStringLiteral("main");
        *attachOnly = attach;
        return true;
    }
    return false;
}

// A new top-level window: its shells start in `cwd` with `env` (kept on the window, so later splits
// and tabs inherit them too); with `tmuxName` it shows that tmux session instead.
static QQuickWindow *openWindow(const QString &cwd, const QStringList &env, const QString &tmuxName, bool attachOnly)
{
    if (!g_app.windowComponent)
        g_app.windowComponent = new QQmlComponent(g_app.engine, g_app.windowUrl, g_app.engine);
    QQmlComponent *comp = g_app.windowComponent;
    const TerminalView::SpawnContext spawn { cwd, env };
    QObject *o = nullptr;
    {
        SolidQmlEmbed::MountBatch mount(g_app.engine);
        TerminalView::setPendingSpawn(&spawn);
        if (!tmuxName.isEmpty())
            TerminalTabs::setPendingTmuxSession(tmuxName, attachOnly);
        o = comp->beginCreate(g_app.engine->rootContext());
        if (o) {
            o->setProperty("solidtermCwd", cwd);
            o->setProperty("solidtermEnv", env);
            comp->completeCreate();
        }
        TerminalView::setPendingSpawn(nullptr);
        TerminalTabs::setPendingTmuxSession(QString());
    }
    auto *window = qobject_cast<QQuickWindow *>(o);
    if (!window) {
        qWarning("solidterm: new window failed: %s", qPrintable(comp->errorString()));
        delete o;
        return nullptr;
    }
    o->setParent(g_app.engine); // keep the top-level window alive
    wireWindow(window);
    return window;
}

// A window on tmux session `name` — or the window already showing it, raised (5.2).
static void openTmuxWindow(const QString &name, bool attachOnly)
{
    if (TmuxSession *s = TmuxSession::find(name)) {
        if (QQuickWindow *w = s->window()) {
            w->show();
            w->raise();
            w->requestActivate();
            return;
        }
    }
    openWindow(QString(), QStringList(), name, attachOnly);
}

// A window for another `solidterm` invocation (single instance): its directory and environment,
// and `--tmux` / `--tmux-attach` from its arguments.
static void openForwardedWindow(const SingleInstance::Request &request, void *)
{
    QString name;
    bool attachOnly = false;
    if (tmuxArgs(request.args, &name, &attachOnly)) {
        if (TmuxSession *s = TmuxSession::find(name); s && s->window()) {
            openTmuxWindow(name, attachOnly); // raises it
            return;
        }
        openWindow(request.cwd, request.env, name, attachOnly);
        return;
    }
    openWindow(request.cwd, request.env, QString(), false);
}

// Decision 3: on start, SolidTerm reattaches the tmux sessions IT created (marked @solidterm when
// it created them) that nothing is attached to. One window each. Off with tmuxReattach = 0.
static void reattachOwnTmuxSessions()
{
    auto *p = new QProcess(qApp);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.remove(QStringLiteral("TMUX"));
    p->setProcessEnvironment(env);
    QObject::connect(p, &QProcess::finished, p, [p] {
        const QStringList lines = QString::fromLocal8Bit(p->readAllStandardOutput()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const QString &l : lines) {
            const QStringList f = l.split(QLatin1Char('\t'));
            if (f.size() >= 3 && f[1] == QLatin1String("1") && f[2] == QLatin1String("0"))
                openTmuxWindow(f[0], true);
        }
        p->deleteLater();
    });
    QObject::connect(p, &QProcess::errorOccurred, p, [p](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) // no tmux: nothing to reattach
            p->deleteLater();
    });
    p->start(TmuxSession::binary(), { QStringLiteral("list-sessions"), QStringLiteral("-F"),
                                      QStringLiteral("#{session_name}\t#{@solidterm}\t#{session_attached}") });
}

static const char kUsage[] =
    "Usage: solidterm [options]\n"
    "\n"
    "A terminal. One process serves every window: a second launch opens its window in the running\n"
    "instance (with the caller's directory and environment).\n"
    "\n"
    "Options:\n"
    "  --tmux [session]  attach to tmux session <session> (default \"main\"), creating it if needed,\n"
    "                    in tmux control mode: tmux windows are tabs, panes are splits; closing the\n"
    "                    window detaches and the session keeps running. A window already showing\n"
    "                    that session is raised instead.\n"
    "  --tmux-attach <session>\n"
    "                    the same, but only attach (an error if there is no such session)\n"
    "  --standalone      do not share a process with other SolidTerm windows\n"
    "  -h, --help        show this help and exit\n"
    "\n"
    "Environment:\n"
    "  SOLIDTERM_DIR      the UI directory (default: <bindir>/../share/solidterm)\n"
    "  SOLIDTERM_TMUX_BIN the tmux binary for --tmux (default: Preferences, else tmux in PATH)\n"
    "\n"
    "On start, SolidTerm reattaches the tmux sessions it created that nothing is attached to\n"
    "(Preferences: tmux → Reattach my sessions on start). Requires tmux 3.5 or newer.\n";

int main(int argc, char **argv)
{
    // --help before anything else: no window, no single-instance socket, no Qt.
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::fputs(kUsage, stdout);
            return 0;
        }
    }
    // Single instance: a later launch hands its cwd + environment to the running process and exits
    // here, before any Qt/Wayland/GL start-up — only the first terminal pays the cold boot.
    const SingleInstance::Role role = SingleInstance::negotiate(argc, argv);
    if (role == SingleInstance::Role::Forwarded)
        return 0;

    // Request an alpha channel on the window surface so translucent chrome (uiOpacity < 1) reveals
    // the desktop behind the whole window.
    {
        QSurfaceFormat fmt = QSurfaceFormat::defaultFormat();
        fmt.setAlphaBufferSize(8);
        QSurfaceFormat::setDefaultFormat(fmt);
    }
    QApplication app(argc, argv);
    // Bench probes (scripts/solidterm-bench.sh): off — and free — unless SOLIDTERM_PERF is set.
    PerfTrace::init(qEnvironmentVariableIsSet("SOLIDTERM_PERF"));
    if (PerfTrace::on)
        QObject::connect(&app, &QCoreApplication::aboutToQuit, [] {
            PerfTrace::dump(qgetenv("SOLIDTERM_PERF_OUT").constData());
        });
    app.setApplicationName(QStringLiteral("solidterm"));
    app.setOrganizationName(QStringLiteral("solid-qml"));

    QString dir = qEnvironmentVariable("SOLIDTERM_DIR");
    if (dir.isEmpty()) {
        const QString installed = QCoreApplication::applicationDirPath() + QStringLiteral("/../share/solidterm");
        if (QDir(installed).exists(QStringLiteral("App.generated.qml")))
            dir = installed;
    }
#ifdef SOLIDTERM_DEV_DIR
    if (dir.isEmpty() && QDir(QStringLiteral(SOLIDTERM_DEV_DIR)).exists(QStringLiteral("App.generated.qml")))
        dir = QStringLiteral(SOLIDTERM_DEV_DIR);
#endif
    if (dir.isEmpty()) {
        qWarning("solidterm: no app dir (set SOLIDTERM_DIR)");
        return 1;
    }
    const QUrl appDir = QUrl::fromLocalFile(QDir(dir).absolutePath() + QLatin1Char('/'));

    qmlRegisterType<TerminalView>("SolidTerm", 1, 0, "TerminalView");
    qmlRegisterType<TerminalPanes>("SolidTerm", 1, 0, "TerminalPanes");
    qmlRegisterType<TerminalTabs>("SolidTerm", 1, 0, "TerminalTabs");
    qmlRegisterType<KeyRecorder>("SolidTerm", 1, 0, "KeyRecorder");
    qmlRegisterType<PaneHeader>("SolidTerm", 1, 0, "PaneHeader");

    QQmlApplicationEngine engine;
    SolidQmlEmbed::init(&engine, appDir);

    // Obey the local desktop theme: derive the app's colour layer from QPalette (which Qt takes
    // from the platform/GTK theme) and re-apply it live if the desktop theme changes.
    auto *sysTheme = new SystemTheme(&engine);
    engine.rootContext()->setContextProperty(QStringLiteral("sysTheme"), sysTheme);
    auto *config = new TermConfig(&engine); // ~/.config/solidterm/config.json
    g_app.engine = &engine;
    g_app.sysTheme = sysTheme;
    g_app.config = config;
    engine.rootContext()->setContextProperty(QStringLiteral("termConfig"), config);
    // The saved translucency goes in BEFORE the palette layer loads: set later (by the UI while it
    // is being built) it reloads the theme and re-styles every element created so far.
    sysTheme->setUiOpacity(config->getInt(QStringLiteral("opacity"), 100) / 100.0);
    SolidQmlEmbed::loadCss(&engine, QUrl(QStringLiteral("App.generated.css"))); // structural
    SolidQmlEmbed::loadCssString(&engine, sysTheme->styleSheet());              // palette layer
    QObject::connect(sysTheme, &SystemTheme::changed, &engine, [&engine, sysTheme] {
        SolidQmlEmbed::loadCssString(&engine, sysTheme->styleSheet());
    });

    // `solidterm --tmux [session]` (docs/solidterm-tmux-control-mode.md, phase 1: a read-only viewer):
    // the first window attaches to that tmux session — or creates it — instead of a local shell.
    // SOLIDTERM_TMUX_BIN replaces the tmux binary (the tests' fake server).
    TmuxSession::setConfig(config);
    TerminalTabs::setOpenTmuxWindowHook(openTmuxWindow);
    QString firstTmux;
    bool firstAttachOnly = false;
    if (tmuxArgs(QCoreApplication::arguments(), &firstTmux, &firstAttachOnly))
        TerminalTabs::setPendingTmuxSession(firstTmux, firstAttachOnly);

    const QUrl windowUrl = appDir.resolved(QUrl(QStringLiteral("App.generated.qml")));
    g_app.windowUrl = windowUrl;
    TerminalPanes::setWindowUrl(windowUrl); // so a pane detach can spawn a full new solidterm window
    {
        SolidQmlEmbed::MountBatch mount(&engine); // one layout for the whole UI, not one per element
        engine.load(windowUrl);
    }
    if (engine.rootObjects().isEmpty())
        return 1;
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    if (window) {
        SolidQmlEmbed::attachWindow(&engine, window); // vw/vh follow the first window
        app.installEventFilter(new KeyFilter);       // F11 → fullscreen (app-level, before the terminal)
        wireWindow(window);
    }
    // Detached panes open their own windows: same per-window wiring.
    TerminalPanes::setWindowCreatedHook(wireWindow);
    // A dragged pane's preview (the drag icon) carries the chrome's look: the preview is already a
    // translucent snapshot; where the compositor offers ext-background-effect-v1 the icon surface
    // asks for the same blur as the windows (compositor policy decides whether drag icons get it).
    TerminalPanes::setDragIconHook([](QWindow *icon) {
#ifdef HAVE_BACKGROUND_EFFECT
        const int strength = g_app.config->getInt(QStringLiteral("blur"), 0);
        auto *blur = new WindowBlur(icon); // owned by the icon window: goes when the drag ends
        blur->setStrength(strength);
        blur->setEnabled(strength > 0);
        if (qEnvironmentVariableIsSet("SOLIDTERM_BLURINFO"))
            qInfo("solidterm: drag icon blur %s (strength %d)", blur->isSupported() ? "requested" : "unsupported", strength);
#else
        Q_UNUSED(icon);
#endif
    });

    TerminalTabs::setPendingTmuxSession(QString());
    if (role == SingleInstance::Role::Server) {
        g_app.windowComponent = new QQmlComponent(&engine, windowUrl, &engine); // already compiled: cached
        SingleInstance::listen(openForwardedWindow, nullptr);
        // Only the instance (never a standalone/test run: those must not touch a tmux server).
        if (config->getInt(QStringLiteral("tmuxReattach"), 1) != 0)
            QTimer::singleShot(0, qApp, reattachOwnTmuxSessions);
    }

    // Debug/CI: SOLIDTERM_TMUXTEST=fake|real — self-checking tmux-mode run (see tmuxselftest.h).
    if (qEnvironmentVariableIsSet("SOLIDTERM_TMUXTEST") && window)
        TmuxSelfTest::start(window, qEnvironmentVariable("SOLIDTERM_TMUXTEST"));

    // Debug/CI screenshot: SOLIDTERM_SHOT=<prefix> opens preferences + the context menu after a
    // settle and grabs every top-level window (main + the modal dialog) to <prefix>-<i>.png.
    const QString shot = qEnvironmentVariable("SOLIDTERM_SHOT");
    if (!shot.isEmpty() && window) {
        QTimer::singleShot(1600, window, [&engine, shot] {
            // Open the preferences dialog + a context menu by poking the component state.
            for (QObject *o : engine.rootObjects()) {
                if (auto *w = qobject_cast<QQuickWindow *>(o)) {
                    QQuickItem *self = w->contentItem();
                    // depth-first for the item exposing cfgOpen.
                    QList<QQuickItem *> stack { self };
                    while (!stack.isEmpty()) {
                        QQuickItem *it = stack.takeLast();
                        if (it->property("cfgOpen").isValid()) { it->setProperty("cfgOpen", true); break; }
                        for (QQuickItem *k : it->childItems()) stack.append(k);
                    }
                }
            }
            QTimer::singleShot(700, qApp, [shot] {
                int i = 0;
                for (QWindow *w : QGuiApplication::topLevelWindows()) {
                    if (auto *qw = qobject_cast<QQuickWindow *>(w))
                        qw->grabWindow().save(QStringLiteral("%1-%2.png").arg(shot).arg(i++));
                }
                QCoreApplication::quit();
            });
        });
    }
    // Debug: SOLIDTERM_SENDKEYS="Alt+D@1500,Alt+Shift+D@2500" synthesises key chords to the window
    // (real QKeyEvent press+release through delivery) — for testing accelerator conflicts headless.
    const QString sendKeys = qEnvironmentVariable("SOLIDTERM_SENDKEYS");
    if (!sendKeys.isEmpty() && window) {
        for (const QString &spec : sendKeys.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            const QStringList parts = spec.split(QLatin1Char('@'));
            if (parts.size() != 2)
                continue;
            const QKeySequence seq(parts[0]);
            const int delay = parts[1].toInt();
            if (seq.isEmpty())
                continue;
            const QKeyCombination kc = seq[0];
            QTimer::singleShot(delay, window, [window, kc] {
                QKeyEvent press(QEvent::KeyPress, kc.key(), kc.keyboardModifiers());
                QCoreApplication::sendEvent(window, &press);
                QKeyEvent release(QEvent::KeyRelease, kc.key(), kc.keyboardModifiers());
                QCoreApplication::sendEvent(window, &release);
                qInfo().noquote() << "solidterm: sent" << QKeySequence(kc).toString();
            });
        }
    }

    // Debug/CI: SOLIDTERM_FOCUSTEST=1 sends the REAL Ctrl+Shift+F accelerator (a QKeyEvent through
    // the same QCoreApplication::sendEvent(window, …) path SENDKEYS/MOUSETEST use — real Qt Quick
    // focus delivery, not a property poke) and immediately types "x" with no extra delay, the way a
    // fast typist would. It then asserts activeFocusItem() is NOT the terminal and that "x" landed
    // in the search field's own text (not silently dropped, and not routed to whatever had focus
    // before) — prints PASS/FAIL and quits. This is a regression test for a real bug: `forceActive
    // Focus()` called on the <input> ref used to focus the decorative CssFill WRAPPER (TextField's
    // root item, not the T.TextField that actually handles keys — see TextInputBase::
    // forceActiveFocus(), textinputs.cpp) — Qt reported a focus item, so nothing here LOOKED wrong
    // at a glance, but keystrokes never reached the field. No sleep/timer is part of the fix itself
    // — only this test's own settle delay before the keys are sent.
    if (qEnvironmentVariableIsSet("SOLIDTERM_FOCUSTEST") && window) {
        QTimer::singleShot(1600, window, [window] {
            auto sendSeq = [window](const QKeySequence &seq, Qt::Key key, Qt::KeyboardModifiers mods, const QString &text) {
                QKeyEvent press(QEvent::KeyPress, key, mods, text);
                QCoreApplication::sendEvent(window, &press);
                QKeyEvent release(QEvent::KeyRelease, key, mods, text);
                QCoreApplication::sendEvent(window, &release);
                Q_UNUSED(seq);
            };
            const QKeyCombination accel = QKeySequence(QStringLiteral("Ctrl+Shift+F"))[0];
            sendSeq(QKeySequence(accel), Qt::Key(accel.key()), accel.keyboardModifiers(), QString());
            // Back-to-back, no settle: a real fast typist doesn't wait for the field to "catch up".
            sendSeq(QKeySequence(), Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
            QQuickItem *f = window->activeFocusItem();
            const QString cls = f ? QString::fromLatin1(f->metaObject()->className()) : QStringLiteral("null");
            const QString text = f ? f->property("text").toString() : QString();
            bool ok = f && !cls.contains(QLatin1String("TerminalView")) && text == QLatin1String("x");
            // Esc in the field closes the bar and hands the keyboard back to the terminal.
            sendSeq(QKeySequence(), Qt::Key_Escape, Qt::NoModifier, QString());
            QQuickItem *after = window->activeFocusItem();
            const QString afterCls = after ? QString::fromLatin1(after->metaObject()->className()) : QStringLiteral("null");
            ok = ok && after && after->inherits("TerminalView");
            qInfo().noquote() << (ok ? "FOCUSTEST PASS" : "FOCUSTEST FAIL") << "focusItem=" << cls << "text=" << text
                              << "afterEsc=" << afterCls;
            QCoreApplication::exit(ok ? 0 : 1);
        });
    }

    // Debug: SOLIDTERM_PLAINSHOT=<prefix> grabs every window after a settle WITHOUT poking anything
    // (so focus state is real) — to check the fresh terminal grabs keyboard focus on launch.
    const QString plainShot = qEnvironmentVariable("SOLIDTERM_PLAINSHOT");
    if (!plainShot.isEmpty() && window) {
        QTimer::singleShot(1800, qApp, [plainShot] {
            int i = 0;
            for (QWindow *w : QGuiApplication::topLevelWindows())
                if (auto *qw = qobject_cast<QQuickWindow *>(w))
                    qw->grabWindow().save(QStringLiteral("%1-%2.png").arg(plainShot).arg(i++));
            QCoreApplication::quit();
        });
    }

    // Debug: SOLIDTERM_AUTOSPLIT=<orient> splits the panes after a settle (1=horizontal, 2=vertical)
    // to test multi-pane rendering without keyboard focus.
    const QString autoSplit = qEnvironmentVariable("SOLIDTERM_AUTOSPLIT");
    if (!autoSplit.isEmpty() && window) {
        int t = 1400;
        for (const QString &os : autoSplit.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            const QString cmd = os;
            QTimer::singleShot(t, window, [&engine, cmd] {
                for (QObject *o : engine.rootObjects()) {
                    if (auto *w = qobject_cast<QQuickWindow *>(o)) {
                        QList<QQuickItem *> stack { w->contentItem() };
                        while (!stack.isEmpty()) {
                            QQuickItem *it = stack.takeLast();
                            if (auto *tabs = qobject_cast<TerminalTabs *>(it)) {
                                if (cmd == QLatin1String("c")) tabs->closeFocused();
                                else if (cmd == QLatin1String("n")) tabs->focusNext();
                                else if (cmd == QLatin1String("t")) tabs->newTab();
                                else if (cmd == QLatin1String("z")) tabs->toggleZoom();
                                else if (cmd == QLatin1String("upaste")) {
                                    QGuiApplication::clipboard()->setText(QStringLiteral("line one\nline two\nline three"));
                                    tabs->pasteFocused();
                                } else if (cmd.startsWith(QLatin1String("rename:"))) tabs->setTabTitle(0, cmd.mid(7));
                                else if (cmd.startsWith(QLatin1Char('s'))) tabs->selectTab(cmd.mid(1).toInt());
                                else tabs->split(cmd.toInt());
                                return;
                            }
                            for (QQuickItem *k : it->childItems()) stack.append(k);
                        }
                    }
                }
            });
            t += 500;
        }
    }

    // Debug: SOLIDTERM_DRAGTEST="src,dst,zone" drives the header-drag rearrange after a settle
    // (zone 1=centre 2=left 3=right 4=top 5=bottom). Use with AUTOSPLIT to build panes first, e.g.
    // SOLIDTERM_AUTOSPLIT=1 SOLIDTERM_DRAGTEST="1,0,5" drags pane #2 onto the bottom of pane #1.
    const QString dragTest = qEnvironmentVariable("SOLIDTERM_DRAGTEST");
    if (!dragTest.isEmpty() && window) {
        const QStringList parts = dragTest.split(QLatin1Char(','));
        if (parts.size() == 3) {
            const int s = parts[0].toInt(), d = parts[1].toInt(), z = parts[2].toInt();
            QTimer::singleShot(2200, window, [&engine, s, d, z] {
                for (QObject *o : engine.rootObjects())
                    if (auto *w = qobject_cast<QQuickWindow *>(o)) {
                        QList<QQuickItem *> stack { w->contentItem() };
                        while (!stack.isEmpty()) {
                            QQuickItem *it = stack.takeLast();
                            if (auto *panes = qobject_cast<TerminalPanes *>(it)) {
                                panes->debugDragLeaf(s, d, z);
                                // Grab after the layout settles, then quit (self-contained; don't pair
                                // with PLAINSHOT, whose earlier timer would quit before the drag ran).
                                const QString pfx = qEnvironmentVariable("SOLIDTERM_DRAGSHOT");
                                if (!pfx.isEmpty())
                                    QTimer::singleShot(500, qApp, [pfx] {
                                        int i = 0;
                                        for (QWindow *tw : QGuiApplication::topLevelWindows())
                                            if (auto *qw = qobject_cast<QQuickWindow *>(tw))
                                                qw->grabWindow().save(QStringLiteral("%1-%2.png").arg(pfx).arg(i++));
                                        QCoreApplication::quit();
                                    });
                                return;
                            }
                            for (QQuickItem *k : it->childItems()) stack.append(k);
                        }
                    }
            });
        }
    }

    // Debug: SOLIDTERM_DETACH=1 detaches the focused pane to a new window after a settle (use with
    // AUTOSPLIT so the source keeps a pane). Exercises the cross-window detach without a gesture.
    if (qEnvironmentVariableIsSet("SOLIDTERM_DETACH") && window) {
        QTimer::singleShot(2200, window, [&engine] {
            for (QObject *o : engine.rootObjects())
                if (auto *w = qobject_cast<QQuickWindow *>(o)) {
                    QList<QQuickItem *> stack { w->contentItem() };
                    while (!stack.isEmpty()) {
                        QQuickItem *it = stack.takeLast();
                        if (auto *panes = qobject_cast<TerminalPanes *>(it)) {
                            panes->debugDetachFocused();
                            const QString pfx = qEnvironmentVariable("SOLIDTERM_DETACHSHOT");
                            if (!pfx.isEmpty() && !qEnvironmentVariableIsSet("SOLIDTERM_REATTACH"))
                                QTimer::singleShot(700, qApp, [pfx] {
                                    int i = 0;
                                    for (QWindow *tw : QGuiApplication::topLevelWindows())
                                        if (auto *qw = qobject_cast<QQuickWindow *>(tw))
                                            qw->grabWindow().save(QStringLiteral("%1-%2.png").arg(pfx).arg(i++));
                                    QCoreApplication::quit();
                                });
                            return;
                        }
                        for (QQuickItem *k : it->childItems()) stack.append(k);
                    }
                }
        });
    }

    // Debug: SOLIDTERM_REATTACH=1 (with SOLIDTERM_DETACH) reattaches the detached window's pane back
    // into the first window through the drop path, then grabs — the full detach→reattach round trip.
    // The value picks the target: 2/3/4/5 = left/right/top/bottom of the first pane (1 = right,
    // the default), 6 = its tab strip (new tab). With SOLIDTERM_DETACHSHOT the hover (drop indicator) is grabbed too.
    if (qEnvironmentVariableIsSet("SOLIDTERM_REATTACH") && window) {
        QTimer::singleShot(3200, window, [] {
            const int z = qEnvironmentVariableIntValue("SOLIDTERM_REATTACH");
            const QString pfx = qEnvironmentVariable("SOLIDTERM_DETACHSHOT");
            TerminalPanes::debugReattachLastToFirst(z >= 2 && z <= 6 ? z : TerminalPanes::ZoneRight,
                                                    pfx.isEmpty() ? QString() : pfx + QStringLiteral("-hover.png"));
            if (!pfx.isEmpty())
                QTimer::singleShot(700, qApp, [pfx] {
                    int i = 0;
                    for (QWindow *tw : QGuiApplication::topLevelWindows())
                        if (auto *qw = qobject_cast<QQuickWindow *>(tw))
                            qw->grabWindow().save(QStringLiteral("%1-%2.png").arg(pfx).arg(i++));
                    QCoreApplication::quit();
                });
        });
    }

    // Debug/CI: SOLIDTERM_XDROPTEST=1 — self-checking pane drag & drop run (see panedropselftest.h).
    if (qEnvironmentVariableIsSet("SOLIDTERM_XDROPTEST") && window) {
        QString log = qEnvironmentVariable("SOLIDTERM_PIDLOG");
        if (log.isEmpty())
            log = QDir::current().absoluteFilePath(QStringLiteral("solidterm-xdrop-pid.log"));
        PaneDropSelfTest::start(log, qEnvironmentVariable("SOLIDTERM_XDROPSHOT"));
    }

    // Debug: SOLIDTERM_OVERVIEW=1 opens the F12 overview after a settle (use with AUTOSPLIT).
    if (qEnvironmentVariableIsSet("SOLIDTERM_OVERVIEW") && window) {
        QTimer::singleShot(3200, window, [&engine] {
            for (QObject *o : engine.rootObjects())
                if (auto *w = qobject_cast<QQuickWindow *>(o)) {
                    QList<QQuickItem *> stack { w->contentItem() };
                    while (!stack.isEmpty()) {
                        QQuickItem *it = stack.takeLast();
                        if (it->property("overviewOpen").isValid()) it->setProperty("overviewOpen", true);
                        for (QQuickItem *k : it->childItems()) stack.append(k);
                    }
                }
        });
    }

    // Debug: SOLIDTERM_PANEMENU="x,y" opens the pane header dropdown at scene (x,y) after a settle,
    // by calling the root's openPaneMenu(x, y, ro) — the same path a real ▼ click takes.
    if (qEnvironmentVariableIsSet("SOLIDTERM_PANEMENU") && window) {
        QTimer::singleShot(1600, window, [&engine] {
            const QStringList xy = qEnvironmentVariable("SOLIDTERM_PANEMENU").split(QLatin1Char(','));
            const int mx = xy.size() == 2 ? xy[0].toInt() : 300;
            const int my = xy.size() == 2 ? xy[1].toInt() : 20;
            for (QObject *o : engine.rootObjects())
                if (auto *w = qobject_cast<QQuickWindow *>(o)) {
                    QList<QQuickItem *> stack { w->contentItem() };
                    while (!stack.isEmpty()) {
                        QQuickItem *it = stack.takeLast();
                        if (it->metaObject()->indexOfMethod("openPaneMenu(QVariant,QVariant,QVariant)") >= 0) {
                            QMetaObject::invokeMethod(it, "openPaneMenu", Q_ARG(QVariant, mx), Q_ARG(QVariant, my), Q_ARG(QVariant, false));
                            return;
                        }
                        for (QQuickItem *k : it->childItems()) stack.append(k);
                    }
                }
        });
    }

    // Debug/CI: SOLIDTERM_CTXMENU=<prefix> right-clicks the centre of the first TerminalView after a
    // settle — a real Qt::RightButton press+release through QCoreApplication::sendEvent (the same
    // delivery path a user's right-click takes, so ContextMenu's own MouseArea + suppressWhen run
    // for real), opening the terminal's right-click menu as its own Popup.Window, then grabs EVERY
    // top-level window (main window + the popup) to <prefix>-<i>.png and quits — mirrors
    // SOLIDTERM_PANEMENU (which opens the pane-header dropdown; combine it with SOLIDTERM_PLAINSHOT
    // to grab THAT one, since it already calls openPaneMenu() directly instead of clicking).
    const QString ctxMenuShot = qEnvironmentVariable("SOLIDTERM_CTXMENU");
    if (!ctxMenuShot.isEmpty() && window) {
        QTimer::singleShot(1600, window, [&engine, window, ctxMenuShot] {
            TerminalView *view = nullptr;
            for (QObject *o : engine.rootObjects()) {
                if (auto *w = qobject_cast<QQuickWindow *>(o)) {
                    QList<QQuickItem *> stack { w->contentItem() };
                    while (!stack.isEmpty() && !view) {
                        QQuickItem *it = stack.takeLast();
                        view = qobject_cast<TerminalView *>(it);
                        for (QQuickItem *k : it->childItems()) stack.append(k);
                    }
                }
            }
            if (!view) {
                qWarning("solidterm: ctxmenu found no TerminalView");
                QCoreApplication::quit();
                return;
            }
            const QPointF p = view->mapToScene(QPointF(view->width() / 2, view->height() / 2));
            const QPointF g = window->mapToGlobal(p.toPoint());
            QMouseEvent press(QEvent::MouseButtonPress, p, g, Qt::RightButton, Qt::RightButton, Qt::NoModifier);
            QCoreApplication::sendEvent(window, &press);
            QMouseEvent release(QEvent::MouseButtonRelease, p, g, Qt::RightButton, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(window, &release);
            // Settle for the popup window to be created and painted before grabbing.
            QTimer::singleShot(400, qApp, [ctxMenuShot] {
                int i = 0;
                for (QWindow *w : QGuiApplication::topLevelWindows()) {
                    if (auto *qw = qobject_cast<QQuickWindow *>(w))
                        qw->grabWindow().save(QStringLiteral("%1-%2.png").arg(ctxMenuShot).arg(i++));
                }
                QCoreApplication::quit();
            });
        });
    }

    // Debug: SOLIDTERM_MOUSETEST=1 (or "x,y", window-local, to override the point) synthesises a
    // real left-click press+release and a wheel-up notch after a settle — through
    // QCoreApplication::sendEvent(window, …) like SENDKEYS does for keys, so Qt Quick's own
    // hit-testing + TerminalView's real event handlers run (not a direct method call). Without an
    // explicit point it finds the first TerminalView in the tree and clicks its centre (same
    // tree-walk idiom as AUTOSPLIT/AUTOSPLIT's TerminalTabs lookup below). Used to verify mouse
    // reporting end-to-end: point SHELL at a script that enables SGR mouse tracking and redirects
    // what it reads back to a file.
    const QString mouseTest = qEnvironmentVariable("SOLIDTERM_MOUSETEST");
    if (!mouseTest.isEmpty() && window) {
        const QStringList xy = mouseTest.split(QLatin1Char(','));
        const bool explicitPoint = xy.size() == 2;
        const qreal ex = explicitPoint ? xy[0].toDouble() : 0;
        const qreal ey = explicitPoint ? xy[1].toDouble() : 0;
        QTimer::singleShot(1600, window, [&engine, window, explicitPoint, ex, ey] {
            QPointF p(ex, ey);
            if (!explicitPoint) {
                TerminalView *view = nullptr;
                for (QObject *o : engine.rootObjects()) {
                    if (auto *w = qobject_cast<QQuickWindow *>(o)) {
                        QList<QQuickItem *> stack { w->contentItem() };
                        while (!stack.isEmpty() && !view) {
                            QQuickItem *it = stack.takeLast();
                            view = qobject_cast<TerminalView *>(it);
                            for (QQuickItem *k : it->childItems()) stack.append(k);
                        }
                    }
                }
                if (!view) {
                    qWarning("solidterm: mousetest found no TerminalView");
                    return;
                }
                p = view->mapToScene(QPointF(view->width() / 2, view->height() / 2));
            }
            const QPointF g = window->mapToGlobal(p.toPoint());
            QMouseEvent press(QEvent::MouseButtonPress, p, g, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(window, &press);
            // 4th arg is the button whose state CHANGED (released) — Left, same as the press — not
            // NoButton; 5th is the set still held afterwards, which correctly IS NoButton.
            QMouseEvent release(QEvent::MouseButtonRelease, p, g, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(window, &release);
            QWheelEvent wheel(p, g, QPoint(0, 0), QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                               Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(window, &wheel);
            qInfo().noquote() << "solidterm: mousetest sent click+wheel at" << p;
        });
    }

    // Bench: SOLIDTERM_LATENCYTEST=<n> (with SOLIDTERM_PERF) types n keys into the first TerminalView,
    // one in flight at a time (150ms apart: letters, every other one erased again with Backspace so the
    // line stays short), through QWindowSystemInterface — the platform-input entry point a real key
    // takes — then quits. PerfTrace records each key's stages (see perftrace.h).
    if (const int nKeys = qEnvironmentVariableIntValue("SOLIDTERM_LATENCYTEST"); nKeys > 0 && window) {
        QTimer::singleShot(2500, window, [window, nKeys] {
            auto *timer = new QTimer(window);
            auto *sent = new int(0);
            timer->setInterval(150);
            QObject::connect(timer, &QTimer::timeout, window, [window, timer, sent, nKeys] {
                if (*sent >= nKeys) {
                    timer->stop();
                    QTimer::singleShot(400, qApp, &QCoreApplication::quit);
                    return;
                }
                const bool erase = (*sent & 1) != 0;
                const int key = erase ? int(Qt::Key_Backspace) : int(Qt::Key_A) + (*sent / 2) % 26;
                const QString text = erase ? QString(QChar(8)) : QString(QChar('a' + (*sent / 2) % 26));
                QWindowSystemInterface::handleKeyEvent(window, QEvent::KeyPress, key, Qt::NoModifier, text);
                QWindowSystemInterface::handleKeyEvent(window, QEvent::KeyRelease, key, Qt::NoModifier, text);
                ++*sent;
            });
            timer->start();
        });
    }

    // Bench: SOLIDTERM_FLOODKEYS=<n> (with SOLIDTERM_PERF) types n 'x' keys, 100 ms apart, from a
    // HELPER THREAD through QWindowSystemInterface (asynchronous delivery: what the Wayland event
    // thread does for a real key), so the injection time is not delayed by a busy GUI thread. Meant
    // for a flood (the bench's `flood` case runs seq meanwhile): PerfTrace then records injection →
    // keyPressEvent → the parse carrying the echo → updatePaintNode → frameSwapped.
    if (const int nKeys = qEnvironmentVariableIntValue("SOLIDTERM_FLOODKEYS"); nKeys > 0 && window && PerfTrace::on) {
        PerfTrace::floodKeys = true;
        static std::atomic<bool> stopKeys { false };
        static std::thread injector;
        // The window can go away (the shell exits → last window closes) while the thread still
        // types: stop at its destruction; keep nKeys well inside the flood's duration anyway.
        QObject::connect(window, &QObject::destroyed, [] { stopKeys.store(true); });
        injector = std::thread([window, nKeys] {
            std::this_thread::sleep_for(std::chrono::milliseconds(2500));
            for (int i = 0; i < nKeys && !stopKeys.load(); ++i) {
                PerfTrace::stage(PerfTrace::Key);
                QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::AsynchronousDelivery>(
                    window, QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
                QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::AsynchronousDelivery>(
                    window, QEvent::KeyRelease, Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
        QObject::connect(&app, &QCoreApplication::aboutToQuit, [] {
            stopKeys.store(true);
            if (injector.joinable())
                injector.join();
        });
    }

    // Bench: SOLIDTERM_PERF_GUITICK=1 (with SOLIDTERM_PERF): a 5 ms precise timer on the GUI thread
    // stamps every firing (PerfTrace::guiTick); the bench reports how late it fired during a flood.
    if (PerfTrace::on && qEnvironmentVariableIsSet("SOLIDTERM_PERF_GUITICK")) {
        auto *tick = new QTimer(&app);
        tick->setTimerType(Qt::PreciseTimer);
        tick->setInterval(5);
        QObject::connect(tick, &QTimer::timeout, [] { PerfTrace::guiTick(); });
        tick->start();
    }

    // Bench: SOLIDTERM_WHEELTEST=<n> waits until the first TerminalView holds >= 10000 scrollback lines
    // (the bench's shell prints them), then sends n wheel-up notches followed by n wheel-down notches at
    // 125 Hz through QWindowSystemInterface (a fast continuous scroll), then quits. The frame log
    // (SOLIDTERM_PERF) gives the frame-time histogram for it.
    if (const int nWheel = qEnvironmentVariableIntValue("SOLIDTERM_WHEELTEST"); nWheel > 0 && window) {
        auto *poll = new QTimer(window);
        poll->setInterval(100);
        QObject::connect(poll, &QTimer::timeout, window, [window, poll, nWheel] {
            TerminalView *view = nullptr;
            QList<QQuickItem *> stack { window->contentItem() };
            while (!stack.isEmpty() && !view) {
                QQuickItem *it = stack.takeLast();
                view = qobject_cast<TerminalView *>(it);
                for (QQuickItem *k : it->childItems()) stack.append(k);
            }
            if (view && view->scrollbackLimit() < 20000)
                view->setScrollbackLimit(20000); // room for the 10k lines (the user's config may cap lower)
            if (!view || view->scrollbackCount() < 10000)
                return;
            poll->stop();
            const QPointF c = view->mapToScene(QPointF(view->width() / 2, view->height() / 2));
            const QPointF g = window->mapToGlobal(c.toPoint());
            auto *timer = new QTimer(window);
            auto *sent = new int(0);
            timer->setInterval(8);
            timer->setTimerType(Qt::PreciseTimer);
            QObject::connect(timer, &QTimer::timeout, window, [window, timer, sent, nWheel, c, g] {
                if (*sent >= 2 * nWheel) {
                    timer->stop();
                    QTimer::singleShot(300, qApp, &QCoreApplication::quit);
                    return;
                }
                const int dy = *sent < nWheel ? 120 : -120;
                QWindowSystemInterface::handleWheelEvent(window, c, g, QPoint(0, 0), QPoint(0, dy));
                ++*sent;
            });
            QTimer::singleShot(500, window, [timer] { timer->start(); }); // let the fill settle
        });
        poll->start();
    }

    // Debug: SOLIDTERM_AUTOSCROLLTEST=1 fills the scrollback, presses near the middle of the first
    // TerminalView, moves the pointer ABOVE the view once, then does NOT move it again — the
    // autoscroll QTimer (autoscroll.h / TerminalView::autoscrollTick) must keep scrolling INTO the
    // scrollback and growing the selection on its own. Real input, injected via
    // QWindowSystemInterface::handleMouseEvent (the same platform-input entry point QTest::mouseMove
    // uses) — NOT a raw QMouseEvent via QCoreApplication::sendEvent: that bypasses Qt's own
    // QPointingDevice/EventPoint bookkeeping, so Qt Quick's window-level mouse GRAB silently never
    // sees the followup move (verified live: a window-targeted synthetic move while "pressed" never
    // reached mouseMoveEvent at all — SOLIDTERM_MOUSETEST's press+release still worked because both
    // land at the SAME point, so no grab hand-off was ever exercised there).
    if (qEnvironmentVariableIsSet("SOLIDTERM_AUTOSCROLLTEST") && window) {
        QTimer::singleShot(1600, window, [&engine, window] {
            TerminalView *view = nullptr;
            for (QObject *o : engine.rootObjects()) {
                if (auto *w = qobject_cast<QQuickWindow *>(o)) {
                    QList<QQuickItem *> stack { w->contentItem() };
                    while (!stack.isEmpty() && !view) {
                        QQuickItem *it = stack.takeLast();
                        view = qobject_cast<TerminalView *>(it);
                        for (QQuickItem *k : it->childItems()) stack.append(k);
                    }
                }
            }
            if (!view) {
                qWarning("solidterm: autoscrolltest found no TerminalView");
                return;
            }
            // Enough lines that a handful of autoscroll ticks can't possibly reach the top on their own.
            view->sendText(QStringLiteral("for i in $(seq 1 400); do echo scrollback-line-$i; done\n"));

            QTimer::singleShot(1800, window, [view, window] {
                const int offsetBefore = view->debugScrollOffset();
                const QPointF centre = view->mapToScene(QPointF(view->width() / 2, view->height() / 2));
                const QPointF above = view->mapToScene(QPointF(view->width() / 2, -40)); // 40px past the top edge
                const QPointF gCentre = window->mapToGlobal(centre.toPoint());
                const QPointF gAbove = window->mapToGlobal(above.toPoint());

                QWindowSystemInterface::handleMouseEvent(window, centre, gCentre, Qt::LeftButton, Qt::LeftButton,
                    QEvent::MouseButtonPress, Qt::NoModifier);
                QWindowSystemInterface::handleMouseEvent(window, above, gAbove, Qt::LeftButton, Qt::NoButton,
                    QEvent::MouseMove, Qt::NoModifier);

                // No further movement from here — the timer alone must keep scrolling for ~500ms
                // (several ticks at kAutoscrollTimerMs=40ms).
                QTimer::singleShot(800, window, [view, window, above, gAbove, offsetBefore] {
                    const int offsetDuring = view->debugScrollOffset();
                    QWindowSystemInterface::handleMouseEvent(window, above, gAbove, Qt::NoButton, Qt::LeftButton,
                        QEvent::MouseButtonRelease, Qt::NoModifier);
                    view->copySelection();
                    const QString text = QGuiApplication::clipboard()->text(); // copySelection() uses the regular clipboard
                    const int lines = text.isEmpty() ? 0 : text.count(QLatin1Char('\n')) + 1;
                    qInfo().noquote() << "solidterm: autoscrolltest offset before=" << offsetBefore
                                       << "during=" << offsetDuring << "selection lines=" << lines
                                       << "selection head=" << text.left(40).replace(QLatin1Char('\n'), QStringLiteral("\\n"));
                    QCoreApplication::quit();
                });
            });
        });
    }

    // Debug: SOLIDTERM_SCROLLBARTEST=<prefix> fills the scrollback, grabs it idle (bar faded, "auto"
    // default), wheels back over the terminal to pulse+move the bar and grabs again, then drags the
    // bar's thumb (real synthetic press/move/release on the thumb's OWN computed position — not a
    // method call) and grabs a third time, printing the offsets so the drag's effect is provable from
    // the log even without looking at the PNGs. Finally flips scrollbarMode to "never" and confirms
    // the bar disappears despite nonzero scrollback+offset.
    const QString scrollbarShot = qEnvironmentVariable("SOLIDTERM_SCROLLBARTEST");
    if (!scrollbarShot.isEmpty() && window) {
        QTimer::singleShot(1600, window, [&engine, window, scrollbarShot] {
            TerminalView *view = nullptr;
            for (QObject *o : engine.rootObjects()) {
                if (auto *w = qobject_cast<QQuickWindow *>(o)) {
                    QList<QQuickItem *> stack { w->contentItem() };
                    while (!stack.isEmpty() && !view) {
                        QQuickItem *it = stack.takeLast();
                        view = qobject_cast<TerminalView *>(it);
                        for (QQuickItem *k : it->childItems()) stack.append(k);
                    }
                }
            }
            if (!view) {
                qWarning("solidterm: scrollbartest found no TerminalView");
                return;
            }
            const auto grab = [window](const QString &tag) {
                int i = 0;
                for (QWindow *tw : QGuiApplication::topLevelWindows())
                    if (auto *qw = qobject_cast<QQuickWindow *>(tw))
                        qw->grabWindow().save(tag + QStringLiteral("-%1.png").arg(i++));
            };
            view->sendText(QStringLiteral("for i in $(seq 1 400); do echo scrollbar-line-$i; done\n"));

            // Idle: wait past the 1000ms fade so "auto" has faded back out, then grab.
            QTimer::singleShot(4000, window, [view, window, scrollbarShot, grab] {
                qInfo() << "solidterm: scrollbartest idle offset=" << view->debugScrollOffset()
                        << "scrollbackCount=" << view->scrollbackCount();
                grab(scrollbarShot + QStringLiteral("-1-idle"));

                // Wheel-scroll back over the terminal body (pulses + moves the bar).
                const QPointF centre = view->mapToScene(QPointF(view->width() / 2, view->height() / 2));
                const QPointF g = window->mapToGlobal(centre.toPoint());
                QWindowSystemInterface::handleWheelEvent(window, centre, g, QPoint(0, 0), QPoint(0, 1200));

                QTimer::singleShot(200, window, [view, window, scrollbarShot, grab] {
                    const int afterWheel = view->debugScrollOffset();
                    qInfo() << "solidterm: scrollbartest after wheel offset=" << afterWheel;
                    grab(scrollbarShot + QStringLiteral("-2-wheel"));

                    // Drag the thumb: compute its CURRENT y from the same geometry the bar itself
                    // uses (viewport/(scrollback+rows) height, position from scrollOffset), press
                    // there, move it down (toward live), release, and prove the offset dropped.
                    const int rows = view->viewportRows();
                    const int sbSize = view->scrollbackCount();
                    const qreal trackH = view->height();
                    const qreal thumbH = qMax(24.0, (qreal(rows) / qMax(1, sbSize + rows)) * trackH);
                    const qreal fracTop = sbSize > 0 ? qBound(0.0, (sbSize - qreal(afterWheel)) / sbSize, 1.0) : 1.0;
                    const qreal thumbY = fracTop * (trackH - thumbH);
                    const QPointF thumbMid = view->mapToScene(QPointF(view->width() - 6, thumbY + thumbH / 2));
                    const QPointF thumbEnd = view->mapToScene(QPointF(view->width() - 6, thumbY + thumbH / 2 + 80));
                    const QPointF gThumbMid = window->mapToGlobal(thumbMid.toPoint());
                    const QPointF gThumbEnd = window->mapToGlobal(thumbEnd.toPoint());

                    // QWindowSystemInterface, not a raw QMouseEvent via sendEvent — see the
                    // AUTOSCROLLTEST comment above: a hand-built QMouseEvent doesn't carry Qt's own
                    // QPointingDevice grab state across calls, so the drag's MOVE never reaches the
                    // thumb's MouseArea.
                    QWindowSystemInterface::handleMouseEvent(window, thumbMid, gThumbMid, Qt::LeftButton,
                        Qt::LeftButton, QEvent::MouseButtonPress, Qt::NoModifier);
                    QWindowSystemInterface::handleMouseEvent(window, thumbEnd, gThumbEnd, Qt::LeftButton,
                        Qt::NoButton, QEvent::MouseMove, Qt::NoModifier);
                    QWindowSystemInterface::handleMouseEvent(window, thumbEnd, gThumbEnd, Qt::NoButton,
                        Qt::LeftButton, QEvent::MouseButtonRelease, Qt::NoModifier);

                    QTimer::singleShot(200, window, [view, scrollbarShot, grab, afterWheel] {
                        const int afterDrag = view->debugScrollOffset();
                        qInfo() << "solidterm: scrollbartest after drag offset=" << afterDrag
                                << "(before drag:" << afterWheel << ")";
                        grab(scrollbarShot + QStringLiteral("-3-drag"));

                        // Alt screen (less/vim): the bar must hide even though scrollbackCount is
                        // still > 0 from before — see TerminalView::isAltScreen / hasScrollback.
                        view->sendText(QStringLiteral("less /etc/hostname\n"));
                        QTimer::singleShot(600, view, [view, scrollbarShot, grab] {
                            grab(scrollbarShot + QStringLiteral("-4-altscreen"));
                            view->sendText(QStringLiteral("q"));

                            QTimer::singleShot(400, view, [view, scrollbarShot, grab] {
                                // scrollbarMode="never" must hide the bar even with scrollback + offset > 0.
                                view->setScrollbarMode(QStringLiteral("never"));
                                QTimer::singleShot(250, qApp, [scrollbarShot, grab] {
                                    grab(scrollbarShot + QStringLiteral("-5-never"));
                                    QCoreApplication::quit();
                                });
                            });
                        });
                    });
                });
            });
        });
    }

    // Debug: SOLIDTERM_LINKTEST=1 prints a URL, hovers it (checks TerminalView::hoverLinkUrl), moves off
    // it (checks it clears), double-clicks it (checks the WHOLE url gets copied, not a word-bounded
    // chunk), then hovers it again right before a right-click (checks hoverLinkUrl is still correct —
    // that's exactly what the Solid ContextMenu's "Copy link"/"Open link" Show binding reads). Real
    // input via QWindowSystemInterface — see the AUTOSCROLLTEST comment for why (not a raw QMouseEvent).
    if (qEnvironmentVariableIsSet("SOLIDTERM_LINKTEST") && window) {
        QTimer::singleShot(1600, window, [&engine, window] {
            TerminalView *view = nullptr;
            for (QObject *o : engine.rootObjects()) {
                if (auto *w = qobject_cast<QQuickWindow *>(o)) {
                    QList<QQuickItem *> stack { w->contentItem() };
                    while (!stack.isEmpty() && !view) {
                        QQuickItem *it = stack.takeLast();
                        view = qobject_cast<TerminalView *>(it);
                        for (QQuickItem *k : it->childItems()) stack.append(k);
                    }
                }
            }
            if (!view) {
                qWarning("solidterm: linktest found no TerminalView");
                return;
            }
            const QString wantUrl = QStringLiteral("https://example.com/some/path?q=1");
            view->sendText(QStringLiteral("printf 'see %s here\\n' ") + wantUrl + QStringLiteral("\n"));

            QTimer::singleShot(2500, window, [view, window, wantUrl] {
                // Row 0 printed "see <url> here". Rather than compute the exact cell (font-metric
                // dependent), probe left to right in small steps and ask hoverLinkUrl() — robust to
                // whatever font/size is configured, and exercises the real per-pixel-to-cell mapping.
                QString foundAt;
                QPointF foundScene;
                // Scan rows too — exactly where "see <url> here" lands depends on however many prompt/
                // banner lines came before it, not just column position.
                for (int rowPx = 4; rowPx < 400 && foundAt.isEmpty(); rowPx += 16) {
                    for (int col = 0; col < 120 && foundAt.isEmpty(); ++col) {
                        const QPointF probe = view->mapToScene(QPointF(col * 4.0 + 2, rowPx));
                        const QPointF g = window->mapToGlobal(probe.toPoint());
                        QWindowSystemInterface::handleMouseEvent(window, probe, g, Qt::NoButton, Qt::NoButton,
                            QEvent::MouseMove, Qt::NoModifier);
                        QCoreApplication::processEvents();
                        if (view->hoverLinkUrl() == wantUrl) {
                            foundAt = view->hoverLinkUrl();
                            foundScene = probe;
                        }
                    }
                }
                qInfo().noquote() << "solidterm: linktest hover found=" << (foundAt == wantUrl) << "url=" << foundAt;

                // Move off the link (top-left corner, row 0 col 0 is "s" of "see" — not part of the URL).
                const QPointF off = view->mapToScene(QPointF(1, 4));
                const QPointF gOff = window->mapToGlobal(off.toPoint());
                QWindowSystemInterface::handleMouseEvent(window, off, gOff, Qt::NoButton, Qt::NoButton,
                    QEvent::MouseMove, Qt::NoModifier);
                QCoreApplication::processEvents();
                qInfo() << "solidterm: linktest hover cleared=" << view->hoverLinkUrl().isEmpty();

                if (foundAt.isEmpty()) {
                    QCoreApplication::quit();
                    return;
                }

                // Double-click the link: must select the WHOLE url (copySelection + read the clipboard),
                // not a word-bounded chunk (selectWordAt would stop at '/', ':', '?' etc.). Two rapid
                // press/release pairs — Qt's own input stack synthesizes the double-click from their
                // timing/position, the same as two real clicks would (not a literal DblClick WSI event,
                // which the platform layer doesn't take as input — it's an OUTPUT of this synthesis).
                const QPointF g2 = window->mapToGlobal(foundScene.toPoint());
                for (int click = 0; click < 2; ++click) {
                    QWindowSystemInterface::handleMouseEvent(window, foundScene, g2, Qt::LeftButton, Qt::LeftButton,
                        QEvent::MouseButtonPress, Qt::NoModifier);
                    QWindowSystemInterface::handleMouseEvent(window, foundScene, g2, Qt::NoButton, Qt::LeftButton,
                        QEvent::MouseButtonRelease, Qt::NoModifier);
                    QCoreApplication::processEvents();
                }
                view->copySelection();
                const QString selected = QGuiApplication::clipboard()->text();
                qInfo().noquote() << "solidterm: linktest doubleclick selected=" << selected
                                   << "matchesWholeUrl=" << (selected == wantUrl);

                // Hover it again right before a right-click — exactly what the ContextMenu's Show
                // binding reads at open time (term.tsx: hoverLinkUrl() !== "").
                QWindowSystemInterface::handleMouseEvent(window, foundScene, g2, Qt::NoButton, Qt::NoButton,
                    QEvent::MouseMove, Qt::NoModifier);
                QCoreApplication::processEvents();
                qInfo().noquote() << "solidterm: linktest before right-click hoverLinkUrl="
                                   << view->hoverLinkUrl() << "readyForContextMenu="
                                   << (view->hoverLinkUrl() == wantUrl);
                QCoreApplication::quit();
            });
        });
    }

// Debug: SOLIDTERM_PAUSETEST=1 proves the owner's ask live ("quando seleciono texto o stdout tem
// que travar"): starts a fast, continuous counter, presses+drags a LOCAL selection (no mouse
// reporting active) inside the view, holds it with NO further movement for ~1s, and logs
// scrollbackCount (new output arriving pushes old live rows into scrollback — a visible proxy) and
// TerminalView::outputPaused across press/hold/release/resume. Must show frozen counts + paused=true
// for the whole hold, then paused=false and growing counts again after release. Real input via
// QWindowSystemInterface — see the AUTOSCROLLTEST comment above for why (not a raw QMouseEvent).
    // Debug/CI: SOLIDTERM_THREADSTRESS=<ms> — race harness for the parser thread (run it under
    // ThreadSanitizer with a flooding SHELL, e.g. scripts/solidterm-threadstress.sh). Every 5 ms it
    // does one GUI-side thing that touches the screen while the parser writes it: search / next /
    // clear, scroll back and to live, hover (link scan), a selection drag (pause → copy → resume),
    // a key, a scrollback-limit change, a window resize, a wheel notch, and every few hundred steps
    // a split (a second flooding pane) or closing the focused pane mid-flood. Quits after <ms>.
    if (const int stressMs = qEnvironmentVariableIntValue("SOLIDTERM_THREADSTRESS"); stressMs > 0 && window) {
        QTimer::singleShot(1500, window, [window, stressMs] {
            auto find = [window](auto *dummy) {
                using T = std::remove_pointer_t<decltype(dummy)>;
                QList<QQuickItem *> stack { window->contentItem() };
                while (!stack.isEmpty()) {
                    QQuickItem *it = stack.takeLast();
                    if (auto *t = qobject_cast<T *>(it))
                        return t;
                    for (QQuickItem *k : it->childItems()) stack.append(k);
                }
                return static_cast<T *>(nullptr);
            };
            auto *timer = new QTimer(window);
            auto *step = new int(0);
            auto *clock = new QElapsedTimer;
            clock->start();
            timer->setInterval(5);
            QObject::connect(timer, &QTimer::timeout, window, [=] {
                if (clock->elapsed() > stressMs) {
                    timer->stop();
                    qInfo("solidterm: threadstress done, %d steps", *step);
                    QCoreApplication::quit();
                    return;
                }
                TerminalView *view = find(static_cast<TerminalView *>(nullptr));
                TerminalPanes *panes = find(static_cast<TerminalPanes *>(nullptr));
                if (!view)
                    return;
                const int n = (*step)++;
                const QPointF a = view->mapToScene(QPointF(view->width() * 0.3, view->height() * 0.4));
                const QPointF b = view->mapToScene(QPointF(view->width() * 0.7, view->height() * 0.6));
                const QPointF h = view->mapToScene(QPointF((n * 37) % qMax(1, int(view->width())),
                                                           (n * 53) % qMax(1, int(view->height()))));
                auto mouse = [window](QPointF p, Qt::MouseButtons buttons, Qt::MouseButton button, QEvent::Type type) {
                    QWindowSystemInterface::handleMouseEvent(window, p, window->mapToGlobal(p.toPoint()), buttons,
                                                             button, type);
                };
                if (panes && n % 400 == 200)
                    panes->split(n % 800 == 200 ? 1 : 2);
                if (panes && n % 400 == 300)
                    panes->closeFocused(); // closes a flooding pane (the last one quits the app: keep one)
                switch (n % 14) {
                case 0: view->search(QStringLiteral("7")); break;
                case 1: view->searchNext(); break;
                case 2: view->clearSearch(); break;
                case 3: view->scrollTo(view->scrollbackCount() / 2); break;
                case 4: view->scrollTo(0); break;
                case 5: mouse(h, Qt::NoButton, Qt::NoButton, QEvent::MouseMove); break; // hover: link scan
                case 6: mouse(a, Qt::LeftButton, Qt::LeftButton, QEvent::MouseButtonPress); break; // pause
                case 7: mouse(b, Qt::LeftButton, Qt::NoButton, QEvent::MouseMove); break;
                case 8: mouse(b, Qt::NoButton, Qt::LeftButton, QEvent::MouseButtonRelease); break; // copy + resume
                case 9:
                    QWindowSystemInterface::handleKeyEvent(window, QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, QStringLiteral("a"));
                    QWindowSystemInterface::handleKeyEvent(window, QEvent::KeyRelease, Qt::Key_A, Qt::NoModifier, QStringLiteral("a"));
                    break;
                case 10: view->setScrollbackLimit((n / 14) % 2 ? 300 : 8000); break;
                case 11: window->resize(window->width() + ((n / 14) % 2 ? 37 : -37), window->height() + ((n / 14) % 2 ? 19 : -19)); break;
                case 12: QWindowSystemInterface::handleWheelEvent(window, h, window->mapToGlobal(h.toPoint()), QPoint(0, 0), QPoint(0, 120)); break;
                case 13: view->copySelection(); break;
                }
            });
            timer->start();
        });
    }

    if (qEnvironmentVariableIsSet("SOLIDTERM_PAUSETEST") && window) {
        QTimer::singleShot(1600, window, [&engine, window] {
            TerminalView *view = nullptr;
            for (QObject *o : engine.rootObjects()) {
                if (auto *w = qobject_cast<QQuickWindow *>(o)) {
                    QList<QQuickItem *> stack { w->contentItem() };
                    while (!stack.isEmpty() && !view) {
                        QQuickItem *it = stack.takeLast();
                        view = qobject_cast<TerminalView *>(it);
                        for (QQuickItem *k : it->childItems()) stack.append(k);
                    }
                }
            }
            if (!view) {
                qWarning("solidterm: pausetest found no TerminalView");
                QCoreApplication::quit();
                return;
            }
            // A continuous counter — exactly the "program output would move the text under the
            // selection" scenario the owner described. Throttled (sleep 0.02 ~ 50 lines/s): run
            // flat-out, scrollbackCount (our freeze/growth proxy below) saturates at the 8000-line
            // default limit within a fraction of a second and then stays constant FOREVER —
            // indistinguishable from "frozen". Slowed down, it stays comfortably incremental for
            // the whole test.
            view->sendText(QStringLiteral(
                "i=1; while true; do echo pausetest-line-$i; i=$((i+1)); sleep 0.02; done\n"));

            QTimer::singleShot(1200, window, [view, window] {
                const int countBeforePress = view->scrollbackCount();
                const QPointF p1 = view->mapToScene(QPointF(10, view->height() / 2));
                const QPointF p2 = view->mapToScene(QPointF(view->width() / 2, view->height() / 2));
                const QPointF g1 = window->mapToGlobal(p1.toPoint());
                const QPointF g2 = window->mapToGlobal(p2.toPoint());

                QWindowSystemInterface::handleMouseEvent(window, p1, g1, Qt::LeftButton, Qt::LeftButton,
                    QEvent::MouseButtonPress, Qt::NoModifier);
                QWindowSystemInterface::handleMouseEvent(window, p2, g2, Qt::LeftButton, Qt::NoButton,
                    QEvent::MouseMove, Qt::NoModifier);
                QCoreApplication::processEvents();

                const bool pausedAtPress = view->isOutputPaused();
                const int countAtPress = view->scrollbackCount();

                // Hold for ~1s with NO further movement — output must stay frozen the whole time.
                QTimer::singleShot(1000, window, [view, window, p2, g2, countBeforePress, countAtPress, pausedAtPress] {
                    const int countAfterHold = view->scrollbackCount();
                    const bool stillPaused = view->isOutputPaused();
                    qInfo().noquote() << "solidterm: pausetest before-press count=" << countBeforePress
                                       << "at-press count=" << countAtPress << "paused-at-press=" << pausedAtPress
                                       << "after-1s-hold count=" << countAfterHold << "still-paused=" << stillPaused
                                       << "frozen=" << (countAfterHold == countAtPress);

                    QWindowSystemInterface::handleMouseEvent(window, p2, g2, Qt::NoButton, Qt::LeftButton,
                        QEvent::MouseButtonRelease, Qt::NoModifier);
                    QCoreApplication::processEvents();
                    const bool pausedAfterRelease = view->isOutputPaused();

                    QTimer::singleShot(600, window, [view, countAfterHold, pausedAfterRelease] {
                        const int countAfterResume = view->scrollbackCount();
                        qInfo().noquote() << "solidterm: pausetest paused-after-release=" << pausedAfterRelease
                                           << "after-resume count=" << countAfterResume
                                           << "resumed=" << (countAfterResume > countAfterHold);
                        QCoreApplication::quit();
                    });
                });
            });
        });
    }

    // Debug/CI: SOLIDTERM_SELANCHORTEST=1 — the selection sticks to its TEXT (lineanchor.h), owner
    // report "seleção não rola com a tela". With a small scrollback limit (200) already FULL — where
    // the old ring-index rows drifted — it prints a marker word, drag-selects it (real input via
    // QWindowSystemInterface, see AUTOSCROLLTEST), lets the shell print 7 more lines and checks the
    // selected text is unchanged and its highlight row moved up exactly as far as the marker did;
    // scrolls back 5 lines (the highlight follows the view); then prints far more than the limit
    // and checks the selection was cleared with its evicted text. Exits 0 on PASS, 1 on FAIL.
    if (qEnvironmentVariableIsSet("SOLIDTERM_SELANCHORTEST") && window) {
        QTimer::singleShot(1600, window, [&engine, window] {
            TerminalView *view = nullptr;
            for (QObject *o : engine.rootObjects()) {
                if (auto *w = qobject_cast<QQuickWindow *>(o)) {
                    QList<QQuickItem *> stack { w->contentItem() };
                    while (!stack.isEmpty() && !view) {
                        QQuickItem *it = stack.takeLast();
                        view = qobject_cast<TerminalView *>(it);
                        for (QQuickItem *k : it->childItems()) stack.append(k);
                    }
                }
            }
            if (!view) {
                qWarning("solidterm: selanchortest found no TerminalView");
                QCoreApplication::exit(1);
                return;
            }
            view->setScrollbackLimit(200);
            // 'sel%s' + anchormark: only the OUTPUT line holds "selanchormark", not the echoed command.
            view->sendText(QStringLiteral("seq 1 300; printf 'sel%s\\n' anchormark; seq 1 3\n"));

            QTimer::singleShot(2000, window, [view, window] {
                const QString word = QStringLiteral("selanchormark");
                const int markRow0 = view->debugFindRow(word);
                const int sbAtStart = view->scrollbackCount();
                if (markRow0 < 0) {
                    qWarning("solidterm: selanchortest marker not on screen");
                    QCoreApplication::exit(1);
                    return;
                }
                // Drag-select the word: press on its first cell, move to its last, release.
                const QPointF p0 = view->mapToScene(view->debugCellCenter(markRow0, 0));
                const QPointF p1 = view->mapToScene(view->debugCellCenter(markRow0, int(word.size()) - 1));
                const QPointF g0 = window->mapToGlobal(p0.toPoint()), g1 = window->mapToGlobal(p1.toPoint());
                QWindowSystemInterface::handleMouseEvent(window, p0, g0, Qt::LeftButton, Qt::LeftButton,
                    QEvent::MouseButtonPress, Qt::NoModifier);
                QWindowSystemInterface::handleMouseEvent(window, p1, g1, Qt::LeftButton, Qt::NoButton,
                    QEvent::MouseMove, Qt::NoModifier);
                QWindowSystemInterface::handleMouseEvent(window, p1, g1, Qt::NoButton, Qt::LeftButton,
                    QEvent::MouseButtonRelease, Qt::NoModifier);
                QCoreApplication::processEvents();
                const QString sel0 = view->debugSelectedText();
                const int selRow0 = view->debugSelectionRow();
                // SOLIDTERM_SELANCHORSHOT=<prefix>: grab the window before and after the output, to see it.
                const QString shot = qEnvironmentVariable("SOLIDTERM_SELANCHORSHOT");
                if (!shot.isEmpty())
                    window->grabWindow().save(shot + QStringLiteral("-1-selected.png"));
                view->sendText(QStringLiteral("seq 1 7\n"));

                QTimer::singleShot(1500, window, [view, window, word, markRow0, sbAtStart, sel0, selRow0, shot] {
                    if (!shot.isEmpty())
                        window->grabWindow().save(shot + QStringLiteral("-2-after-output.png"));
                    const QString sel1 = view->debugSelectedText();
                    const int selRow1 = view->debugSelectionRow();
                    const int markRow1 = view->debugFindRow(word);
                    const bool moved = selRow1 == markRow1 && markRow1 < markRow0
                        && selRow0 - selRow1 == markRow0 - markRow1;
                    const bool phase1 = sel0 == word && selRow0 == markRow0 && sel1 == sel0 && moved
                        && sbAtStart == 200;
                    qInfo().noquote() << "solidterm: selanchortest scrollback=" << sbAtStart << "(limit 200, full)"
                                       << "marker row" << markRow0 << "->" << markRow1
                                       << "| highlight row" << selRow0 << "->" << selRow1
                                       << "| text" << sel0 << "->" << sel1 << "| follows-text=" << phase1;

                    view->scrollTo(5); // the user scrolls back: the highlight goes down with its text
                    const int selRowScrolled = view->debugSelectionRow();
                    const bool phase2 = selRowScrolled == selRow1 + 5 && view->debugSelectedText() == word;
                    qInfo().noquote() << "solidterm: selanchortest scrolled back 5: highlight row" << selRowScrolled
                                       << "follows-scroll=" << phase2;
                    view->scrollTo(0);
                    view->sendText(QStringLiteral("seq 1 400\n")); // > limit + rows: the marker is evicted

                    QTimer::singleShot(2000, window, [view, phase1, phase2] {
                        const bool phase3 = !view->hasSelection() && view->debugSelectedText().isEmpty();
                        qInfo().noquote() << "solidterm: selanchortest after evicting the text: hasSelection="
                                           << view->hasSelection() << "cleared=" << phase3;
                        const bool ok = phase1 && phase2 && phase3;
                        qInfo().noquote() << "solidterm: selanchortest" << (ok ? "PASS" : "FAIL");
                        QCoreApplication::exit(ok ? 0 : 1);
                    });
                });
            });
        });
    }

// Debug: SOLIDTERM_FINDTEST="query" opens the search bar + runs a search after a settle.
    const QString findTest = qEnvironmentVariable("SOLIDTERM_FINDTEST");
    if (!findTest.isEmpty() && window) {
        QTimer::singleShot(1600, window, [&engine, findTest] {
            for (QObject *o : engine.rootObjects()) {
                if (auto *w = qobject_cast<QQuickWindow *>(o)) {
                    QList<QQuickItem *> stack { w->contentItem() };
                    while (!stack.isEmpty()) {
                        QQuickItem *it = stack.takeLast();
                        if (it->property("searchOpen").isValid()) it->setProperty("searchOpen", true);
                        if (auto *tabs = qobject_cast<TerminalTabs *>(it)) tabs->searchFocused(findTest);
                        for (QQuickItem *k : it->childItems()) stack.append(k);
                    }
                }
            }
        });
    }

    return app.exec();
}
