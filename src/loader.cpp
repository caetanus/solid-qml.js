#include "qmlcss/QMLCss.h"
#include "shims/jspolyfill.h"
#include "shims/nodeshims.h"
#include "shims/tabstop.h"
#include "shims/webfetch.h"
#include "shims/weblocalstorage.h"
#include "shims/webplatform.h"
#include "shims/webtimers.h"

#include <QCommandLineParser>
#include <QApplication>
#include <QGuiApplication>
#include <QDebug>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlError>
#include <QQmlContext>
#include <QDir>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScreen>
#include <QStringList>
#include <QSurfaceFormat>
#include <QTimer>
#include <QUrl>

#include <functional>

#ifdef __linux__
#include <csignal>
#include <sys/prctl.h>
#endif

namespace {

QUrl qmlUrl(const QString &path)
{
    if (path.startsWith(QLatin1String("qrc:/")) || path.startsWith(QLatin1String("file:/")))
        return QUrl(path);
    return QUrl::fromLocalFile(path);
}

// Headless "mini-node": evaluate a JS/ES-module file directly in the V4 engine with the browser/host
// shims installed, no QML/Window. Lets us probe V4 JS behaviour and exercise mirrored npm modules
// (the file may `import` them) without scaffolding a throwaway .qml each time.
int runMiniNode(QGuiApplication &app, const QString &path)
{
    QQmlEngine engine;
    engine.installExtensions(QJSEngine::ConsoleExtension); // console.log/warn/error
    WebLocalStorage::install(&engine);
    WebFetch::install(&engine);
    WebTimers::install(&engine);
    WebPlatform::install(&engine);
    JsPolyfill::install(&engine);
    NodeShims::install(&engine);

    const QString abs = QFileInfo(path).absoluteFilePath();
    if (!QFileInfo::exists(abs)) {
        qWarning().noquote() << "mini-node: file not found:" << abs;
        return 1;
    }

    QJSValue ns = engine.importModule(abs);
    if (ns.isError()) {
        qWarning().noquote() << "mini-node: module error:" << ns.toString()
                             << "\n  at" << ns.property(QStringLiteral("fileName")).toString() + QLatin1Char(':')
                                            + ns.property(QStringLiteral("lineNumber")).toString();
        return 1;
    }
    // If the module default-exports a function, call it (lets a probe run code and/or return a promise).
    QJSValue def = ns.property(QStringLiteral("default"));
    if (def.isCallable()) {
        const QJSValue r = def.call();
        if (r.isError()) {
            qWarning().noquote() << "mini-node: default() threw:" << r.toString();
            return 1;
        }
    }
    // Pump the event loop briefly so timers / microtasks / fetch can resolve, then exit.
    bool msOk = false;
    int ms = qEnvironmentVariableIntValue("SQ_MININODE_MS", &msOk);
    if (!msOk || ms <= 0) ms = 400;
    QTimer::singleShot(ms, &app, [&app] { app.quit(); });
    return app.exec();
}

} // namespace

