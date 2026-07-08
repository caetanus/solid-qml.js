#include "swipeview.h"

#include "snippetwidget.h"

namespace {

// Original SwipeView.qml internals — `root` = the C++ wrapper. Templates create NO contentItem
// (verified pitfall) → a Basic-style ListView over the contentModel; `clip` keeps the
// neighbouring pages inside the box while swiping.
const char *kSwipeViewBody = R"(import QtQuick
import QtQuick.Templates as T

T.SwipeView {
    id: sw
    anchors.fill: parent
    onCurrentIndexChanged: root.currentIndex = currentIndex

    Binding on currentIndex {
        value: root.currentIndex
        restoreMode: Binding.RestoreNone
    }

    // Basic-style implicit size: Templates leave implicit sizes to the style (us).
    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset, implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset, implicitContentHeight + topPadding + bottomPadding)
    background: null
    contentItem: ListView {
        model: sw.contentModel
        interactive: sw.interactive
        currentIndex: sw.currentIndex
        spacing: sw.spacing
        orientation: sw.orientation
        snapMode: ListView.SnapOneItem
        boundsBehavior: Flickable.StopAtBounds
        highlightRangeMode: ListView.StrictlyEnforceRange
        preferredHighlightBegin: 0
        preferredHighlightEnd: 0
        highlightMoveDuration: 250
        clip: true
    }
}
)";

// Original PageIndicator.qml internals. The dot delegate is a Css.CssRect ["dot"] 8×8 with
// cssState "selected" on the current page; Templates create NO contentItem → the Basic-style
// Row + Repeater is supplied here.
const char *kPageIndicatorBody = R"(import QtQuick
import QtQuick.Templates as T
import qmlcss 1.0 as Css

T.PageIndicator {
    id: dots
    anchors.fill: parent
    count: root.count
    currentIndex: root.currentIndex
    // Basic-style implicit size: Templates leave implicit sizes to the style (us).
    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset, implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset, implicitContentHeight + topPadding + bottomPadding)
    background: null
    spacing: 6
    delegate: Css.CssRect {
        required property int index
        cssPrimitive: "div"
        cssClass: ["dot"]
        cssState: index === dots.currentIndex ? ["selected"] : []
        implicitWidth: 8
        implicitHeight: 8
    }
    contentItem: Row {
        spacing: dots.spacing
        Repeater {
            model: dots.count
            delegate: dots.delegate
        }
    }
}
)";

} // namespace

namespace SolidWidgets {

SwipeView::SwipeView(QQuickItem *parent)
    : SlotContainer(parent)
{
    setCssPrimitive(QStringLiteral("swipeview"));
}

void SwipeView::setCurrentIndex(int v)
{
    if (m_currentIndex == v)
        return;
    m_currentIndex = v;
    emit currentIndexChanged();
}

void SwipeView::componentComplete()
{
    QmlCss::CssFill::componentComplete();
    adoptControl(composeInternal(this, content(), QStringLiteral("solidwidgets-swipeview"), kSwipeViewBody));
    if (m_control) {
        const auto mirror = [this] {
            setImplicitWidth(m_control->implicitWidth());
            setImplicitHeight(m_control->implicitHeight());
        };
        connect(m_control, &QQuickItem::implicitWidthChanged, this, mirror);
        connect(m_control, &QQuickItem::implicitHeightChanged, this, mirror);
        mirror();
    }
}

PageIndicator::PageIndicator(QQuickItem *parent)
    : QmlCss::CssFill(parent)
{
    setCssPrimitive(QStringLiteral("pageindicator"));
}

void PageIndicator::setCount(int v)
{
    if (m_count == v)
        return;
    m_count = v;
    emit countChanged();
}

void PageIndicator::setCurrentIndex(int v)
{
    if (m_currentIndex == v)
        return;
    m_currentIndex = v;
    emit currentIndexChanged();
}

void PageIndicator::componentComplete()
{
    QmlCss::CssFill::componentComplete();
    m_control = composeInternal(this, content(), QStringLiteral("solidwidgets-pageindicator"), kPageIndicatorBody);
    if (m_control) {
        const auto mirror = [this] {
            setImplicitWidth(m_control->implicitWidth());
            setImplicitHeight(m_control->implicitHeight());
        };
        connect(m_control, &QQuickItem::implicitWidthChanged, this, mirror);
        connect(m_control, &QQuickItem::implicitHeightChanged, this, mirror);
        mirror();
    }
}

} // namespace SolidWidgets
