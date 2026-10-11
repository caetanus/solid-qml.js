#include "swipe.h"

#include "snippetwidget.h"

#include <QGuiApplication>
#include <QMouseEvent>
#include <QPropertyAnimation>
#include <QQuickItem>
#include <QStyleHints>
#include <QTimer>
#include <QWheelEvent>
#include <QtMath>
#include <QtQuick/private/qquicktranslate_p.h>

namespace {

// The label on the uncovered side — `root` = the C++ wrapper. A plain child of the box, stacked
// under the content holder (it shows only where the row has slid away); `.swipe-hint` styles it.
const char *kHintBody = R"(import QtQuick
import qmlcss 1.0 as Css

Item {
    anchors.fill: parent
    visible: root.dx !== 0
    Text {
        text: root.hint
        anchors.verticalCenter: parent.verticalCenter
        anchors.left: root.hintOnLeft ? parent.left : undefined
        anchors.right: root.hintOnLeft ? undefined : parent.right
        anchors.leftMargin: 20
        anchors.rightMargin: 20
        Css.CssItem { cssPrimitive: "text"; cssClass: ["swipe-hint"] }
    }
}
)";

// How long a touchpad may pause before the gesture counts as ended (for devices that send no
// ScrollEnd phase).
constexpr int kWheelPauseMs = 220;
constexpr int kSlideMs = 170;

} // namespace

namespace SolidWidgets {

Swipe::Swipe(QQuickItem *parent)
    : QmlCss::CssFill(parent)
{
    setCssPrimitive(QStringLiteral("div"));
    setFiltersChildMouseEvents(true);
    setAcceptedMouseButtons(Qt::LeftButton);
    m_anim = new QPropertyAnimation(this, "dx", this);
    m_anim->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_anim, &QPropertyAnimation::finished, this, [this] {
        if (m_pending.isEmpty())
            return;
        const QString a = m_pending;
        m_pending.clear();
        QPointer<Swipe> self(this);
        emit swiped(a);
        if (!self)
            return;
        // A row the action does not take away (its list did not change, or the app asked
        // something first) comes back after a moment; one the app removes — often a little
        // later, from its model — is gone before then.
        m_return->start();
    });
    m_return = new QTimer(this);
    m_return->setSingleShot(true);
    m_return->setInterval(450);
    connect(m_return, &QTimer::timeout, this, [this] {
        if (m_gesture == Gesture::Dragging || m_gesture == Gesture::Wheel)
            return;   // a gesture holds the row; its end settles it
        if (m_dx != 0 && m_anim->state() != QAbstractAnimation::Running)
            animateTo(0, QString());
    });
    m_wheelEnd = new QTimer(this);
    m_wheelEnd->setSingleShot(true);
    m_wheelEnd->setInterval(kWheelPauseMs);
    connect(m_wheelEnd, &QTimer::timeout, this, [this] {
        if (m_gesture == Gesture::Wheel)
            release();
    });
}

void Swipe::setRight(const QString &v) { if (m_right != v) { m_right = v; emit labelsChanged(); emit hintChanged(); } }
void Swipe::setFarRight(const QString &v) { if (m_farRight != v) { m_farRight = v; emit labelsChanged(); emit hintChanged(); } }
void Swipe::setLeft(const QString &v) { if (m_left != v) { m_left = v; emit labelsChanged(); emit hintChanged(); } }
void Swipe::setFarLeft(const QString &v) { if (m_farLeft != v) { m_farLeft = v; emit labelsChanged(); emit hintChanged(); } }

// Near: 15% of the width, between 56 and 120 px; far: 55%, at most 240 px past near (a wide row
// does not ask for a long trip), and at least 40 px past it.
qreal Swipe::nearAt() const { return qBound<qreal>(56, width() * 0.15, 120); }
qreal Swipe::farAt() const { return qMax(nearAt() + 40, qMin(width() * 0.55, nearAt() + 240)); }

QString Swipe::actionFor(qreal dx) const
{
    if (dx > 0 && !m_right.isEmpty()) {
        if (dx >= farAt() && !m_farRight.isEmpty())
            return QStringLiteral("far-right");
        if (dx >= nearAt())
            return QStringLiteral("right");
    } else if (dx < 0 && !m_left.isEmpty()) {
        if (-dx >= farAt() && !m_farLeft.isEmpty())
            return QStringLiteral("far-left");
        if (-dx >= nearAt())
            return QStringLiteral("left");
    }
    return QString();
}

