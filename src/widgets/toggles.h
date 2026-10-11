#pragma once

#include "qmlcss/cssfill.h"

#include <QPointer>

// Checkbox + Toggle (ports of Checkbox.qml / Toggle.qml). The C++ wrapper carries `checked`,
// the `toggled` relay and the checked/focus/disabled cssState; the T.CheckBox / T.Switch and
// their indicator visuals compose from the original QML bodies as snippets bound to `root`.
// The controlled-input contract holds: the emit's RestoreNone Binding writes root.checked, a
// snippet-side RestoreNone Binding forwards it into the control, and user toggles come back
// through onToggled (no echo on programmatic writes).
namespace SolidWidgets {

class ToggleBase : public QmlCss::CssFill {
    Q_OBJECT
    Q_PROPERTY(bool checked READ checked WRITE setChecked NOTIFY checkedChanged)
    // aria-label/aria-description (release-plan M5 follow-up): routed to the inner T.CheckBox /
    // T.Switch — the item that actually takes focus and answers QAccessible.
    Q_PROPERTY(QString accessibleName READ accessibleName WRITE setAccessibleName NOTIFY accessibleNameChanged)
    Q_PROPERTY(QString accessibleDescription READ accessibleDescription WRITE setAccessibleDescription NOTIFY accessibleDescriptionChanged)

public:
    explicit ToggleBase(QQuickItem *parent = nullptr);

    bool checked() const { return m_checked; }
    void setChecked(bool v);
    QString accessibleName() const { return m_accessibleName; }
    void setAccessibleName(const QString &v);
    QString accessibleDescription() const { return m_accessibleDescription; }
    void setAccessibleDescription(const QString &v);

signals:
    void checkedChanged();
    void accessibleNameChanged();
    void accessibleDescriptionChanged();
    void toggled();

protected:
    void componentComplete() override;

    // The subclass's control snippet (created at completion; `root` = this).
    virtual const char *snippet() const = 0;
    virtual QString snippetKey() const = 0;

private:
    Q_SLOT void syncState();

    bool m_checked = false;
    QString m_accessibleName;
    QString m_accessibleDescription;
    QPointer<QQuickItem> m_control;
};

// Checkbox — <input type="checkbox">: 20×20 indicator with the ✓ glyph.
class Checkbox : public ToggleBase {
    Q_OBJECT

public:
    explicit Checkbox(QQuickItem *parent = nullptr);

protected:
    const char *snippet() const override;
    QString snippetKey() const override { return QStringLiteral("solidwidgets-checkbox"); }
};

// Toggle — <input type="checkbox" role="switch">: 36×20 track with the sliding knob.
class Toggle : public ToggleBase {
    Q_OBJECT

public:
    explicit Toggle(QQuickItem *parent = nullptr);

protected:
    const char *snippet() const override;
    QString snippetKey() const override { return QStringLiteral("solidwidgets-toggle"); }
};

} // namespace SolidWidgets
