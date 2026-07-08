#pragma once

#include "qmlcss/cssfill.h"

#include <QPointer>

// SpinBox + Dial (ports of SpinBox.qml / Dial.qml). The C++ wrapper owns the controlled `value`
// (RestoreNone Binding forwards root→control; user edits come back through onValueModified /
// onMoved with the wrapper refreshed BEFORE the relay fires) plus the range surface and the
// focus/active/disabled cssState; the T.SpinBox / T.Dial with their indicators, wheel stepping
// and handle math ride the original QML bodies as `root`-bound snippets.
namespace SolidWidgets {

class SpinBox : public QmlCss::CssFill {
    Q_OBJECT
    Q_PROPERTY(int value READ value WRITE setValue NOTIFY valueChanged)
    Q_PROPERTY(int from READ from WRITE setFrom NOTIFY fromChanged)
    Q_PROPERTY(int to READ to WRITE setTo NOTIFY toChanged)
    Q_PROPERTY(int stepSize READ stepSize WRITE setStepSize NOTIFY stepSizeChanged)

public:
    explicit SpinBox(QQuickItem *parent = nullptr);

    int value() const { return m_value; }
    void setValue(int v);
    int from() const { return m_from; }
    void setFrom(int v);
    int to() const { return m_to; }
    void setTo(int v);
    int stepSize() const { return m_stepSize; }
    void setStepSize(int v);

signals:
    void valueChanged();
    void fromChanged();
    void toChanged();
    void stepSizeChanged();
    void valueModified();

protected:
    void componentComplete() override;

private:
    Q_SLOT void syncState();

    int m_value = 0;
    int m_from = 0;
    int m_to = 99;
    int m_stepSize = 1;
    QPointer<QQuickItem> m_control;
};

// Dial — rotary <Dial>. `pressed` is written by the snippet (onPressedChanged) so the wrapper can
// carry the `active` cssState; the handle placement from `angle` stays in the snippet.
class Dial : public QmlCss::CssFill {
    Q_OBJECT
    Q_PROPERTY(qreal value READ value WRITE setValue NOTIFY valueChanged)
    Q_PROPERTY(qreal from READ from WRITE setFrom NOTIFY fromChanged)
    Q_PROPERTY(qreal to READ to WRITE setTo NOTIFY toChanged)
    Q_PROPERTY(qreal stepSize READ stepSize WRITE setStepSize NOTIFY stepSizeChanged)
    Q_PROPERTY(bool disabled READ disabled WRITE setDisabled NOTIFY disabledChanged)
    Q_PROPERTY(bool pressed READ pressed WRITE setPressed NOTIFY pressedChanged)

public:
    explicit Dial(QQuickItem *parent = nullptr);

    qreal value() const { return m_value; }
    void setValue(qreal v);
    qreal from() const { return m_from; }
    void setFrom(qreal v);
    qreal to() const { return m_to; }
    void setTo(qreal v);
    qreal stepSize() const { return m_stepSize; }
    void setStepSize(qreal v);
    bool disabled() const { return m_disabled; }
    void setDisabled(bool v);
    bool pressed() const { return m_pressed; }
    void setPressed(bool v);

signals:
    void valueChanged();
    void fromChanged();
    void toChanged();
    void stepSizeChanged();
    void disabledChanged();
    void pressedChanged();
    void moved();

protected:
    void componentComplete() override;

private:
    Q_SLOT void syncState();

    qreal m_value = 0;
    qreal m_from = 0;
    qreal m_to = 100;
    qreal m_stepSize = 1;
    bool m_disabled = false;
    bool m_pressed = false;
    QPointer<QQuickItem> m_control;
};

} // namespace SolidWidgets
