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
#include "native/terminalpanes.h"
#include "native/terminalview.h"

#include <QApplication>
#include <QDir>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QTimer>
#include <QGuiApplication>

int main(int argc, char **argv)
{
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

    QQmlApplicationEngine engine;
    SolidQmlEmbed::init(&engine, appDir);

    // Obey the local desktop theme: derive the app's colour layer from QPalette (which Qt takes
    // from the platform/GTK theme) and re-apply it live if the desktop theme changes.
    auto *sysTheme = new SystemTheme(&engine);
    engine.rootContext()->setContextProperty(QStringLiteral("sysTheme"), sysTheme);
    SolidQmlEmbed::loadCss(&engine, QUrl(QStringLiteral("App.generated.css"))); // structural
    SolidQmlEmbed::loadCssString(&engine, sysTheme->styleSheet());              // palette layer
    QObject::connect(sysTheme, &SystemTheme::changed, &engine, [&engine, sysTheme] {
        SolidQmlEmbed::loadCssString(&engine, sysTheme->styleSheet());
    });

    engine.load(appDir.resolved(QUrl(QStringLiteral("App.generated.qml"))));
    if (engine.rootObjects().isEmpty())
        return 1;
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    if (window)
        SolidQmlEmbed::attachWindow(&engine, window);

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
    return app.exec();
}
