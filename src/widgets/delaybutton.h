#pragma once

#include "qmlcss/cssfill.h"

#include <QPointer>

// DelayButton (port of DelayButton.qml). The C++ wrapper owns text/delay/disabled and the full
// button state list (hover/active/checked/focus/disabled — the snippet writes hovered/pressed/
// checked back, focus comes from the control's activeFocusChanged) so `.delay:active`/`:checked`
// restyle the pill; the T.DelayButton with the Basic-style hold transition, progress overlay and
// paint-owning `.delay` background rides the original QML body as a `root`-bound snippet.
namespace SolidWidgets {

class DelayButton : public QmlCss::CssFill {
    Q_OBJECT
    Q_PROPERTY(QString text READ text WRITE setText NOTIFY textChanged)
    Q_PROPERTY(int delay READ delay WRITE setDelay NOTIFY delayChanged)
    Q_PROPERTY(bool disabled READ disabled WRITE setDisabled NOTIFY disabledChanged)
    Q_PROPERTY(bool hovered READ hovered WRITE setHovered NOTIFY hoveredChanged)
    Q_PROPERTY(bool pressed READ pressed WRITE setPressed NOTIFY pressedChanged)
    Q_PROPERTY(bool checked READ checked WRITE setChecked NOTIFY checkedChanged)

public:
    explicit DelayButton(QQuickItem *parent = nullptr);

    QString text() const { return m_text; }
    void setText(const QString &v);
    int delay() const { return m_delay; }
    void setDelay(int v);
    bool disabled() const { return m_disabled; }
    void setDisabled(bool v);
    bool hovered() const { return m_hovered; }
    void setHovered(bool v);
    bool pressed() const { return m_pressed; }
    void setPressed(bool v);
    bool checked() const { return m_checked; }
    void setChecked(bool v);

signals:
    void textChanged();
    void delayChanged();
    void disabledChanged();
    void hoveredChanged();
    void pressedChanged();
    void checkedChanged();
    void activated();

protected:
    void componentComplete() override;

private:
    Q_SLOT void syncState();

    QString m_text;
    int m_delay = 300;
    bool m_disabled = false;
    bool m_hovered = false;
    bool m_pressed = false;
    bool m_checked = false;
    QPointer<QQuickItem> m_control;
};

} // namespace SolidWidgets
