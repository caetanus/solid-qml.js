#pragma once

#include <QQuickPaintedItem>
#include <QPointer>
#include "richtext_layout.h"

namespace rt {
// Public Qt Quick decoration layer. Its backing image is bounded by the viewport.
class TableDecoration final : public QQuickPaintedItem {
public:
    TableDecoration(RichTextLayout *layout, QQuickItem *editor);
    void paint(QPainter *) override;
private:
    void refreshViewport();
    QPointer<RichTextLayout> m_layout;
    QPointer<QQuickItem> m_editor;
    QPointF m_origin;
};
class NodeTableDecoration final : public QQuickItem {
public:
    NodeTableDecoration(RichTextLayout *layout, QQuickItem *editor);
protected:
    QSGNode *updatePaintNode(QSGNode *, UpdatePaintNodeData *) override;
private:
    void refreshViewport();
    QPointer<RichTextLayout> m_layout;
    QPointer<QQuickItem> m_editor;
    QRectF m_visible;
};

}
