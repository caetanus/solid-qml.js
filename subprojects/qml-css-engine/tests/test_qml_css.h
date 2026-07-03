#pragma once

#include <QObject>

class QmlCssTests final : public QObject {
    Q_OBJECT

private slots:
    // Register the classic C++ types (CssRect/CssHr/CssImage) into the `qmlcss` module once,
    // before any test — so `import qmlcss` resolves everywhere (incl. the CssFill shim).
    void initTestCase();

    void cssRectLoadsAndRestyles();
    void cssTextUsesStandaloneDefaults();
    void cssItemAppliesToParent();

    // Layout (geometry) tests — exercise CssLayoutEngine directly.
    void layoutFlexColumnStacks();
    void layoutClampsMaxAndMinWidth();
    void layoutHonoursFlexOrder();
    void layoutAppliesFlexBasis();
    void layoutVisibilityHiddenKeepsSpace();
    void layoutGridTemplateAreas();
    void layoutFlexShrinkOverflow();
    void layoutAlignContentCenter();
    void layoutBoxSizingBorderBox();
    void buttonLabelInheritsColor();

    // New CSS property mapping tests.
    void cssRectZIndex();
    void cssRectOverflowHidden();
    void cssTextDecorationUnderline();
    void cssTextWhiteSpaceNowrap();
    void cssTextOverflowEllipsis();

    // C++ CssRect — composition translation of CssRect.qml.
    void cssRectCppComposesShapeAndContains();

    // C++ CssHr — composition translation of CssHr.qml.
    void cssHrCppComposesRealShape();

    // C++ CssImage — composition translation of CssImage.qml.
    void cssImageCppComposesImageEffectAndMask();
    void cssImageCppNoRadiusShowsImageDirect();

    // C++ CssText — composition translation of CssText.qml.
    void cssTextCppComposesRealText();

    // C++ CssFill — composition translation of CssFill.qml.
    void cssFillCppComposesImageRectAndHostsChildren();

    // C++ CssDropShadow — composition translation of CssDropShadow.qml.
    void cssDropShadowCppComposesMultiEffect();

    // C++ CssKeyframes — composition translation of CssKeyframes.qml.
    void cssKeyframesCppInterpolatesTarget();

    // C++ CssRect @keyframes animation driver — live NumberAnimation on animTick.
    void cssRectCppKeyframesDriverLive();

    // C++ CssIcon — composition translation of CssIcon.qml.
    void cssIconCppComposesImageAndEffect();

    // C++ CssFillLayer — composition translation of CssFillLayer.qml.
    void cssFillLayerCppComposesShape();

    // C++ CssItem — composition translation of CssItem.qml (migrated from cssItemAppliesToParent).
    void cssItemCppAppliesToParent();

    // C++ Contrast — WCAG contrast utilities, ported from Contrast.js.
    void contrastCppPure();
    void contrastQmlSingleton();

    // notifyParentLayout must duck-type like the QML original: a plain-Item grandparent
    // (no requestRelayout()) is skipped silently, not invoked-and-warned.
    void layoutNotifyParentSkipsNonBoxAncestors();

    // Ancestor-scoped rules end-to-end: `.nav button` styles only buttons under `.nav`,
    // and a class/state change on an ANCESTOR restyles the scoped descendants too.
    void ancestorScopedRulesStyleTheTree();

    // CssText must paint `background-color` (+ border-radius) behind the composed Text,
    // like the web does for any element (cssgaps `.wrap/.nowrap/.ellipsis` tiles).
    void cssTextCppPaintsBackground();

    // Applying a style with NO `display` must not touch the item's `visible` — an imperative
    // setVisible(true) destroys author bindings like `visible: cond` (the <Show> guard).
    void styleApplyPreservesVisibleBinding();

    // text-shadow keeps the composed Text visible (a hidden MultiEffect source renders empty).
    void cssTextShadowKeepsLabelVisible();
};
