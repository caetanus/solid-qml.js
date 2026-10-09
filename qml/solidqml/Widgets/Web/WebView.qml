// WebView — an opt-in module (solidqml.Widgets.Web): QtWebEngine loads only when the <WebView>
// tag is used, the core never depends on it (the Chart/Media pattern). A CssFill "div" wrapper
// hosts the WebEngineView as a foreign fill; the loader unconditionally sets
// Qt::AA_ShareOpenGLContexts (dependency-free) so the engine can compose.
//
// The transpiler emits:  WWeb.WebView { cssClass: […]; src: <url> }
//                    or  WWeb.WebView { cssClass: […]; html: <string>; remoteContent: <bool> }
//
// `html` is UNTRUSTED content (an e-mail body, a feed item) and is loaded under a policy:
//   - it is wrapped in a document whose FIRST tag is a Content-Security-Policy. Without
//     `remoteContent` nothing remote is fetched — no tracking pixel, image, stylesheet, font or
//     frame; with it, images/styles/fonts/media may load. Scripts never run: the policy has no
//     script-src and JavaScript is off. A policy the content carries can only ADD restrictions
//     (CSP policies intersect), so it cannot loosen this one;
//   - DNS prefetch is off — CSP does not govern <link rel=dns-prefetch>;
//   - navigation is pinned: the view accepts only its own load — a main-frame navigation to the
//     `data:` URL loadHtml() produces, which content cannot start (Chromium refuses renderer-initiated
//     top-level data: navigations); a clicked http(s)/mailto link opens in the system handler —
//     no other scheme (file:, smb:, ssh:, third-party handlers) — and anything else (meta refresh,
//     form submit) is refused;
//   - loads are coalesced to one per event-loop turn with the FINAL html/policy: two loadHtml()
//     in a row (content + the remote switch flipped in the same turn) produced one navigation,
//     carrying the FIRST document's policy;
//   - the box is as tall as the content (implicitHeight follows it), so it sits in a card.
//
// Internal properties are `_`-prefixed: an element's bindings resolve names against the element
// FIRST, so an internal `htmlMode` shadowed an app signal of the same name — `html: htmlMode && …`
// read the widget's own (false, html still empty) and stayed empty.
//
// The WebEngineView itself is created only once there is something to show (`html` or `src`): an
// empty <WebView> — one per message in a list, most of them plain text — costs no engine and
// takes no space.
import QtQuick
import QtWebEngine
import qmlcss 1.0 as Css

Css.CssFill {
    id: root
    property url src: ""
    property string html: ""
    property bool remoteContent: false
    readonly property bool loading: engine.item ? engine.item.loading : false
    readonly property string title: engine.item ? engine.item.title : ""

    readonly property bool _htmlMode: html.length > 0
    readonly property string _policy: remoteContent
        ? "default-src 'none'; img-src * data: cid:; style-src * 'unsafe-inline'; font-src *; media-src *"
        : "default-src 'none'; img-src data: cid:; style-src 'unsafe-inline'"

    cssPrimitive: "div"
    implicitWidth: 560
    implicitHeight: 360

    function loadContent() { Qt.callLater(root.loadNow); }
    function loadNow() {
        if (!_htmlMode || !engine.item)
            return;
        engine.item.loadHtml("<!doctype html><html><head><meta charset=\"utf-8\">"
            + "<meta http-equiv=\"Content-Security-Policy\" content=\"" + _policy + "\">"
            + "<style>html,body{margin:0;padding:0;background:transparent}img{max-width:100%}</style>"
            + "</head><body>" + html + "</body></html>", "about:blank");
    }
    onHtmlChanged: loadContent()
    onRemoteContentChanged: loadContent()

    // The scroll box around us (an engine `overflow: auto` Flickable: it has __maxY), or null.
    function _scroller() {
        for (var p = root.parent; p; p = p.parent)
            if (p.__maxY !== undefined) return p
        return null
    }

    // The box's height comes from this FOREIGN child: CssFill owns its own implicitHeight (it
    // writes it from the children), and the engine re-flows when a foreign child's implicit size
    // changes. At least 24px, or the page has no viewport and reports no content height at all.
    Item {
        anchors.fill: parent
        implicitHeight: root._htmlMode ? Math.max(24, engine.item ? engine.item.contentsSize.height : 0)
                      : engine.active ? 360 : 0

        Loader {
            id: engine
            anchors.fill: parent
            active: root._htmlMode || root.src.toString().length > 0
            onLoaded: root.loadContent()
            sourceComponent: WebEngineView {
                url: root._htmlMode ? "" : root.src
                backgroundColor: root._htmlMode ? "transparent" : "white"
                settings.javascriptEnabled: !root._htmlMode
                settings.javascriptCanOpenWindows: !root._htmlMode
                settings.localContentCanAccessRemoteUrls: !root._htmlMode
                settings.dnsPrefetchEnabled: !root._htmlMode
                // Ctrl+A over the page selects ITS text: claimed at ShortcutOverride, so an app-level
                // Ctrl+A shortcut (select all of a list) does not take it while the page has focus.
                Keys.onShortcutOverride: (event) => { if (event.matches(StandardKey.SelectAll)) event.accepted = true }
                onNavigationRequested: (request) => {
                    if (!root._htmlMode)
                        return;
                    if (request.isMainFrame && request.url.toString().startsWith("data:"))
                        return;
                    request.reject();
                    // Only web and mail links leave the view: an untrusted message must not hand
                    // file:, smb:, ssh: or a third-party desktop handler to the system opener.
                    if (request.navigationType === WebEngineNavigationRequest.LinkClickedNavigation
                            && /^(https?|mailto):/i.test(request.url.toString()))
                        Qt.openUrlExternally(request.url);
                }
            }
        }

        // An `html` body is as tall as its content: it never scrolls itself — the box around it
        // does. Chromium takes the wheel and drops it, so over the body the wheel moved nothing;
        // here it moves the enclosing scroll box (the engine's own step: pixels, or ~3 lines a
        // notch). Buttons and hover still reach the page (links click).
        MouseArea {
            anchors.fill: parent
            z: 1
            enabled: root._htmlMode
            acceptedButtons: Qt.NoButton
            onWheel: (ev) => {
                var f = root._scroller()
                if (!f) { ev.accepted = false; return }
                var dy = ev.pixelDelta.y !== 0 ? ev.pixelDelta.y : (ev.angleDelta.y / 120) * 54
                f.contentY = Math.max(0, Math.min(f.contentY - dy, f.__maxY))
            }
        }
    }
}
