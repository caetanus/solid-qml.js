// solidterm — a real, daily-driver terminal (owner 2026-07-22: tilix-like, "esse terminal eu
// pretendo usar mesmo"). The app IS the two-way embed story in production shape: its own C++
// (PtySession + the libvterm-backed TerminalView) registered as the `SolidTerm` QML module and
// consumed from the solid TSX UI via `import { TerminalView } from "qml:SolidTerm"`; the solid
// runtime hosted through libsolidqml's embed surface. Rendering rides the Quick scene graph
// (GL/RHI accelerated).
//
// App dir resolution: $SOLIDTERM_DIR > <bindir>/../share/solidterm > the source tree (dev).
#include "embed/solidqmlembed.h"
#include "native/systemtheme.h"
#include "native/keyrecorder.h"
#include "native/paneheader.h"
#include "native/termconfig.h"
#include "native/terminalpanes.h"
#include "native/terminaltabs.h"
#include "native/terminalview.h"

#include <QApplication>
#include <QDir>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QKeyEvent>
#include <QKeySequence>
#include <QTimer>
#include <QGuiApplication>
#include <QClipboard>
#include <QSurfaceFormat>

// App-level key filter: F11 toggles fullscreen. Installed on the QApplication so it's caught before
// the focused TerminalView consumes it (no Q_OBJECT → no moc; eventFilter is a plain virtual).
class KeyFilter : public QObject {
public:
    QQuickWindow *window = nullptr;
    bool eventFilter(QObject *obj, QEvent *event) override {
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

int main(int argc, char **argv)
{
    // Request an alpha channel on the window surface so translucent chrome (uiOpacity < 1) reveals
    // the desktop behind the whole window.
    {
        QSurfaceFormat fmt = QSurfaceFormat::defaultFormat();
        fmt.setAlphaBufferSize(8);
        QSurfaceFormat::setDefaultFormat(fmt);
    }
    QApplication app(argc, argv);
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
    engine.rootContext()->setContextProperty(QStringLiteral("termConfig"), config);
    SolidQmlEmbed::loadCss(&engine, QUrl(QStringLiteral("App.generated.css"))); // structural
    SolidQmlEmbed::loadCssString(&engine, sysTheme->styleSheet());              // palette layer
    QObject::connect(sysTheme, &SystemTheme::changed, &engine, [&engine, sysTheme] {
        SolidQmlEmbed::loadCssString(&engine, sysTheme->styleSheet());
    });

    const QUrl windowUrl = appDir.resolved(QUrl(QStringLiteral("App.generated.qml")));
    TerminalPanes::setWindowUrl(windowUrl); // so a pane detach can spawn a full new solidterm window
    engine.load(windowUrl);
    if (engine.rootObjects().isEmpty())
        return 1;
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    if (window) {
        SolidQmlEmbed::attachWindow(&engine, window);
        auto *keyFilter = new KeyFilter; // F11 → fullscreen (app-level, before the terminal)
        keyFilter->window = window;
        app.installEventFilter(keyFilter);
        // Clear to transparent only when translucent (uiOpacity < 1) so the desktop shows through the
        // transparent app root; otherwise clear to the solid window colour. Tracks live changes.
        const auto retint = [window, sysTheme] {
            window->setColor(sysTheme->uiOpacity() < 1.0 ? QColor(Qt::transparent) : sysTheme->window());
        };
        retint();
        QObject::connect(sysTheme, &SystemTheme::changed, window, retint);
    }

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
    // into the first window, then grabs — exercises the full detach→reattach round trip.
    if (qEnvironmentVariableIsSet("SOLIDTERM_REATTACH") && window) {
        QTimer::singleShot(3200, window, [] {
            TerminalPanes::debugReattachLastToFirst();
            const QString pfx = qEnvironmentVariable("SOLIDTERM_DETACHSHOT");
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

    // Debug: SOLIDTERM_PANEMENU=1 opens the pane header dropdown after a settle.
    if (qEnvironmentVariableIsSet("SOLIDTERM_PANEMENU") && window) {
        QTimer::singleShot(1600, window, [&engine] {
            for (QObject *o : engine.rootObjects())
                if (auto *w = qobject_cast<QQuickWindow *>(o)) {
                    QList<QQuickItem *> stack { w->contentItem() };
                    while (!stack.isEmpty()) {
                        QQuickItem *it = stack.takeLast();
                        if (it->property("paneMenuOpen").isValid()) {
                            it->setProperty("paneMenuX", 40); it->setProperty("paneMenuY", 30);
                            it->setProperty("paneMenuOpen", true);
                        }
                        for (QQuickItem *k : it->childItems()) stack.append(k);
                    }
                }
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
