#pragma once

#include <QPointer>
#include <QQuickItem>
#include <QString>
#include <QVariantList>

// CSS @keyframes driver, translated 1:1 from qml/qmlcss/CssKeyframes.qml — BY COMPOSITION.
// An invisible Item that animates `target[animatedProperty]` through a parsed @keyframes
// sequence by looping a normalised `progress` (0 → 1) over `duration` ms via a REAL
// QtQuick NumberAnimation (composed via the Qt type-system). On every progress tick the
// _numAt interpolation is applied and written to `target[animatedProperty]`.
//
// Equivalent QML this C++ composes:
//   import QtQuick
//   Item {                                     // <- this CssKeyframes (invisible, 0×0)
//       visible: false; width: 0; height: 0
//       property var frames: []
//       property string animatedProperty: ""
//       property var target: null
//       property int duration: 1000
//       property int easingType: Easing.InOutSine   // == 19
//       property int iterations: -1                 // -1 = infinite
//       property bool running: false
//       property real progress: 0.0
//
//       function _numAt(p) { ... }              // linear interp between keyframe offsets
//       onProgressChanged: { if target && prop, write _numAt(p) to target[prop] }
//
//       NumberAnimation on progress {           // <- m_anim (composed QObject, not QQuickItem)
//           from: 0.0; to: 1.0
//           duration: Math.max(1, root.duration)
//           loops: root.iterations < 0 ? Animation.Infinite : Math.max(1, root.iterations)
//           easing.type: root.easingType        // grouped — set via QQmlProperty
//           running: root.running && root.frames.length > 0
//       }
//   }
class CssKeyframes : public QQuickItem {
    Q_OBJECT
    // `frames`: list of {offset: 0..1, properties: {propName: "value"}} keyframe maps.
    Q_PROPERTY(QVariantList frames READ frames WRITE setFrames NOTIFY framesChanged)
    // Name of the property on `target` to animate (e.g. "opacity").
    Q_PROPERTY(QString animatedProperty READ animatedProperty WRITE setAnimatedProperty NOTIFY animatedPropertyChanged)
    // The QObject whose property is driven by the keyframe interpolation.
    Q_PROPERTY(QObject *target READ target WRITE setTarget NOTIFY targetChanged)
    Q_PROPERTY(int duration READ duration WRITE setDuration NOTIFY durationChanged)
    // QEasingCurve::Type value — default InOutSine (19). Passed to NumberAnimation easing.type
    // via QQmlProperty (grouped property; setProperty cannot resolve it).
    Q_PROPERTY(int easingType READ easingType WRITE setEasingType NOTIFY easingTypeChanged)
    // Number of cycles; -1 means infinite (Animation.Infinite).
    Q_PROPERTY(int iterations READ iterations WRITE setIterations NOTIFY iterationsChanged)
    Q_PROPERTY(bool running READ running WRITE setRunning NOTIFY runningChanged)
    // Normalised animation position 0..1, written by the composed NumberAnimation.
    // Setter applies the _numAt interpolation and writes to target[animatedProperty].
    Q_PROPERTY(qreal progress READ progress WRITE setProgress NOTIFY progressChanged)

public:
    explicit CssKeyframes(QQuickItem *parent = nullptr);

    QVariantList frames() const { return m_frames; }
    void setFrames(const QVariantList &v);

    QString animatedProperty() const { return m_animatedProperty; }
    void setAnimatedProperty(const QString &v);

    QObject *target() const { return m_target.data(); }
    void setTarget(QObject *v);

    int duration() const { return m_duration; }
    void setDuration(int v);

    int easingType() const { return m_easingType; }
    void setEasingType(int v);

    int iterations() const { return m_iterations; }
    void setIterations(int v);

    bool running() const { return m_running; }
    void setRunning(bool v);

    qreal progress() const { return m_progress; }
    void setProgress(qreal v);

signals:
    void framesChanged();
    void animatedPropertyChanged();
    void targetChanged();
    void durationChanged();
    void easingTypeChanged();
    void iterationsChanged();
    void runningChanged();
    void progressChanged();

protected:
    void componentComplete() override;

private:
    // Faithful C++ port of the QML `_numAt(p)` function: clamp at boundaries, linearly
    // interpolate between adjacent keyframe offsets. Returns qQNaN() when frames is empty.
    qreal numAt(qreal p) const;
    // Push current duration/loops/easing/running into the composed NumberAnimation.
    void updateAnimation();

    QVariantList m_frames;
    QString m_animatedProperty;
    QPointer<QObject> m_target;
    int m_duration = 1000;
    // QEasingCurve::InOutSine == 19 (== Easing.InOutSine in QML)
    int m_easingType = 19;
    int m_iterations = -1;
    bool m_running = false;
    qreal m_progress = 0.0;

    QPointer<QObject> m_anim; // REAL QtQuick NumberAnimation (QObject, not QQuickItem)
};
