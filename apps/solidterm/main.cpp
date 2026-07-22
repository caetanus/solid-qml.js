// solidterm — a real, daily-driver terminal (owner 2026-07-22: tilix-like, "esse terminal eu
// pretendo usar mesmo"). The app IS the two-way embed story in production shape: its own C++
// (PtySession + the libvterm-backed TerminalView) registered as the `SolidTerm` QML module and
// consumed from the solid TSX UI via `import { TerminalView } from "qml:SolidTerm"`; the solid
// runtime hosted through libsolidqml's embed surface. Rendering rides the Quick scene graph
// (GL/RHI accelerated).
//
// App dir resolution: $SOLIDTERM_DIR > <bindir>/../share/solidterm > the source tree (dev).
#include "embed/solidqmlembed.h"
#include "native/terminalview.h"

#include <QApplication>
#include <QDir>
#include <QQmlApplicationEngine>
#include <QQuickWindow>

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

    QQmlApplicationEngine engine;
    SolidQmlEmbed::init(&engine, appDir);
    SolidQmlEmbed::loadCss(&engine, QUrl(QStringLiteral("App.generated.css")));
    engine.load(appDir.resolved(QUrl(QStringLiteral("App.generated.qml"))));
    if (engine.rootObjects().isEmpty())
        return 1;
    if (auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first()))
        SolidQmlEmbed::attachWindow(&engine, window);
    return app.exec();
}
