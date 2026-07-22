// A deliberately VANILLA QtQuick app standing in for a pre-existing C++ project, adopting one
// solid-qml UI island (docs: the embed surface is solidqml/solidqmlembed.h — two calls total:
// link libsolidqml, SolidQmlEmbed::init(engine)). --grab <png> renders offscreen for the test
// harness (SQ_GRAB_MS to tune the settle delay).
#include "embed/solidqmlembed.h" // in-tree; installed consumers use <solidqml/solidqmlembed.h>

#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QTimer>

static QString args_qml()
{
    const QStringList args = QCoreApplication::arguments();
    const int i = args.indexOf(QStringLiteral("--qml"));
    return i >= 0 && i + 1 < args.size() ? args.at(i + 1) : QStringLiteral(EMBED_HOST_QML);
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QQmlApplicationEngine engine;

    // The ONE integration call: arms this (host-owned) engine with the solid-qml runtime.
    // appDir anchors island sources — here the repo's generated output.
    const QUrl appDir = QUrl::fromLocalFile(QStringLiteral(SOLIDQML_APP_DIR));
    SolidQmlEmbed::init(&engine, appDir);

    engine.load(QUrl::fromLocalFile(args_qml()));
    if (engine.rootObjects().isEmpty())
        return 1;

    const QStringList args = app.arguments();
    const int grabAt = args.indexOf(QStringLiteral("--grab"));
    if (grabAt >= 0 && grabAt + 1 < args.size()) {
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        const QString path = args.at(grabAt + 1);
        const int ms = qEnvironmentVariableIntValue("SQ_GRAB_MS") > 0
            ? qEnvironmentVariableIntValue("SQ_GRAB_MS") : 1200;
        QTimer::singleShot(ms, window, [window, path] {
            window->grabWindow().save(path);
            QCoreApplication::quit();
        });
    }
    return app.exec();
}
