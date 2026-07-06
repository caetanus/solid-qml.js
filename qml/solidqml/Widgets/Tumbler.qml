// Tumbler — the <Tumbler> component in module solidqml.Widgets (owner directive 2026-07-05: one
// .qml per component; the transpiler INSTANTIATES these, it no longer hand-emits the picker). A
// T.Tumbler with CSS support.
//
// T.Tumbler instantiates NO view of its own (qquicktumbler_p.h): the C++ walks contentItem looking
// for a PathView or ListView (determineViewType). The Basic style uses a private TumblerView helper
// (PathView-backed) we cannot import, so our contentItem is the minimal working ListView:
// SnapToItem + StrictlyEnforceRange with a one-item-tall preferred highlight window centred in the
// view — the exact non-wrap configuration TumblerView generates internally. `wrap: false` is
// EXPLICIT: with count ≥ visibleItemCount the implicit default flips to wrapping, and the C++ then
// expects a PathView.
//
// The attached Tumbler.displacement must be read on the delegate ROOT (the attached object's init
// requires a delegate item with a parent and the `index` context property); the root Item mirrors
// it into `__disp` for the inner CssText's cssState. Cell size is bound declaratively
// (availableWidth × availableHeight/visibleItemCount — the template's own resize formula): the C++
// imperative resize misses items incubated after the last geometry change.
//
// The transpiler emits:  W.Tumbler { id: __inputN; cssClass; model; onCurrentIndexChanged; disabled }
// with a RestoreNone Binding on `currentIndex` (options.indexOf(value)) for the controlled value.
// `model`/`currentIndex` are aliases to the control so the emit's reads/writes resolve across the
// component boundary.
import QtQuick
import QtQuick.Templates as T
import qmlcss 1.0 as Css

Css.CssFill {
    id: root
    property alias model: __ctl.model
    property alias currentIndex: __ctl.currentIndex
    property bool disabled: false

    cssPrimitive: ""
    cssState: (__ctl.activeFocus ? ["focus"] : []).concat(!__ctl.enabled ? ["disabled"] : [])
    implicitWidth: __ctl.implicitWidth
    implicitHeight: __ctl.implicitHeight

    T.Tumbler {
        id: __ctl
        anchors.fill: parent
        activeFocusOnTab: solidTabstop.enabled
        wrap: false
        enabled: !root.disabled
        // Basic-style implicit size: Templates leave implicit sizes to the style (us).
        implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset, implicitContentWidth + leftPadding + rightPadding)
        implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset, implicitContentHeight + topPadding + bottomPadding)
        delegate: Item {
            width: __ctl.availableWidth
            height: __ctl.availableHeight / __ctl.visibleItemCount
            property real __disp: T.Tumbler.displacement
            Css.CssText {
                cssPrimitive: ""
                cssClass: ["item"]
                // displacement is 0 for the row settled on the centre; the view snaps, but the
                // settle is float-valued — a half-row tolerance marks exactly one row selected.
                cssState: Math.abs(__disp) < 0.5 ? ["selected"] : []
                text: modelData
                anchors.centerIn: parent
            }
        }
        contentItem: ListView {
            implicitWidth: 60
            implicitHeight: 180
            model: __ctl.model
            delegate: __ctl.delegate
            snapMode: ListView.SnapToItem
            highlightRangeMode: ListView.StrictlyEnforceRange
            preferredHighlightBegin: height / 2 - height / __ctl.visibleItemCount / 2
            preferredHighlightEnd: height / 2 + height / __ctl.visibleItemCount / 2
            clip: true
        }
    }
}
