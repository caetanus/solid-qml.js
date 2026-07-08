#pragma once

#include "qmlcss/cssfill.h"

#include <QVariant>

// TableView (port of TableView.qml). A CssFill "div" wrapper over a header row + a REAL QtQuick
// ListView of data rows (row virtualization) with DYNAMIC columns — QtQuick's TableView needs a
// static TableModel per column, so header + ListView is what gives author-driven columns AND
// recycling. The C++ wrapper carries __columns/__rows/currentIndex and the selected(row, index)
// relay; the sort state, normalized _cols, sorted _rows, the toggle-sort squelch dance and the
// keyboard model all ride on the snippet host verbatim.
namespace SolidWidgets {

class TableView : public QmlCss::CssFill {
    Q_OBJECT
    Q_PROPERTY(QVariant __columns READ columns WRITE setColumns NOTIFY columnsChanged)
    Q_PROPERTY(QVariant __rows READ rows WRITE setRows NOTIFY rowsChanged)
    Q_PROPERTY(int currentIndex READ currentIndex WRITE setCurrentIndex NOTIFY currentIndexChanged)

public:
    explicit TableView(QQuickItem *parent = nullptr);

    QVariant columns() const { return m_columns; }
    void setColumns(const QVariant &v);
    QVariant rows() const { return m_rows; }
    void setRows(const QVariant &v);
    int currentIndex() const { return m_currentIndex; }
    void setCurrentIndex(int v);

signals:
    void columnsChanged();
    void rowsChanged();
    void currentIndexChanged();
    void selected(const QVariant &row, int index);

protected:
    void componentComplete() override;

private:
    QVariant m_columns = QVariantList();
    QVariant m_rows = QVariantList();
    int m_currentIndex = -1;
};

} // namespace SolidWidgets
