#pragma once

#include "qmlcss/cssfill.h"
#include "qmlcss/csstext.h"

#include <QPointer>

// Button — the <button> component (port of Button.qml). Extends the engine's CssFill so it
// participates in CSS layout/paint; hover, focus, click and Space/Enter activation are native
// C++ event handlers now — no MouseArea/Keys objects. The C++ root takes keyboard focus itself
// (setActiveFocusOnTab is a plain QQuickItem call here; only the QML registration hid it).
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

public:
    explicit Button(QQuickItem *parent = nullptr);

    QString text() const { return m_text; }
    void setText(const QString &v);

    bool isDefault() const { return m_isDefault; }
    void setIsDefault(bool v);

    // Move keyboard focus onto the button — Dialog focuses the default button on open, so
    // Enter confirms it (study §6).
    Q_INVOKABLE void takeFocus() { forceActiveFocus(Qt::TabFocusReason); }

signals:
    void textChanged();
    void isDefaultChanged();
    void clicked();

protected:
    void componentComplete() override;
    void hoverEnterEvent(QHoverEvent *event) override;
    void hoverLeaveEvent(QHoverEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void itemChange(QQuickItem::ItemChange change, const QQuickItem::ItemChangeData &data) override;

private:
    Q_SLOT void syncTabstop();
    void syncState();
    void ensureLabel();

    QString m_text;
    bool m_isDefault = false;
    bool m_hovered = false;
    QPointer<QmlCss::CssText> m_label;
    QObject *m_tabstop = nullptr; // the loader's solidTabstop switch
};

} // namespace SolidWidgets
