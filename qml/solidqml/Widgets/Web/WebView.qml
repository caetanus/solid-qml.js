// WebView — an opt-in module (solidqml.Widgets.Web): QtWebEngine loads only when the <WebView>
// tag is used, the core never depends on it (the Chart/Media pattern). A CssFill "div" wrapper
// hosts the WebEngineView as a foreign fill; the loader unconditionally sets
// Qt::AA_ShareOpenGLContexts (dependency-free) so the engine can compose.
//
// The transpiler emits:  WWeb.WebView { cssClass: […]; src: <url> }
import QtQuick
import QtWebEngine
import qmlcss 1.0 as Css

Css.CssFill {
    id: root
    property url src: ""
    property alias loading: view.loading
    property alias title: view.title

    cssPrimitive: "div"
    implicitWidth: 560
    implicitHeight: 360

    WebEngineView {
        id: view
        anchors.fill: parent
        url: root.src
    }
}
