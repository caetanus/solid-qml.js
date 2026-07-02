#pragma once

#include <QPointer>
#include <QQuickItem>
#include <QVariantList>
#include <QVariantMap>

// One CSS background layer — solid colour, linear- or radial-gradient — painted as a rounded
// rectangle matching this item's size and the given (already clamped) corner radii. Translated
// 1:1 from qml/qmlcss/CssFillLayer.qml — BY COMPOSITION. CssRect stacks these via a Repeater
// in its kRenderShell to express radial gradients and multi-layer `background` values that the
// single built-in gradient fill can't express.
//
// Like the original QML Shape, this item paints OPAQUE and carries its alpha on the composed
// Shape's `opacity` (peakAlpha — the maximum alpha of the gradient stops, or the solid colour's
// alpha). A translucent Shape fill blends wrong; opacity is the correct knob.
//
// Equivalent QML this C++ composes (see kFillLayerShell in cssfilllayer.cpp):
//
//   import QtQuick
//   import QtQuick.Shapes
//   Shape {                                          // <- m_shape
//       preferredRendererType: Shape.CurveRenderer
//       // inputs pushed by C++:
//       property string kind                         // "color" | "linear" | "radial"
//       property color fillColor                     // opaque solid (alpha rides on opacity)
//       property real r0, r1, r2, r3                // per-corner radii
//       property real angle                          // CSS linear angle (degrees; 0=up, CW)
//       property real cx, cy                         // radial centre fractions
//       property var spec                            // raw spec map — rebuildStops reads .stops
//       // geometry bindings (reactive to width/height):
//       readonly property real _angleRad, _dx, _dy, _len
//       readonly property real cxp, cyp             // centre in pixels
//       ShapePath { /* 4×(PathLine + PathArc) rounded-rect */ }
//       LinearGradient { id: lin; ... }
//       RadialGradient { id: rad; ... }
//       Component { id: stopComponent; GradientStop {} }
//       function rebuildStops() { ... }
//       onSpecChanged: rebuildStops()
//       Component.onCompleted: rebuildStops()
//   }
class CssFillLayer : public QQuickItem {
    Q_OBJECT
    // { type: "color"|"linear"|"radial", color?, angle?, cx?, cy?, stops? } (from CssTheme)
    Q_PROPERTY(QVariantMap spec READ spec WRITE setSpec NOTIFY specChanged)
    // [topLeft, topRight, bottomRight, bottomLeft] — already clamped by the caller
    Q_PROPERTY(QVariantList radii READ radii WRITE setRadii NOTIFY radiiChanged)

public:
    explicit CssFillLayer(QQuickItem *parent = nullptr);

    QVariantMap spec() const { return m_spec; }
    void setSpec(const QVariantMap &v);

    QVariantList radii() const { return m_radii; }
    void setRadii(const QVariantList &v);

signals:
    void specChanged();
    void radiiChanged();

protected:
    void componentComplete() override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private:
    // Resolve kind, peakAlpha, fill colour, gradient params from spec + radii; push onto the
    // composed Shape shell (faithfully reproducing the QML `readonly property` bindings and
    // `opacity: peakAlpha`).
    void recompute();
    // Keep the composed Shape sized to us (C++ equivalent of `anchors.fill: parent`).
    void layoutChild();

    QVariantMap  m_spec;
    QVariantList m_radii = {0.0, 0.0, 0.0, 0.0};

    QPointer<QQuickItem> m_shape; // REAL QtQuick Shape (ShapePath + PathLine/Arc + gradients)
};
