#pragma once

#include <QDate>
#include <QQuickItem>
#include <QVariant>

// MonthGrid (port of MonthGrid.qml) — the shared month-grid calendar body, used by both Calendar
// (inline) and DateField (in its popup); module-internal, never emitted directly. The C++ root
// owns the date-state surface; the header (prev/next `.cal-nav` + `.cal-title`), the
// T.AbstractDayOfWeekRow and the T.AbstractMonthGrid with its day delegate ride as a snippet.
//
// viewMonth/viewYear keep the QML init-binding semantics: they FOLLOW selectedDate (or today)
// until first written — a nav click or DateField's imperative keyboard stepping — then stick.
namespace SolidWidgets {

class MonthGrid : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QVariant selectedDate READ selectedDate WRITE setSelectedDate NOTIFY selectedDateChanged)
    Q_PROPERTY(QVariant cursorDate READ cursorDate WRITE setCursorDate NOTIFY cursorDateChanged)
    Q_PROPERTY(int viewMonth READ viewMonth WRITE setViewMonth NOTIFY viewMonthChanged)
    Q_PROPERTY(int viewYear READ viewYear WRITE setViewYear NOTIFY viewYearChanged)

public:
    explicit MonthGrid(QQuickItem *parent = nullptr);

    QVariant selectedDate() const { return m_selectedDate; }
    void setSelectedDate(const QVariant &v);
    QVariant cursorDate() const { return m_cursorDate; }
    void setCursorDate(const QVariant &v);
    int viewMonth() const;
    void setViewMonth(int v);
    int viewYear() const;
    void setViewYear(int v);

signals:
    void selectedDateChanged();
    void cursorDateChanged();
    void viewMonthChanged();
    void viewYearChanged();
    void dayPicked(const QVariant &date);

protected:
    void componentComplete() override;

private:
    // The selected date if valid, else today (the QML fallbacks: `selectedDate instanceof Date
    // ? … : new Date()`).
    QDate baseDate() const;

    QVariant m_selectedDate;
    QVariant m_cursorDate;
    int m_viewMonth = 0;
    int m_viewYear = 0;
    bool m_viewMonthSet = false;
    bool m_viewYearSet = false;
};

} // namespace SolidWidgets
