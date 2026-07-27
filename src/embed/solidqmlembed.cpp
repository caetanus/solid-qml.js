#include "solidqmlembed.h"

#include "qmlcss/QMLCss.h"
#include "shims/backgroundtasks.h"
#include "shims/jspolyfill.h"
#include "shims/nodeshims.h"
#include "shims/notifications.h"
#include "shims/sharedbuffers.h"
#include "shims/tabstop.h"
#include "shims/webfetch.h"
#include "shims/weblocalstorage.h"
#include "shims/webplatform.h"
#include "shims/webtimers.h"
#include "shims/webworker.h"
#include "widgets/solidwidgets.h"

#include <QCoreApplication>
#include <QDir>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWindow>

namespace SolidQmlEmbed {

namespace {

// Engine-scoped runtime bundle, parented to the engine (dies with it). Holding it as a child
// QObject keyed by name makes init() idempotent without a global registry.
class EngineRuntime : public QObject {
public:
    EngineRuntime(QQmlEngine *engine, const QUrl &appDir)
        : QObject(engine)
        , theme()
        , layout(&theme)
        , appBase(appDir)
    {
        // Keyed via a dynamic property (findChild would need Q_OBJECT+moc for a local class).
        engine->setProperty("__solidQmlEmbedRuntime", QVariant::fromValue<QObject *>(this));
        QQmlContext *ctx = engine->rootContext();
        ctx->setContextProperty(QStringLiteral("cssTheme"), &theme);
        ctx->setContextProperty(QStringLiteral("cssLayout"), &layout);
        ctx->setContextProperty(QStringLiteral("solidTabstop"), &tabstop);
        ctx->setContextProperty(QStringLiteral("solidNotifications"), &notifications);
    }

    QmlCss::CssTheme theme;
    QmlCss::CssLayoutEngine layout;
    SolidTabstop tabstop;
    SolidNotifications notifications;
    QUrl appBase;
    QQuickWindow *viewportWindow = nullptr;
};

EngineRuntime *runtimeFor(QQmlEngine *engine)
{
    return static_cast<EngineRuntime *>(
        engine->property("__solidQmlEmbedRuntime").value<QObject *>());
}

} // namespace

void init(QQmlEngine *engine, const QUrl &appDir, const QString &capabilities)
{
    if (!engine || runtimeFor(engine))
        return;

    // Type registrations are process-wide and idempotent by URI/version.
    QmlCss::registerTypes();
    SolidWidgets::registerTypes();
    static const int islandType = qmlRegisterType<SolidIsland>("solidqml.Embed", 1, 0, "SolidIsland");
    Q_UNUSED(islandType);

    const QUrl base = appDir.isEmpty()
        ? QUrl::fromLocalFile(QCoreApplication::applicationDirPath() + QLatin1Char('/'))
        : appDir;
    new EngineRuntime(engine, base); // parents itself to the engine

    // The same shim stack the loader arms — the generated components and their handler JS
    // depend on it (fetch/localStorage/timers/process/…, Worker, SharedArrayBuffer, background).
    engine->installExtensions(QJSEngine::ConsoleExtension);
    WebLocalStorage::install(engine);
    WebFetch::install(engine);
    WebTimers::install(engine);
    WebPlatform::install(engine);
    JsPolyfill::install(engine);
    NodeShims::install(engine, NodeShims::profileFromString(capabilities));
    SolidWorkers::SharedBuffers::install(engine);
    SolidWorkers::BackgroundTasks::install(engine);
    SolidWorkers::WebWorkerFactory::install(engine, base);

    // The generated components live in a `solidqml`-style module dir next to the app output;
    // the host adds its own import paths as usual. The app dir itself is an import root so
    // `import solidqml.Widgets` resolves for any REMAINING composite modules (opt-ins).
    if (base.isLocalFile())
        engine->addImportPath(QDir(base.toLocalFile()).absolutePath());
}

void loadCss(QQmlEngine *engine, const QUrl &cssFile)
{
    EngineRuntime *rt = runtimeFor(engine);
    if (!rt)
        return;
    const QUrl resolved = rt->appBase.resolved(cssFile);
    rt->theme.load(resolved.isLocalFile() ? resolved.toLocalFile() : resolved.toString());
}

void loadCssString(QQmlEngine *engine, const QString &css)
{
    EngineRuntime *rt = runtimeFor(engine);
    if (rt)
        rt->theme.loadLayeredString(css);
}

void attachWindow(QQmlEngine *engine, QQuickWindow *window)
{
    EngineRuntime *rt = runtimeFor(engine);
    if (!rt || !window || rt->viewportWindow == window)
        return;
    rt->viewportWindow = window;
    rt->theme.setViewport(window->width(), window->height());
    const auto sync = [rt, window] { rt->theme.setViewport(window->width(), window->height()); };
    QObject::connect(window, &QQuickWindow::widthChanged, &rt->theme, sync);
    QObject::connect(window, &QQuickWindow::heightChanged, &rt->theme, sync);
}

SolidIsland::SolidIsland(QQuickItem *parent)
    : QQuickItem(parent)
{
}

void SolidIsland::setSource(const QUrl &v)
{
    if (m_source == v)
        return;
    m_source = v;
    emit sourceChanged();
    if (isComponentComplete())
        build();
}

void SolidIsland::setCssFiles(const QStringList &v)
{
    if (m_cssFiles == v)
        return;
    m_cssFiles = v;
    emit cssFilesChanged();
}

void SolidIsland::componentComplete()
{
    QQuickItem::componentComplete();
    build();
}

void SolidIsland::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (m_root)
        m_root->setSize(newGeometry.size());
}

