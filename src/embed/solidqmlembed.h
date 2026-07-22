#pragma once

#include <QQuickItem>
#include <QStringList>
#include <QUrl>

class QQmlEngine;

// Embedding surface for PRE-EXISTING QtQuick apps (owner 2026-07-22: "plugar o solid-qml num
// projeto pre-existente c++" — host QtQuick, UI islands). Two pieces:
//
//   1. SolidQmlEmbed::init(engine) — arms a HOST engine with the solid-qml runtime: registers
//      the qmlcss + solidqml.Widgets C++ types and the solidqml.Embed module, installs the
//      browser/node shims (fetch, localStorage, timers, Workers, SharedArrayBuffer, background
//      tasks, notifications), and creates the engine-scoped services (CssTheme, CssLayoutEngine,
//      tab-stop switch, notification hub) as root-context properties — everything the generated
//      components reference. Idempotent per engine.
//
//   2. SolidIsland — the QML item the host scene composes:
//
//         import solidqml.Embed 1.0
//         SolidIsland {
//             source: "solid/Dashboard.qml"      // transpiler output (relative to the app dir)
//             cssFiles: ["solid/App.generated.css"]
//             onReady: island.rootItem.someProp = 42   // the component root, props/signals live
//         }
//
//      CSS files load ADDITIVELY into the engine's shared theme (islands share one cascade —
//      namespace by class as on the web). Viewport units (vh/vw) resolve against the HOST
//      window, mirroring an in-page web component; the island itself sizes like any Item
//      (anchor/fix it from the host scene; the solid root fills it).
//
// Consumption: link libsolidqml (pkg-config `solidqml` for meson/qmake, find_package(SolidQml)
// for CMake) and call init() after creating the engine, before loading host QML.
namespace SolidQmlEmbed {

// Arm a host engine (idempotent). `appDir` anchors relative SolidIsland.source /cssFiles paths
// and Worker script URLs; defaults to the application directory.
void init(QQmlEngine *engine, const QUrl &appDir = QUrl());

// Full-app hosting (a standalone binary loading a generated Window root directly, no island):
// load a stylesheet into the engine's shared cascade, and keep viewport units tracking a window.
void loadCss(QQmlEngine *engine, const QUrl &cssFile);
void attachWindow(QQmlEngine *engine, QQuickWindow *window);

class SolidIsland : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QUrl source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(QStringList cssFiles READ cssFiles WRITE setCssFiles NOTIFY cssFilesChanged)
    Q_PROPERTY(QQuickItem *rootItem READ rootItem NOTIFY rootItemChanged)

public:
    explicit SolidIsland(QQuickItem *parent = nullptr);

    QUrl source() const { return m_source; }
    void setSource(const QUrl &v);
    QStringList cssFiles() const { return m_cssFiles; }
    void setCssFiles(const QStringList &v);
    QQuickItem *rootItem() const { return m_root; }

signals:
    void sourceChanged();
    void cssFilesChanged();
    void rootItemChanged();
    void ready();
    void error(const QString &message);

protected:
    void componentComplete() override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private:
    void build();

    QUrl m_source;
    QStringList m_cssFiles;
    QQuickItem *m_root = nullptr;
};

} // namespace SolidQmlEmbed