int main(int argc, char **argv)
{
#ifdef __linux__
    // Die with the parent (e.g. the Vite dev server that spawned us) so killing qmldev never
    // leaves an orphaned window serving stale code.
    prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
    // QApplication (not QGuiApplication): Qt Labs Platform's OS-native menu bar / menus / dialogs
    // need Qt Widgets. QApplication IS-A QGuiApplication, so runMiniNode(QGuiApplication&) is unaffected.
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("solid-qml-loader"));
    QApplication::setOrganizationName(QStringLiteral("solid-qml"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Load a Solid QML generated root."));
    parser.addHelpOption();
    parser.addOption({
        QStringLiteral("qml"),
        QStringLiteral("Root QML file to load. Generated output is loaded from the filesystem "
                       "(never baked into a qrc), so edits show on reload without rebuilding."),
        QStringLiteral("path"),
        QStringLiteral("qml/solidqml/App.generated.qml"),
    });
    parser.addOption({
        QStringLiteral("css"),
        QStringLiteral("CSS file(s) to load into cssTheme, layered in order (repeatable). "
                       "Pass a filesystem path for hot reload; defaults to the bundled sheet."),
        QStringLiteral("path"),
    });
    parser.addOption({ QStringLiteral("width"), QStringLiteral("Override the window width."), QStringLiteral("px") });
    parser.addOption({ QStringLiteral("height"), QStringLiteral("Override the window height."), QStringLiteral("px") });
    parser.addOption({
        QStringLiteral("grab"),
        QStringLiteral("Render one frame to this PNG and exit (works headless with "
                       "QT_QPA_PLATFORM=offscreen). For visual testing."),
        QStringLiteral("png"),
    });
    parser.addOption({
        QStringLiteral("resize-grab"),
        QStringLiteral("Dynamic resize test: after load, resize the Window through the sequence in "
                       "SQ_RESIZE_SEQ (e.g. \"760x520,1200x800,500x700\"; default that), let each "
                       "step settle, and grab a PNG per step to <prefix>-<i>-<WxH>.png. Exercises "
                       "live relayout on resize (unlike --grab, which reads a fresh startup frame)."),
        QStringLiteral("prefix"),
    });
    parser.addOption({
        QStringLiteral("watch"),
        QStringLiteral("Hot reload: reload the QML when its file changes (CSS files passed "
                       "via --css are already watched). Pass filesystem paths, not qrc."),
    });
    parser.addOption({
        QStringLiteral("mini-node"),
        QStringLiteral("Headless: run a JS/ES-module file directly in the V4 engine with the same "
                       "browser/host shims installed (no Window). Supports `import`, so mirrored "
                       "npm modules can be exercised. console.log goes to stderr. For probing "
                       "V4 JS behaviour and npm-module compat. SQ_MININODE_MS sets the async wait."),
        QStringLiteral("path"),
    });
    parser.process(app);

    const QString miniNodePath = parser.value(QStringLiteral("mini-node"));
    if (!miniNodePath.isEmpty())
        return runMiniNode(app, miniNodePath);

    QmlCss::CssTheme cssTheme;
    // Base layer: the app stylesheet(s) from disk/qrc. Inline-style rules synthesised by the
    // transpiler are appended on top later, from QML, via cssTheme.loadLayeredString().
    QStringList cssPaths = parser.values(QStringLiteral("css"));
    cssPaths.removeAll(QString());
    if (cssPaths.isEmpty())
        cssPaths << QStringLiteral("qml/solidqml/App.generated.css");
    // Shared base ("user-agent") layer first, so defaults (e.g. <hr>) match the web target;
    // the app sheet layers on top. Skipped silently if not found (e.g. an installed binary).
    const QString baseSheet = QStringLiteral("src/solid-qml/base.css");
    if (QFileInfo::exists(baseSheet))
        cssPaths.prepend(baseSheet);
    cssTheme.loadLayered(cssPaths);

    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setAlphaBufferSize(8);

    QSurfaceFormat::setDefaultFormat(format);

    bool okW = false, okH = false;
    const int forcedW = parser.value(QStringLiteral("width")).toInt(&okW);
    const int forcedH = parser.value(QStringLiteral("height")).toInt(&okH);

    // Wire a freshly-loaded Window to the engine: apply any size override and feed the
    // viewport so `@media`/`vw`/`vh` track the window (live on resize unless pinned).
    const auto wireWindow = [&](QQuickWindow *window) {
        if (!window)
            return;
        if (okW) window->setWidth(forcedW);
        if (okH) window->setHeight(forcedH);
        if (okW || okH) {
            cssTheme.setViewport(okW ? forcedW : window->width(), okH ? forcedH : window->height());
        } else {
            const auto sync = [&cssTheme, window] { cssTheme.setViewport(window->width(), window->height()); };
            sync();
            QObject::connect(window, &QQuickWindow::widthChanged, &cssTheme, [sync] { sync(); });
            QObject::connect(window, &QQuickWindow::heightChanged, &cssTheme, [sync] { sync(); });
        }
    };

    const QString qmlPath = parser.value(QStringLiteral("qml"));
    const QUrl url = qmlUrl(qmlPath);

    QmlCss::CssLayoutEngine cssLayout(&cssTheme);

    // The CSS primitives are C++ QQuickItems since the qml/ dir was retired; generated QML
    // does `import qmlcss 1.0 as Css`.
    QmlCss::registerTypes();

    SolidTabstop solidTabstop;

    QQmlApplicationEngine engine;
    // The generated app does `import solidqml.Widgets 1.0 as W`; the module lives in a `Widgets/`
    // dir beside the app (qml/solidqml/Widgets). Add the app root (parent of the app dir) so the
    // dotted module `solidqml.Widgets` resolves to <root>/solidqml/Widgets.
    {
        QDir appRoot = QFileInfo(qmlPath).absoluteDir();
        appRoot.cdUp();
        engine.addImportPath(appRoot.absolutePath());
    }
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &cssTheme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &cssLayout);
    engine.rootContext()->setContextProperty(QStringLiteral("solidTabstop"), &solidTabstop);
    // Browser-API shims for the V4 engine.
    WebLocalStorage::install(&engine); // synchronous, persistent localStorage
    WebFetch::install(&engine);        // fetch + Headers/Request/Response/AbortController
    WebTimers::install(&engine);       // setTimeout/setInterval/clear* + queueMicrotask
    WebPlatform::install(&engine);     // btoa/atob, TextEncoder/Decoder, crypto, performance, structuredClone
    JsPolyfill::install(&engine);      // additive ES2019-2023 stdlib backfill (trimStart, flat, at, fromEntries, ...)
    NodeShims::install(&engine);       // process / fs (sync) / child_process (promise) as importable modules
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed,
                     &app, [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.load(url);

    if (engine.rootObjects().isEmpty()) {
        qWarning().noquote() << "solid-qml-loader: no root object loaded";
        return 1;
    }
    QPointer<QQuickWindow> hostWindow = qobject_cast<QQuickWindow *>(engine.rootObjects().constLast());
    if (!hostWindow) {
        qWarning().noquote() << "solid-qml-loader: root object is not a Window";
        return 1;
    }
    wireWindow(hostWindow);

    const auto showToast = [&engine, &hostWindow](const QString &message) {
        if (!hostWindow || !hostWindow->contentItem())
            return;

        static const char toastQml[] = R"(
import QtQuick

Rectangle {
    id: toast
    property string message: ""
    z: 1000000
    width: Math.min(parent ? parent.width - 48 : 520, label.implicitWidth + 32)
    height: Math.max(44, label.implicitHeight + 20)
    x: parent ? (parent.width - width) / 2 : 24
    y: parent ? parent.height - height - 32 : 24
    radius: 8
    color: "#e02f3437"
    opacity: 0

    Text {
        id: label
        anchors.centerIn: parent
        width: Math.min(520, parent.width - 32)
        text: toast.message
        color: "white"
        font.pointSize: 11
        wrapMode: Text.WordWrap
        horizontalAlignment: Text.AlignHCenter
    }

    SequentialAnimation on opacity {
        running: true
        NumberAnimation { to: 1; duration: 120 }
        PauseAnimation { duration: 3600 }
        NumberAnimation { to: 0; duration: 180 }
        ScriptAction { script: toast.destroy() }
    }
}
)";

        QQmlComponent component(&engine);
        component.setData(QByteArray(toastQml), QUrl(QStringLiteral("solid-qml-toast.qml")));
        QObject *object = component.createWithInitialProperties({ { QStringLiteral("message"), message } });
        auto *item = qobject_cast<QQuickItem *>(object);
        if (!item) {
            delete object;
            return;
        }
        item->setParentItem(hostWindow->contentItem());
        item->setParent(hostWindow->contentItem());
    };

    // Hot reload: reload the QML file on change (CSS files are watched by CssTheme itself).
    if (parser.isSet(QStringLiteral("watch")) && url.isLocalFile()) {
        QPointer<QObject> reloadStateRoot;
        auto *watcher = new QFileSystemWatcher(&app);
        watcher->addPath(url.toLocalFile());
        QObject::connect(watcher, &QFileSystemWatcher::fileChanged, &app,
                         [&engine, &cssTheme, &hostWindow, &reloadStateRoot, &showToast, url, watcher](const QString &path) {
            // Editors replace files atomically (dropping the watch); re-arm after a beat,
            // then swap only the visual scene. The native Window itself stays alive; creating
            // a fresh top-level Window on every edit makes focus/placement unusable in dev.
            QTimer::singleShot(60, &engine, [&engine, &cssTheme, &hostWindow, &reloadStateRoot, &showToast, url, watcher, path] {
                if (QFileInfo::exists(path) && !watcher->files().contains(path))
                    watcher->addPath(path);
                if (!hostWindow) {
                    qWarning().noquote() << "solid-qml-loader: host window disappeared";
                    return;
                }
                qInfo().noquote() << "solid-qml-loader: reloading" << path;

                QQuickItem *hostContent = hostWindow->contentItem();
                const QList<QQuickItem *> oldVisuals = hostContent ? hostContent->childItems() : QList<QQuickItem *>();
                QPointer<QObject> oldStateRoot = reloadStateRoot;

                engine.clearComponentCache();
                QQmlComponent component(&engine, url, QQmlComponent::PreferSynchronous);
                if (component.isError()) {
                    QStringList messages;
                    for (const QQmlError &err : component.errors())
                        messages << err.toString();
                    qWarning().noquote() << messages.join(QLatin1Char('\n'));
                    showToast(QStringLiteral("QML reload failed\n") + messages.join(QLatin1Char('\n')));
                    return;
                }

                QObject *newRoot = component.createWithInitialProperties({ { QStringLiteral("visible"), false } });
                if (!newRoot) {
                    QStringList messages;
                    for (const QQmlError &err : component.errors())
                        messages << err.toString();
                    const QString detail = messages.isEmpty()
                        ? QStringLiteral("component.create() returned null")
                        : messages.join(QLatin1Char('\n'));
                    qWarning().noquote() << detail;
                    showToast(QStringLiteral("QML reload failed\n") + detail);
                    return;
                }

                auto *newWindow = qobject_cast<QQuickWindow *>(newRoot);
                if (!newWindow) {
                    const QString detail = QStringLiteral("reloaded root is not a Window");
                    qWarning().noquote() << "solid-qml-loader:" << detail;
                    showToast(QStringLiteral("QML reload failed\n") + detail);
                    newRoot->deleteLater();
                    return;
                }

                hostWindow->setTitle(newWindow->title());
                cssTheme.setViewport(hostWindow->width(), hostWindow->height());

                // Retire the old body BEFORE swapping the new one in. Detach each old visual from
                // the host content first (which takes it off-screen so it stops laying out, painting
                // and hit-testing), then deleteLater() it for a safe teardown on the next event-loop
                // turn. Keeping the old and new subtrees parented under the same content item at the
                // same time races: CSS relayout and binding teardown run across a mix of both trees.
                // Detaching first makes the swap atomic within this callback. The toast overlay
                // (z >= 1000000) is not part of the reloaded body, so leave it in place.
                for (QQuickItem *item : oldVisuals) {
                    if (!item || item->z() >= 1000000)
                        continue;
                    item->setVisible(false);
                    item->setParentItem(nullptr);
                    item->setParent(nullptr);
                    item->deleteLater();
                }
                if (oldStateRoot)
                    oldStateRoot->deleteLater();

                // Now move the freshly-built visuals under the (now-cleared) host content.
                QQuickItem *newContent = newWindow->contentItem();
                const QList<QQuickItem *> newVisuals = newContent ? newContent->childItems() : QList<QQuickItem *>();
                for (QQuickItem *item : newVisuals) {
                    item->setParentItem(hostContent);
                    item->setParent(hostContent);
                }

                newWindow->setVisible(false);
                reloadStateRoot = newRoot;
            });
        });
    }

    const QString resizeGrabPrefix = parser.value(QStringLiteral("resize-grab"));
    if (!resizeGrabPrefix.isEmpty()) {
        auto *window = hostWindow.data();
        if (!window) {
            qWarning().noquote() << "solid-qml-loader: --resize-grab needs a Window root";
            return 1;
        }
        // Live resize test. The default caths the reported stale-on-resize case: start wide, shrink,
        // grow again — each step must relayout AND repaint every box (fills/borders track the new
        // geometry). Override the sequence via SQ_RESIZE_SEQ.
        // A tiling / geometry-forcing WM would clamp a managed top-level's size, defeating the
        // test. Bypass the WM (X11 override-redirect) so setWidth/Height actually takes on the real
        // GPU compositor. Opt out with SQ_RESIZE_KEEP_WM=1 for WMs that honour programmatic resize.
        if (!qEnvironmentVariableIntValue("SQ_RESIZE_KEEP_WM")) {
            window->setFlag(Qt::BypassWindowManagerHint, true);
            window->setVisible(false);
            window->setVisible(true);
        }

        QString seq = qEnvironmentVariable("SQ_RESIZE_SEQ");
        if (seq.isEmpty())
            seq = QStringLiteral("760x520,1200x800,500x700,760x520");
        QList<QPair<int, int>> steps;
        for (const QString &s : seq.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            const QStringList wh = s.split(QLatin1Char('x'));
            if (wh.size() == 2)
                steps.append({ wh[0].toInt(), wh[1].toInt() });
        }
        const int settleMs = qEnvironmentVariableIntValue("SQ_RESIZE_MS") > 0
            ? qEnvironmentVariableIntValue("SQ_RESIZE_MS") : 1000;

        // Drive the sequence with a chained single-shot: apply size, wait for the relayout to
        // settle (CSS relayout is coalesced to the event loop + deferred via Qt.callLater), grab,
        // advance. Feed the viewport too so @media/vw/vh re-evaluate as they do on a real resize.
        auto *idx = new int(0);
        std::function<void()> stepFn;
        auto *stepHolder = new std::function<void()>();
        stepFn = [window, &cssTheme, steps, settleMs, resizeGrabPrefix, idx, stepHolder]() {
            if (*idx >= steps.size()) {
                QCoreApplication::quit();
                return;
            }
            const int w = steps[*idx].first, h = steps[*idx].second;
            window->setWidth(w);
            window->setHeight(h);
            cssTheme.setViewport(w, h);
            const bool onScreen = qEnvironmentVariableIntValue("SQ_RESIZE_ONSCREEN") != 0;
            QTimer::singleShot(settleMs, window, [window, resizeGrabPrefix, idx, w, h, stepHolder, onScreen] {
                // Two capture modes. Default: QQuickWindow::grabWindow() re-renders the scene graph
                // synchronously — always the settled, correct frame (good for verifying the final
                // layout). SQ_RESIZE_ONSCREEN=1: QScreen::grabWindow() reads the ACTUAL on-screen
                // framebuffer from X without re-rendering, so a short SQ_RESIZE_MS can catch a
                // TRANSIENT stale/black frame that the scene-graph re-render would paper over.
                QImage frame;
                if (onScreen && window->screen())
                    frame = window->screen()->grabWindow(window->winId()).toImage();
                if (frame.isNull())
                    frame = window->grabWindow();
                const QString path = QStringLiteral("%1-%2-%3x%4.png")
                    .arg(resizeGrabPrefix).arg(*idx).arg(w).arg(h);
                if (frame.save(path))
                    qInfo().noquote() << "solid-qml-loader: wrote" << path;
                else
                    qWarning().noquote() << "solid-qml-loader: failed to write" << path;
                ++(*idx);
                (*stepHolder)();
            });
        };
        *stepHolder = stepFn;
        // Let the first frame build before the first resize.
        QTimer::singleShot(settleMs, window, [stepHolder] { (*stepHolder)(); });
        return app.exec();
    }

    const QString grabPath = parser.value(QStringLiteral("grab"));
    if (!grabPath.isEmpty()) {
        auto *window = hostWindow.data();
        if (!window) {
            qWarning().noquote() << "solid-qml-loader: --grab needs a Window root";
            return 1;
        }
        // Let the scene settle (CSS load + relayout are deferred via Qt.callLater) before
        // grabbing a single frame, then quit. Delay overridable via SQ_GRAB_MS (e.g. to
        // sample an animation at different phases).
        const int grabMs = qEnvironmentVariableIntValue("SQ_GRAB_MS") > 0
            ? qEnvironmentVariableIntValue("SQ_GRAB_MS") : 1200;
        QTimer::singleShot(grabMs, window, [window, &grabPath] {
            const QImage frame = window->grabWindow();
            if (frame.save(grabPath))
                qInfo().noquote() << "solid-qml-loader: wrote" << grabPath;
            else
                qWarning().noquote() << "solid-qml-loader: failed to write" << grabPath;
            QCoreApplication::quit();
        });
    }

    return app.exec();
}
