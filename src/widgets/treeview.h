#pragma once

#include "qmlcss/cssfill.h"

#include <QVariant>

// TreeView (port of TreeView.qml). OUR OWN tree over plain { label, children? } objects — Qt's
// TreeView needs a QAbstractItemModel; virtualization is a later phase (sidebar/config-panel
// sized trees). The C++ wrapper carries __treeData/currentIndex and the selected(node) relay; the
// flattened `_visibleRows`, the collapsed-path state with its `_rev` bump, and the full QtWidgets
// keyboard model (one tab stop; Up/Down move, Right expands/descends, Left collapses/ascends,
// Enter toggles+commits, Space toggles) ride on the snippet host verbatim.
namespace SolidWidgets {

class TreeView : public QmlCss::CssFill {
    Q_OBJECT
    Q_PROPERTY(QVariant __treeData READ treeData WRITE setTreeData NOTIFY treeDataChanged)
    Q_PROPERTY(int currentIndex READ currentIndex WRITE setCurrentIndex NOTIFY currentIndexChanged)

public:
    explicit TreeView(QQuickItem *parent = nullptr);

    QVariant treeData() const { return m_treeData; }
    void setTreeData(const QVariant &v);
    int currentIndex() const { return m_currentIndex; }
    void setCurrentIndex(int v);

signals:
    void treeDataChanged();
    void currentIndexChanged();
    void selected(const QVariant &node);

protected:
    void componentComplete() override;

private:
    QVariant m_treeData = QVariantList();
    int m_currentIndex = -1;
};

} // namespace SolidWidgets
