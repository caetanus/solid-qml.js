#pragma once

#include "qmlcss/cssfill.h"

#include <QVariant>

// DateField (port of DateField.qml) — <input type="date">: a readOnly T.TextField (Qt.formatDate)
// with a `.chevron` glyph and a T.Popup hosting the shared W.MonthGrid. The C++ wrapper carries
// `value`, the keyboard `cursor`, the dayPicked relay and the cssState (`:focus` while the popup
// is open — the snippet pushes `popupVisible` back — `:disabled` from enabled). The keyboard
// stepping (±1/±7 with imperative viewMonth/viewYear writes), the toggle-guarded MouseArea, the
// flip-above popup and the cssAncestor re-anchors all ride in the snippet verbatim.
namespace SolidWidgets {

class DateField : public QmlCss::CssFill {
    Q_OBJECT
    Q_PROPERTY(QVariant value READ value WRITE setValue NOTIFY valueChanged)
    Q_PROPERTY(QVariant cursor READ cursorValue WRITE setCursorValue NOTIFY cursorChanged)
    Q_PROPERTY(bool popupVisible READ popupVisible WRITE setPopupVisible NOTIFY popupVisibleChanged)

public:
    explicit DateField(QQuickItem *parent = nullptr);

    QVariant value() const { return m_value; }
    void setValue(const QVariant &v);
    // Named to dodge QQuickItem::cursor()/setCursor(QCursor); the QML property stays `cursor`.
    QVariant cursorValue() const { return m_cursor; }
    void setCursorValue(const QVariant &v);
    bool popupVisible() const { return m_popupVisible; }
    void setPopupVisible(bool v);

signals:
    void valueChanged();
    void cursorChanged();
    void popupVisibleChanged();
    void dayPicked(const QVariant &date);

protected:
    void componentComplete() override;

private:
    Q_SLOT void syncState();

    QVariant m_value;
    QVariant m_cursor;
    bool m_popupVisible = false;
};

} // namespace SolidWidgets
