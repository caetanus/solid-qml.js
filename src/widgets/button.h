#pragma once

#include "qmlcss/cssfill.h"
#include "qmlcss/csstext.h"

#include <QPointer>

// Button — the <button> component. The CssFill root owns CSS identity, layout and paint; the
// BEHAVIOR comes from a real QtQuick.Templates Button composed inside it (background/contentItem
// nulled, filling the root): click, Space/Enter activation, press/hover/focus states, auto-repeat
// and — crucially — the Button accessibility role/name all come from QQuickAbstractButton instead
// of being re-derived here. This is the same "native behavior + CSS visuals" split every other
// widget in this library uses (Checkbox wraps T.CheckBox, DelayButton wraps T.DelayButton, …).
//
// The transpiler emits:  W.Button { cssClass: […]; text: "Save"; onClicked: <handler>; <children> }
namespace SolidWidgets {

class Button : public QmlCss::CssFill {
    Q_OBJECT
    // The label text (text children of <button>). Element children (a nested <img>) land in the
    // default content slot and lay out alongside the label, exactly like the QML did.
    Q_PROPERTY(QString text READ text WRITE setText NOTIFY textChanged)
    // The default button (<button type="submit">): the enclosing Dialog fires it on Enter and
    // `button:default { … }` can emphasise it (surfaced as a "default" cssState).
    Q_PROPERTY(bool isDefault READ isDefault WRITE setIsDefault NOTIFY isDefaultChanged)
    // Disabled: no interaction, `:disabled` styling (RoundButton exposes it; harmless on <button>).
    Q_PROPERTY(bool disabled READ disabled WRITE setDisabled NOTIFY disabledChanged)

public:
    explicit Button(QQuickItem *parent = nullptr);

    QString text() const { return m_text; }
    void setText(const QString &v);

    bool isDefault() const { return m_isDefault; }
    void setIsDefault(bool v);

    bool disabled() const { return m_disabled; }
    void setDisabled(bool v);

    // Move keyboard focus onto the button — Dialog focuses the default button on open, so
    // Enter confirms it (study §6). Focus lives on the composed control (it is the tab stop).
    Q_INVOKABLE void takeFocus();

    // Called by the composed control whenever one of its states changes (hovered/down/activeFocus).
    Q_INVOKABLE void syncState();

signals:
    void textChanged();
    void isDefaultChanged();
    void disabledChanged();
    void clicked();

protected:
    void componentComplete() override;

private:
    void ensureLabel();
    void ensureControl();

    QString m_text;
    bool m_isDefault = false;
    bool m_disabled = false;
    QPointer<QmlCss::CssText> m_label;
    QPointer<QQuickItem> m_control; // the composed T.Button: all interaction + a11y
};

// RoundButton — same interaction surface as Button (the "round" class comes from the emit);
// pressed/disabled states style `:active`/`:disabled` like the T.RoundButton wrapper did.
class RoundButton : public Button {
    Q_OBJECT

public:
    using Button::Button;
};

// ToolButton — same interaction surface as Button (the "tool" class comes from the emit); the
// T.ToolButton the QML wrapped supplied the semantic role only, which the control covers.
class ToolButton : public Button {
    Q_OBJECT

public:
    using Button::Button;
};

} // namespace SolidWidgets