QString Swipe::hint() const
{
    if (m_dx > 0)
        return m_action == QLatin1String("far-right") ? m_farRight : m_right;
    if (m_dx < 0)
        return m_action == QLatin1String("far-left") ? m_farLeft : m_left;
    return QString();
}

void Swipe::setDx(qreal v)
{
    if (qFuzzyCompare(m_dx + 1, v + 1))
        return;
    const int wasSide = (m_dx > 0) - (m_dx < 0);
    m_dx = v;
    const int side = (v > 0) - (v < 0);
    if (m_translate)
        m_translate->setX(v);
    // The slid content stays inside the box (a CssFill clips only under overflow: hidden).
    if (v != 0 && !clip()) {
        setClip(true);
        m_clipped = true;
    } else if (v == 0 && m_clipped) {
        setClip(false);
        m_clipped = false;
    }
    if ((side > 0) != (wasSide > 0))
        emit hintSideChanged();
    bool hintMoved = side != wasSide;   // the hint names the uncovered side, threshold or not
    // The action follows the row, except while it slides out or back: that keeps the colour.
    if (m_anim->state() != QAbstractAnimation::Running || v == 0) {
        const QString a = actionFor(v);
        if (a != m_action) {
            m_action = a;
            setCssState(a.isEmpty() ? QVariantList() : QVariantList{a});
            emit actionChanged();
            hintMoved = true;
        }
    }
    if (hintMoved)
        emit hintChanged();
    emit dxChanged();
}

void Swipe::componentComplete()
{
    QmlCss::CssFill::componentComplete();
    if (QQuickItem *holder = layoutContentHolder()) {
        m_translate = new QQuickTranslate(this);
        m_translate->appendToItem(holder);
        auto *hint = qobject_cast<QQuickItem *>(composeInternalPlain(this, QStringLiteral("solidwidgets-swipe-hint"), kHintBody));
        // Under the content: the holder, or the scroll Flickable around it when the box scrolls.
        QQuickItem *above = holder->parentItem() == this ? holder : scrollFlickable();
        if (hint && above && above->parentItem() == this)
            hint->stackBefore(above);
    }
}

void Swipe::follow(qreal dx)
{
    if (!canGo(dx))
        dx = 0;
    // Past the far threshold the row still follows, slower (it never leaves the box by hand).
    const qreal w = width();
    if (qAbs(dx) > w * 0.8)
        dx = (dx > 0 ? 1 : -1) * (w * 0.8 + (qAbs(dx) - w * 0.8) * 0.3);
    setDx(dx);
}

void Swipe::release()
{
    m_wheelEnd->stop();
    const QString a = actionFor(m_dx);
    m_gesture = Gesture::None;
    if (a.isEmpty()) {
        animateTo(0, QString());
        return;
    }
    animateTo(m_dx > 0 ? width() : -width(), a);
}

void Swipe::animateTo(qreal to, const QString &then)
{
    m_anim->stop();
    m_pending = then;
    m_anim->setDuration(kSlideMs);
    m_anim->setStartValue(m_dx);
    m_anim->setEndValue(to);
    m_anim->start();
}

bool Swipe::committed() const
{
    return m_anim->state() == QAbstractAnimation::Running && !m_pending.isEmpty();
}

void Swipe::press(QPointF scenePos)
{
    // A spring-back or a return is interrupted (the row is taken where it is); a committed
    // slide-out never is (see the callers).
    m_anim->stop();
    m_return->stop();
    m_gesture = Gesture::Pressed;
    m_pressAt = scenePos;
    m_startDx = m_dx;
}

void Swipe::settle()
{
    m_gesture = Gesture::None;
    setKeepMouseGrab(false);
    if (m_dx != 0 && m_anim->state() != QAbstractAnimation::Running)
        animateTo(0, QString());
}

void Swipe::reset()
{
    m_anim->stop();
    m_pending.clear();
    m_return->stop();
    m_wheelEnd->stop();
    m_gesture = Gesture::None;
    setKeepMouseGrab(false);
    setDx(0);
}

void Swipe::itemChange(ItemChange change, const ItemChangeData &value)
{
    QmlCss::CssFill::itemChange(change, value);
    if ((change == ItemParentHasChanged && !value.item) || (change == ItemSceneChange && !value.window))
        reset();
}

// ── Mouse: a press goes on to the child under it; a sideways move takes it over ──────────────

