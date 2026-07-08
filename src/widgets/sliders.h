#pragma once

#include "qmlcss/cssfill.h"

#include <QPointer>

// Slider + RangeSlider (ports of Slider.qml / RangeSlider.qml). The C++ wrapper owns the value
// surface and the focus/disabled cssState; the T.Slider / T.RangeSlider with their Basic-style
// track/fill/handle visuals and the focused-wheel stepping ride the original QML bodies as
// `root`-bound snippets. Controlled contract: the emit's RestoreNone Binding writes root.value,
// a snippet-side RestoreNone Binding forwards it into the control, user drags come back through
// onMoved (root.value refreshed BEFORE moved() fires, so handlers read the new value).
namespace SolidWidgets {

class Slider : public QmlCss::CssFill {
    Q_OBJECT
    Q_PROPERTY(qreal value READ value WRITE setValue NOTIFY valueChanged)
    Q_PROPERTY(qreal from READ from WRITE setFrom NOTIFY fromChanged)
    Q_PROPERTY(qreal to READ to WRITE setTo NOTIFY toChanged)
    Q_PROPERTY(qreal stepSize READ stepSize WRITE setStepSize NOTIFY stepSizeChanged)

public:
    explicit Slider(QQuickItem *parent = nullptr);

    qreal value() const { return m_value; }
    void setValue(qreal v);
    qreal from() const { return m_from; }
    void setFrom(qreal v);
    qreal to() const { return m_to; }
    void setTo(qreal v);
    qreal stepSize() const { return m_stepSize; }
    void setStepSize(qreal v);

signals:
    void valueChanged();
    void fromChanged();
    void toChanged();
    void stepSizeChanged();
    void moved();

protected:
    void componentComplete() override;

private:
    Q_SLOT void syncState();

    qreal m_value = 0;
    qreal m_from = 0;
    qreal m_to = 1;
    qreal m_stepSize = 0;
    QPointer<QQuickItem> m_control;
};

// RangeSlider — two handles. The emit binds the control's NODES across the boundary
// (`__inputN.first.value` reads; `Binding { target: __inputN.first }` writes), so `first` and
// `second` expose the T.RangeSlider node objects once the snippet composes.
class RangeSlider : public QmlCss::CssFill {
    Q_OBJECT
    Q_PROPERTY(QObject *first READ first NOTIFY nodesChanged)
    Q_PROPERTY(QObject *second READ second NOTIFY nodesChanged)
    Q_PROPERTY(qreal from READ from WRITE setFrom NOTIFY fromChanged)
    Q_PROPERTY(qreal to READ to WRITE setTo NOTIFY toChanged)
    Q_PROPERTY(qreal stepSize READ stepSize WRITE setStepSize NOTIFY stepSizeChanged)
    Q_PROPERTY(bool disabled READ disabled WRITE setDisabled NOTIFY disabledChanged)

public:
    explicit RangeSlider(QQuickItem *parent = nullptr);

    QObject *first() const { return m_firstNode; }
    QObject *second() const { return m_secondNode; }
    qreal from() const { return m_from; }
    void setFrom(qreal v);
    qreal to() const { return m_to; }
    void setTo(qreal v);
    qreal stepSize() const { return m_stepSize; }
    void setStepSize(qreal v);
    bool disabled() const { return m_disabled; }
    void setDisabled(bool v);

signals:
    void nodesChanged();
    void fromChanged();
    void toChanged();
    void stepSizeChanged();
    void disabledChanged();
    void moved();

protected:
    void componentComplete() override;

private:
    Q_SLOT void syncState();

    QObject *m_firstNode = nullptr;
    QObject *m_secondNode = nullptr;
    qreal m_from = 0;
    qreal m_to = 100;
    qreal m_stepSize = 1;
    bool m_disabled = false;
    QPointer<QQuickItem> m_control;
};

} // namespace SolidWidgets