void SolidIsland::build()
{
    QQmlEngine *engine = qmlEngine(this);
    if (!engine || m_source.isEmpty())
        return;
    EngineRuntime *rt = runtimeFor(engine);
    if (!rt) {
        emit error(QStringLiteral("SolidQmlEmbed::init(engine) was not called before loading host QML"));
        return;
    }

    // CSS loads ADDITIVELY into the engine's shared cascade (islands coexist like sections of
    // one page — namespace with classes, exactly as on the web).
    for (const QString &css : std::as_const(m_cssFiles)) {
        const QUrl resolved = rt->appBase.resolved(QUrl(css));
        rt->theme.load(resolved.isLocalFile() ? resolved.toLocalFile() : resolved.toString());
    }

    // Viewport units track the HOST window (an island behaves like an in-page component, not
    // its own page). First island wires the sync; the theme is engine-shared.
    if (QQuickWindow *win = window(); win && rt->viewportWindow != win) {
        rt->viewportWindow = win;
        rt->theme.setViewport(win->width(), win->height());
        const auto sync = [rt, win] { rt->theme.setViewport(win->width(), win->height()); };
        connect(win, &QQuickWindow::widthChanged, &rt->theme, sync);
        connect(win, &QQuickWindow::heightChanged, &rt->theme, sync);
    }

    if (m_root) {
        m_root->deleteLater();
        m_root = nullptr;
    }

    const QUrl resolved = rt->appBase.resolved(m_source);
    QQmlComponent component(engine, resolved);
    if (component.isError()) {
        emit error(component.errorString());
        return;
    }
    // Root context: the generated components resolve cssTheme/solidTabstop/… from it.
    QObject *o = component.create(engine->rootContext());
    auto *item = qobject_cast<QQuickItem *>(o);
    if (!item) {
        delete o;
        emit error(QStringLiteral("island root is not an Item: ") + resolved.toString());
        return;
    }
    item->setParent(this);
    item->setParentItem(this);
    item->setSize(size());
    m_root = item;
    emit rootItemChanged();
    emit ready();
}

} // namespace SolidQmlEmbed
