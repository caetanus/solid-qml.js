#include "qmlcss/componentcache.h"
#include "qmlcss/csskeyframes.h"

#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlProperty>
#include <QUrl>

#include <cmath>

// Equivalent QML (see csskeyframes.h for the annotated version):
//   NumberAnimation on progress {
//       from: 0.0; to: 1.0
//       duration: Math.max(1, root.duration)
//       loops: root.iterations < 0 ? Animation.Infinite : Math.max(1, root.iterations)
//       easing.type: root.easingType   // via QQmlProperty (grouped)
//       running: root.running && root.frames.length > 0
//   }

CssKeyframes::CssKeyframes(QQuickItem *parent)
    : QQuickItem(parent)
{
    // QML: visible: false; width: 0; height: 0
    setVisible(false);
    setWidth(0);
    setHeight(0);
}

// ---------- numAt -----------------------------------------------------------
// Literal C++ port of the QML `function _numAt(p)`:
//   if no frames: NaN
//   if p <= first.offset: first keyframe value
//   linear interp between bracketing keyframes
//   if p > last.offset: last keyframe value
qreal CssKeyframes::numAt(qreal p) const
{
    if (m_frames.isEmpty())
        return qQNaN();

    auto asMap  = [](const QVariant &v) { return v.toMap(); };
    auto offset = [&asMap](const QVariant &v) {
        return asMap(v).value(QStringLiteral("offset")).toReal();
    };
    auto propVal = [&](const QVariant &v) -> qreal {
        return asMap(v).value(QStringLiteral("properties")).toMap()
               .value(m_animatedProperty).toReal();
    };

    // QML: if (p <= ks[0].offset) return parseFloat(ks[0].properties[prop])
    if (p <= offset(m_frames.first()))
        return propVal(m_frames.first());

    // QML: for i in 0..length-2: if p in [a.offset, b.offset] -> interpolate
    for (int i = 0; i < m_frames.size() - 1; ++i) {
        const qreal oa = offset(m_frames[i]);
        const qreal ob = offset(m_frames[i + 1]);
        if (p >= oa && p <= ob) {
            const qreal span = ob - oa;
            const qreal t    = span > 0 ? (p - oa) / span : 0.0;
            const qreal va   = propVal(m_frames[i]);
            const qreal vb   = propVal(m_frames[i + 1]);
            return va + (vb - va) * t;
        }
    }

    // QML: return parseFloat(ks[last].properties[prop])
    return propVal(m_frames.last());
}

// ---------- property setters ------------------------------------------------

void CssKeyframes::setFrames(const QVariantList &v)
{
    if (m_frames == v)
        return;
    m_frames = v;
    emit framesChanged();
    updateAnimation(); // running condition: frames.length > 0
}

void CssKeyframes::setAnimatedProperty(const QString &v)
{
    if (m_animatedProperty == v)
        return;
    m_animatedProperty = v;
    emit animatedPropertyChanged();
}

void CssKeyframes::setTarget(QObject *v)
{
    if (m_target == v)
        return;
    m_target = v;
    emit targetChanged();
}

void CssKeyframes::setDuration(int v)
{
    if (m_duration == v)
        return;
    m_duration = v;
    emit durationChanged();
    updateAnimation();
}

void CssKeyframes::setEasingType(int v)
{
    if (m_easingType == v)
        return;
    m_easingType = v;
    emit easingTypeChanged();
    updateAnimation();
}

void CssKeyframes::setIterations(int v)
{
    if (m_iterations == v)
        return;
    m_iterations = v;
    emit iterationsChanged();
    updateAnimation();
}

void CssKeyframes::setRunning(bool v)
{
    if (m_running == v)
        return;
    m_running = v;
    emit runningChanged();
    updateAnimation();
}

void CssKeyframes::setProgress(qreal v)
{
    // Called by the composed NumberAnimation on every tick.
    m_progress = v;
    emit progressChanged();

    // QML: onProgressChanged { if (target && animatedProperty) target[prop] = _numAt(p) }
    if (!m_target || m_animatedProperty.isEmpty())
        return;
    const qreal val = numAt(m_progress);
    if (!std::isnan(val))
        m_target->setProperty(m_animatedProperty.toUtf8().constData(), val);
}

// ---------- animation sync --------------------------------------------------

void CssKeyframes::updateAnimation()
{
    if (!m_anim)
        return;
    // QML: duration: Math.max(1, root.duration)
    m_anim->setProperty("duration", qMax(1, m_duration));
    // QML: loops: root.iterations < 0 ? Animation.Infinite : Math.max(1, root.iterations)
    // Animation.Infinite == QAbstractAnimation::Infinite == -1
    m_anim->setProperty("loops", m_iterations < 0 ? -1 : qMax(1, m_iterations));
    // QML: easing.type: root.easingType — grouped property, setProperty cannot resolve it
    QQmlProperty(m_anim.data(), QStringLiteral("easing.type")).write(m_easingType);
    // QML: running: root.running && root.frames && root.frames.length > 0
    m_anim->setProperty("running", m_running && !m_frames.isEmpty());
}

// ---------- componentComplete -----------------------------------------------

void CssKeyframes::componentComplete()
{
    QQuickItem::componentComplete();

    if (QQmlEngine *eng = qmlEngine(this)) {
        // REAL QtQuick NumberAnimation animating this item's `progress` property from 0 to 1.
        // It is a QObject (not a QQuickItem), so setParent keeps ownership.
        QQmlComponent *comp = QmlCss::cachedComponent(eng, QStringLiteral("csskeyframes-fd9161da"),
            
            "import QtQuick\n"
            "NumberAnimation { from: 0.0; to: 1.0 }\n");
        if (QObject *o = comp->create(qmlContext(this))) {
            o->setParent(this);
            // Target this C++ item's `progress` Q_PROPERTY.
            o->setProperty("target", QVariant::fromValue(static_cast<QObject *>(this)));
            o->setProperty("property", QStringLiteral("progress"));
            m_anim = o;
        } else {
            qWarning("CssKeyframes: failed to compose NumberAnimation: %s",
                     qPrintable(comp->errorString()));
        }
    }

    updateAnimation();
}