bool Swipe::childMouseEventFilter(QQuickItem *item, QEvent *event)
{
    Q_UNUSED(item);
    switch (event->type()) {
    case QEvent::MouseButtonPress: {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() != Qt::LeftButton)
            return false;
        if (committed()) {
            m_gesture = Gesture::None;
            return true;   // the row is on its way out: its action happens, the press is spent
        }
        press(me->scenePosition());
        return false;
    }
    case QEvent::MouseMove: {
        auto *me = static_cast<QMouseEvent *>(event);
        if (m_gesture == Gesture::Dragging) {
            follow(m_startDx + me->scenePosition().x() - m_pressAt.x());
            return true;
        }
        if (m_gesture != Gesture::Pressed)
            return false;
        const QPointF d = me->scenePosition() - m_pressAt;
        const int threshold = QGuiApplication::styleHints()->startDragDistance();
        if (qAbs(d.x()) > threshold && qAbs(d.x()) > 2 * qAbs(d.y()) && canGo(d.x())) {
            m_gesture = Gesture::Dragging;
            me->setExclusiveGrabber(me->point(0), this);
            setKeepMouseGrab(true);
            follow(m_startDx + d.x());
            return true;
        }
        if (qAbs(d.y()) > threshold)
            m_gesture = Gesture::None;   // a vertical drag: not ours
        return false;
    }
    case QEvent::MouseButtonRelease:
        if (m_gesture == Gesture::Dragging) {
            setKeepMouseGrab(false);
            release();
            return true;
        }
        settle();   // a click on a row left slid (or stopped mid-return) puts it back
        return false;
    default:
        return false;
    }
}

void Swipe::mousePressEvent(QMouseEvent *event)
{
    event->accept();
    if (committed()) {
        m_gesture = Gesture::None;
        return;
    }
    press(event->scenePosition());
}

void Swipe::mouseMoveEvent(QMouseEvent *event)
{
    const QPointF d = event->scenePosition() - m_pressAt;
    if (m_gesture == Gesture::Pressed) {
        const int threshold = QGuiApplication::styleHints()->startDragDistance();
        if (qAbs(d.x()) > threshold && qAbs(d.x()) > 2 * qAbs(d.y()) && canGo(d.x())) {
            m_gesture = Gesture::Dragging;
            setKeepMouseGrab(true);
        }
    }
    if (m_gesture == Gesture::Dragging)
        follow(m_startDx + d.x());
}

void Swipe::mouseReleaseEvent(QMouseEvent *event)
{
    Q_UNUSED(event);
    setKeepMouseGrab(false);
    if (m_gesture == Gesture::Dragging)
        release();
    else
        settle();
}

void Swipe::mouseUngrabEvent()
{
    // The grab was taken away (a popup, the window lost it): back in place, nothing done.
    if (m_gesture == Gesture::Dragging || m_gesture == Gesture::Pressed)
        settle();
}

// ── Touchpad: a two-finger sideways swipe arrives as horizontal wheel events ─────────────────

void Swipe::wheelEvent(QWheelEvent *event)
{
    const QPoint px = event->pixelDelta();
    const Qt::ScrollPhase phase = event->phase();
    if (m_gesture == Gesture::Wheel) {
        if (phase == Qt::ScrollEnd) {
            release();
        } else if (phase == Qt::ScrollMomentum) {
            release();
            m_gesture = Gesture::Momentum;   // swallow the rest of the coasting
        } else {
            m_wheelAcc += px.x();
            follow(m_startDx + m_wheelAcc);
            m_wheelEnd->start();
        }
        event->accept();
        return;
    }
    if (m_gesture == Gesture::Momentum) {
        // The coasting after the fingers lifted belongs to the finished swipe.
        if (phase == Qt::ScrollMomentum) {
            event->accept();
            return;
        }
        m_gesture = Gesture::None;
    }
    if (phase == Qt::ScrollMomentum) {
        event->ignore();   // a scroll's coasting (the list's), never the start of a swipe
        return;
    }
    // A mouse wheel (no pixel deltas) or a mostly vertical scroll: the list's.
    if (px.isNull() || qAbs(px.x()) <= qAbs(px.y()) || !canGo(px.x()) || m_gesture != Gesture::None) {
        event->ignore();
        return;
    }
    if (committed()) {
        event->accept();   // the row is on its way out
        return;
    }
    m_anim->stop();
    m_return->stop();
    m_gesture = Gesture::Wheel;
    m_startDx = m_dx;
    m_wheelAcc = px.x();
    follow(m_startDx + m_wheelAcc);
    m_wheelEnd->start();
    event->accept();
}

} // namespace SolidWidgets
