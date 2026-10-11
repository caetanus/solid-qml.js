#pragma once

#include "qmlcss/cssfill.h"

#include <QPointer>
#include <QPointF>

class QPropertyAnimation;
class QQuickTranslate;
class QTimer;

// Swipe — a row that is pushed sideways to act on it (Mailbox's list): <Swipe right="Arquivar"
// farRight="Lixo" left="Adiar" farLeft="Lista" onSwipe={(action) => …}>children</Swipe>.
//
// The children slide (a Translate on the content holder: no relayout while dragging); the box
// itself stays and shows what is behind — its CSS state names the action the row is past
// (`:right` `:far-right` `:left` `:far-left`, so the app colours each one) and a hint label (the
// prop's text, class "swipe-hint") sits on the uncovered side. Released past a threshold, the row
// slides out and `swiped(action)` fires; short of it, the row springs back.
//
// Input, desktop first:
//  - mouse: a horizontal drag anywhere on the row — the box filters its children's mouse events,
//    so a press still clicks the child under it; only a move that is clearly sideways (past the
//    drag threshold, |dx| > 2|dy|) takes the grab, and the child's press is cancelled;
//  - touchpad: a two-finger sideways swipe — horizontal wheel events with pixel deltas. A gesture
//    starts on an event that is mostly horizontal (a vertical one goes on to the list that
//    scrolls); it ends at the ScrollEnd phase, or after a short pause when the device sends none.
//    Momentum events after the fingers lift are swallowed.
// A direction whose label is empty does not swipe; an empty far label makes the near action
// cover any distance. Once a release has committed an action, it happens: a press during the
// slide-out does not cancel it.
//
// There is no keyboard or accessibility path through a swipe: it is a shortcut, and the app must
// offer the same actions another way (a menu, a button, a key).
namespace SolidWidgets {

class Swipe : public QmlCss::CssFill {
    Q_OBJECT
    // The TSX props right/farRight/left/farLeft (QQuickItem already has `left`/`right`).
    Q_PROPERTY(QString rightLabel READ right WRITE setRight NOTIFY labelsChanged)
    Q_PROPERTY(QString farRightLabel READ farRight WRITE setFarRight NOTIFY labelsChanged)
    Q_PROPERTY(QString leftLabel READ left WRITE setLeft NOTIFY labelsChanged)
    Q_PROPERTY(QString farLeftLabel READ farLeft WRITE setFarLeft NOTIFY labelsChanged)
    // The slide (px; + to the right). Written by the gesture and by the release animation.
    Q_PROPERTY(qreal dx READ dx WRITE setDx NOTIFY dxChanged)
    // What a release now would do ("", "right", "far-right", "left", "far-left") and its label.
    Q_PROPERTY(QString action READ action NOTIFY actionChanged)
    Q_PROPERTY(QString hint READ hint NOTIFY hintChanged)
    Q_PROPERTY(bool hintOnLeft READ hintOnLeft NOTIFY hintSideChanged)

public:
    explicit Swipe(QQuickItem *parent = nullptr);

    QString right() const { return m_right; }
    void setRight(const QString &v);
    QString farRight() const { return m_farRight; }
    void setFarRight(const QString &v);
    QString left() const { return m_left; }
    void setLeft(const QString &v);
    QString farLeft() const { return m_farLeft; }
    void setFarLeft(const QString &v);
    qreal dx() const { return m_dx; }
    void setDx(qreal v);
    QString action() const { return m_action; }
    QString hint() const;
    bool hintOnLeft() const { return m_dx > 0; }

signals:
    void labelsChanged();
    void dxChanged();
    void actionChanged();
    void hintSideChanged();
    void hintChanged();   // the side (or none), the action, or a label changed
    void swiped(const QString &action);

protected:
    void componentComplete() override;
    bool childMouseEventFilter(QQuickItem *item, QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseUngrabEvent() override;
    void wheelEvent(QWheelEvent *event) override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;

private:
    enum class Gesture { None, Pressed, Dragging, Wheel, Momentum };

    // The near and far thresholds for this width (px).
    qreal nearAt() const;
    qreal farAt() const;
    QString actionFor(qreal dx) const;
    bool canGo(qreal dx) const { return dx > 0 ? !m_right.isEmpty() : dx < 0 ? !m_left.isEmpty() : true; }
    void follow(qreal dx);   // a gesture moved the row: clamped to the directions that swipe
    void release();         // the gesture ended: out and act, or back
    void animateTo(qreal to, const QString &then);
    bool committed() const;  // sliding out after a release past a threshold: the action will fire
    void press(QPointF scenePos);
    void settle();          // a gesture ended without an action: back in place if it moved
    void reset();           // taken out of the scene (or recycled): no slide, no pending action

    QString m_right, m_farRight, m_left, m_farLeft;
    qreal m_dx = 0;
    QString m_action;
    Gesture m_gesture = Gesture::None;
    QPointF m_pressAt;
    qreal m_startDx = 0;
    qreal m_wheelAcc = 0;   // the touchpad gesture's travel so far
    QQuickTranslate *m_translate = nullptr;
    QPropertyAnimation *m_anim = nullptr;
    QString m_pending;      // the action to fire when the slide-out animation ends
    QTimer *m_wheelEnd = nullptr;
    QTimer *m_return = nullptr;   // brings back a row its action did not take away
    bool m_clipped = false;       // clip() was turned on by the slide (not by the app's overflow)
};

} // namespace SolidWidgets
