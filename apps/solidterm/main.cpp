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
#include <QSurfaceFormat>

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

    engine.load(appDir.resolved(QUrl(QStringLiteral("App.generated.qml"))));
    if (engine.rootObjects().isEmpty())
        return 1;
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    if (window) {
        SolidQmlEmbed::attachWindow(&engine, window);
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

    return app.exec();
}
