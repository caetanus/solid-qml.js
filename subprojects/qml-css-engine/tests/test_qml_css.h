#pragma once

#include <QObject>

class QmlCssTests final : public QObject {
    Q_OBJECT

private slots:
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
};
