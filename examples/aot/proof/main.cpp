// M-AOT-0 AOT binary (hand-written proof). Mirrors src/loader.cpp minus QML loading: it wires the
// same engine/theme/layout/context, builds the generated scene in C++, hosts it in a QQuickWindow,
// and supports the same offscreen `--grab` used by the twin-render harness.
#include "counterapp.h"

#include "qmlcss/QMLCss.h"
#include "qmlcss/csslayout.h"
#include "qmlcss/csstheme.h"
#include "widgets/solidwidgets.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QMouseEvent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>

int main(int argc, char **argv)
{
    QApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QApplication app(argc, argv);

    QmlCss::registerTypes();
    SolidWidgets::registerTypes();

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({ QStringLiteral("css"),
                       QStringLiteral("CSS file(s), layered in order (repeatable)."),
                       QStringLiteral("path") });
    parser.addOption({ QStringLiteral("width"), QStringLiteral("Window width."),
                       QStringLiteral("px"), QStringLiteral("420") });
    parser.addOption({ QStringLiteral("height"), QStringLiteral("Window height."),
                       QStringLiteral("px"), QStringLiteral("180") });
    parser.addOption({ QStringLiteral("grab"),
                       QStringLiteral("Render one frame to this PNG and exit (offscreen)."),
                       QStringLiteral("png") });
    parser.addOption({ QStringLiteral("click"),
                       QStringLiteral("Synthesize a left click at \"x,y\" before grabbing (repeatable)."),
                       QStringLiteral("x,y") });
    parser.process(app);

    const int w = parser.value(QStringLiteral("width")).toInt();
    const int h = parser.value(QStringLiteral("height")).toInt();

    QQmlEngine engine;
    QmlCss::CssTheme theme;
    QmlCss::CssLayoutEngine layout(&theme);
    QQmlContext *ctx = engine.rootContext();
    ctx->setContextProperty(QStringLiteral("cssTheme"), &theme);
    ctx->setContextProperty(QStringLiteral("cssLayout"), &layout);

    // Same layered load as the loader: base reset first, then the app sheet(s).
    theme.loadLayered(parser.values(QStringLiteral("css")));

    QQuickWindow window;
    window.setWidth(w);
    window.setHeight(h);
    window.setTitle(QStringLiteral("AOT Counter"));
    theme.setViewport(w, h);
    QObject::connect(&window, &QQuickWindow::widthChanged, &theme,
                     [&] { theme.setViewport(window.width(), window.height()); });
    QObject::connect(&window, &QQuickWindow::heightChanged, &theme,
                     [&] { theme.setViewport(window.width(), window.height()); });

    QQuickItem *root = aot::buildCounterApp(ctx);
    root->setParentItem(window.contentItem());
    root->setSize(QSizeF(w, h));
    // Keep the window box filling the surface on resize (the layout re-flows on geometryChange).
    QObject::connect(&window, &QQuickWindow::widthChanged, root,
                     [root, &window] { root->setWidth(window.width()); });
    QObject::connect(&window, &QQuickWindow::heightChanged, root,
                     [root, &window] { root->setHeight(window.height()); });

    window.show();

    // Synthetic clicks (proves the handler wiring: onClicked → count = count + 1). Fired at half the
    // grab delay so the increment lands before the single grabbed frame.
    const QStringList clicks = parser.values(QStringLiteral("click"));
    const int grabMs = qEnvironmentVariableIntValue("SQ_GRAB_MS") > 0
        ? qEnvironmentVariableIntValue("SQ_GRAB_MS")
        : 1400;
    if (!clicks.isEmpty()) {
        QTimer::singleShot(grabMs / 2, &window, [&window, clicks] {
            for (const QString &c : clicks) {
                const QStringList xy = c.split(QLatin1Char(','));
                if (xy.size() != 2)
                    continue;
                const QPointF p(xy[0].toDouble(), xy[1].toDouble());
                const QPointF g = window.mapToGlobal(p);
                QMouseEvent press(QEvent::MouseButtonPress, p, g, Qt::LeftButton, Qt::LeftButton,
                                  Qt::NoModifier);
                QMouseEvent release(QEvent::MouseButtonRelease, p, g, Qt::LeftButton, Qt::NoButton,
                                    Qt::NoModifier);
                QCoreApplication::sendEvent(&window, &press);
                QCoreApplication::sendEvent(&window, &release);
            }
        });
    }

    if (parser.isSet(QStringLiteral("grab"))) {
        const QString grabPath = parser.value(QStringLiteral("grab"));
        QTimer::singleShot(grabMs, &window, [&window, grabPath] {
            const QImage frame = window.grabWindow();
            if (frame.save(grabPath))
                qInfo("aot-counter: wrote %s", qUtf8Printable(grabPath));
            else
                qWarning("aot-counter: failed to write %s", qUtf8Printable(grabPath));
            QCoreApplication::quit();
        });
    }

    return app.exec();
}
