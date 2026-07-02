// Minimal standalone host for the qml-css-engine — a "hello world" that proves the engine builds
// and runs entirely on its own (no app framework, no transpiler, nothing project-specific).
//
// Integration in three steps, exactly as a consumer would do it:
//   1. construct a CssTheme and load a stylesheet,
//   2. construct a CssLayoutEngine over that theme,
//   3. expose both to QML as the context properties `cssTheme` and `cssLayout`,
// then use the `qrc:/qmlcss` components (CssRect / CssText / ...) from QML.
//
// Pass `--grab out.png` to render one frame headless (with QT_QPA_PLATFORM=offscreen) and exit —
// used by the build's smoke check.

#include "qmlcss/csslayout.h"
#include "qmlcss/cssrect.h"
#include "qmlcss/csstext.h"
#include "qmlcss/csstheme.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QtQml/qqml>
#include <QQmlContext>
#include <QQuickWindow>
#include <QTimer>
#include <QUrl>

#ifndef HELLO_DIR
#define HELLO_DIR "."
#endif

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);

    // CssRect is a classic C++ type; register it into the `qmlcss` module so Hello.qml's
    // `import qmlcss` resolves it (the box + layout container primitive).
    qmlRegisterType<CssRect>("qmlcss", 1, 0, "CssRect");
    qmlRegisterType<CssText>("qmlcss", 1, 0, "CssText");

    const QString dir = QStringLiteral(HELLO_DIR);

    CssTheme theme;
    theme.loadLayered({ dir + QStringLiteral("/hello.css") });

    CssLayoutEngine layout(&theme);

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layout);
    engine.load(QUrl::fromLocalFile(dir + QStringLiteral("/Hello.qml")));

    if (engine.rootObjects().isEmpty()) {
        qWarning("hello: failed to load Hello.qml");
        return 1;
    }

    // --grab <png>: render a single frame and quit (headless smoke check).
    QString grabPath;
    const QStringList args = app.arguments();
    const int gi = args.indexOf(QStringLiteral("--grab"));
    if (gi >= 0 && gi + 1 < args.size())
        grabPath = args.at(gi + 1);

    if (!grabPath.isEmpty()) {
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
        if (!window) { qWarning("hello: root is not a Window"); return 1; }
        int rc = 0;
        QObject::connect(window, &QQuickWindow::afterRendering, &app, [&] {
            static bool done = false;
            if (done) return;
            done = true;
            const QImage frame = window->grabWindow();
            rc = frame.save(grabPath) ? 0 : 2;
            QTimer::singleShot(0, &app, [&] { app.exit(rc); });
        }, Qt::QueuedConnection);
        // Safety timeout so the smoke check never hangs.
        QTimer::singleShot(5000, &app, [&] { app.exit(3); });
        const int code = app.exec();
        return code;
    }

    return app.exec();
}
