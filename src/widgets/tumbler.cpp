#include "tumbler.h"

#include "snippetwidget.h"

namespace {

// Original Tumbler.qml internals — `root` = the C++ wrapper.
//
// T.Tumbler instantiates NO view of its own (qquicktumbler_p.h): the C++ walks contentItem looking
// for a PathView or ListView (determineViewType). The Basic style uses a private TumblerView helper
// (PathView-backed) we cannot import, so our contentItem is the minimal working ListView. `wrap:
// false` is EXPLICIT: with count ≥ visibleItemCount the implicit default flips to wrapping, and the
// C++ then expects a PathView.
//
// The attached Tumbler.displacement must be read on the delegate ROOT (the attached object's init
// requires a delegate item with a parent and the `index` context property); the root Item mirrors
// it into `__disp` for the inner CssText's cssState. Cell size is bound declaratively
// (availableWidth × availableHeight/visibleItemCount — the template's own resize formula): the C++
// imperative resize misses items incubated after the last geometry change.
const char *kTumblerBody = R"(import QtQuick
import QtQuick.Templates as T
import qmlcss 1.0 as Css

T.Tumbler {
    id: __ctl
    anchors.fill: parent
    activeFocusOnTab: solidTabstop.enabled
    wrap: false
    model: root.model
    enabled: !root.disabled
    onCurrentIndexChanged: root.currentIndex = currentIndex

    Binding on currentIndex {
        value: root.currentIndex
        restoreMode: Binding.RestoreNone
    }

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
)";

} // namespace

namespace SolidWidgets {

Tumbler::Tumbler(QQuickItem *parent)
    : QmlCss::CssFill(parent)
{
    connect(this, &QQuickItem::enabledChanged, this, &Tumbler::syncState);
}

void Tumbler::setModel(const QVariant &v)
{
    if (m_model == v)
        return;
    m_model = v;
    emit modelChanged();
}

void Tumbler::setCurrentIndex(int v)
{
    if (m_currentIndex == v)
        return;
    m_currentIndex = v;
    emit currentIndexChanged();
}

void Tumbler::setDisabled(bool v)
{
    if (m_disabled == v)
        return;
    m_disabled = v;
    emit disabledChanged();
    syncState();
}

void Tumbler::componentComplete()
{
    QmlCss::CssFill::componentComplete();
    m_control = composeInternal(this, content(), QStringLiteral("solidwidgets-tumbler"), kTumblerBody);
    if (m_control) {
        connect(m_control, SIGNAL(activeFocusChanged(bool)), this, SLOT(syncState()));
        const auto mirror = [this] {
            setImplicitWidth(m_control->implicitWidth());
            setImplicitHeight(m_control->implicitHeight());
        };
        connect(m_control, &QQuickItem::implicitWidthChanged, this, mirror);
        connect(m_control, &QQuickItem::implicitHeightChanged, this, mirror);
        mirror();
    }
    syncState();
}

void Tumbler::syncState()
{
    QVariantList state;
    if (m_control && m_control->hasActiveFocus())
        state << QStringLiteral("focus");
    if (m_disabled || !isEnabled())
        state << QStringLiteral("disabled");
    setCssState(state);
}

} // namespace SolidWidgets
