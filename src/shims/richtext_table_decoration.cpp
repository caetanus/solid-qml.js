#include "richtext_table_decoration.h"
#include <QPainter>
#include <QQuickWindow>
#include <QSGRectangleNode>
#include <QSGRenderNode>
#include <QSGRendererInterface>

namespace rt {
namespace {
class SoftwareTableNode final : public QSGRenderNode {
public:
    struct Band { QRectF rect; QColor color; };
    QList<Band> bands;
    QRectF bounds;
    QQuickWindow *window;
    explicit SoftwareTableNode(QQuickWindow *w) : window(w) {}
    QRectF rect() const override { return bounds; }
    RenderingFlags flags() const override { return BoundedRectRendering | NoExternalRendering; }
    StateFlags changedStates() const override { return {}; }
    void render(const RenderState *state) override {
        auto *painter=static_cast<QPainter *>(window->rendererInterface()->getResource(window,QSGRendererInterface::PainterResource));
        if (!painter) return;
        painter->save(); painter->resetTransform();
        if (state->clipRegion()) painter->setClipRegion(*state->clipRegion(),Qt::IntersectClip);
        painter->setRenderHint(QPainter::Antialiasing,false);
        painter->setOpacity(inheritedOpacity());
        painter->setWorldTransform(matrix()->toTransform());
        for (const auto &band:bands)
            painter->fillRect(band.rect.toAlignedRect(),band.color);
        painter->restore();
    }
};
}
TableDecoration::TableDecoration(RichTextLayout *layout, QQuickItem *editor)
    : QQuickPaintedItem(editor), m_layout(layout), m_editor(editor)
{
    setZ(-1);
    setAcceptedMouseButtons(Qt::NoButton);
    setFlag(ItemObservesViewport, true);
    connect(layout, &QAbstractTextDocumentLayout::update, this, [this] { refreshViewport(); update(); });
    connect(layout, &QObject::destroyed, this, [this] { setVisible(false); deleteLater(); });
    // Flickable scrolling moves an ancestor content item. Observe that public
    // geometry instead of allocating an image as tall as the document.
    for (auto *item=editor;item;item=item->parentItem()) {
        connect(item,&QQuickItem::xChanged,this,&TableDecoration::refreshViewport);
        connect(item,&QQuickItem::yChanged,this,&TableDecoration::refreshViewport);
        connect(item,&QQuickItem::widthChanged,this,&TableDecoration::refreshViewport);
        connect(item,&QQuickItem::heightChanged,this,&TableDecoration::refreshViewport);
        connect(item,&QQuickItem::scaleChanged,this,&TableDecoration::refreshViewport);
    }
    const auto attachWindow=[this](QQuickWindow *window) {
        if (window) {
            connect(window,&QWindow::widthChanged,this,&TableDecoration::refreshViewport);
            connect(window,&QWindow::heightChanged,this,&TableDecoration::refreshViewport);
        }
        refreshViewport();
    };
    connect(editor,&QQuickItem::windowChanged,this,attachWindow);
    attachWindow(editor->window());
}
void TableDecoration::refreshViewport()
{
    if (!m_layout || !m_editor || !window()) return;
    auto *viewport=viewportItem();
    if (!viewport) viewport=window()->contentItem();
    const QRectF visible=m_editor->mapRectFromItem(viewport,viewport->clipRect())
        .intersected(QRectF({},m_layout->documentSize()));
    m_origin=visible.topLeft();
    setPosition(m_origin); setSize(visible.size());
    update();
}
void TableDecoration::paint(QPainter *painter)
{
    if (!m_layout) return;
    painter->translate(-m_origin);
    m_layout->drawTableDecorations(painter,QRectF(m_origin,size()),true);
}
NodeTableDecoration::NodeTableDecoration(RichTextLayout *layout, QQuickItem *editor)
    : QQuickItem(editor), m_layout(layout), m_editor(editor)
{
    setZ(-1); setAcceptedMouseButtons(Qt::NoButton);
    setFlag(ItemHasContents,true); setFlag(ItemObservesViewport,true);
    connect(layout,&QAbstractTextDocumentLayout::update,this,[this] { refreshViewport(); update(); });
    connect(layout,&QObject::destroyed,this,[this] { setVisible(false); deleteLater(); });
    for (auto *item=editor;item;item=item->parentItem()) {
        connect(item,&QQuickItem::xChanged,this,&NodeTableDecoration::refreshViewport);
        connect(item,&QQuickItem::yChanged,this,&NodeTableDecoration::refreshViewport);
        connect(item,&QQuickItem::widthChanged,this,&NodeTableDecoration::refreshViewport);
        connect(item,&QQuickItem::heightChanged,this,&NodeTableDecoration::refreshViewport);
        connect(item,&QQuickItem::scaleChanged,this,&NodeTableDecoration::refreshViewport);
    }
    const auto attachWindow=[this](QQuickWindow *window) {
        if (window) {
            connect(window,&QWindow::widthChanged,this,&NodeTableDecoration::refreshViewport);
            connect(window,&QWindow::heightChanged,this,&NodeTableDecoration::refreshViewport);
        }
        refreshViewport();
    };
    connect(editor,&QQuickItem::windowChanged,this,attachWindow);
    attachWindow(editor->window());
}
void NodeTableDecoration::refreshViewport()
{
    if (!m_layout || !m_editor || !window()) return;
    auto *viewport=viewportItem();
    if (!viewport) viewport=window()->contentItem();
    m_visible=m_editor->mapRectFromItem(viewport,viewport->clipRect());
    setSize(m_layout->documentSize()); update();
}
QSGNode *NodeTableDecoration::updatePaintNode(QSGNode *root, UpdatePaintNodeData *)
{
    const bool software=window()->rendererInterface()->graphicsApi()==QSGRendererInterface::Software;
    if (!root) root=software ? static_cast<QSGNode *>(new SoftwareTableNode(window())) : new QSGNode;
    auto *softwareNode=software ? static_cast<SoftwareTableNode *>(root) : nullptr;
    if (softwareNode) { softwareNode->bands.clear(); softwareNode->bounds=m_visible; }
    auto *next=root->firstChild();
    const auto rectangle=[&](const QRectF &rect,const QBrush &brush) {
        if (brush.style()==Qt::NoBrush) return;
        if (softwareNode) { softwareNode->bands.append({rect,brush.color()}); return; }
        QSGRectangleNode *node;
        if (next) { node=static_cast<QSGRectangleNode *>(next); next=next->nextSibling(); }
        else { node=window()->createRectangleNode(); root->appendChildNode(node); }
        node->setRect(rect); node->setColor(brush.color());
    };
    if (m_layout) for (const auto &fragment:m_layout->tableFragments()) {
        if (!fragment.rect.adjusted(-1,-1,1,1).intersects(m_visible)) continue;
        rectangle(fragment.rect,fragment.background);
        if (fragment.border.style()==Qt::NoPen) continue;
        const qreal width=fragment.border.widthF();
        const qreal inset=fragment.collapsed ? -width/2 : 0;
        const auto rect=fragment.rect.translated(inset,inset);
        const auto &brush=fragment.border.brush();
        rectangle(QRectF(rect.left(),rect.top(),width,rect.height()+width),brush);
        rectangle(QRectF(rect.right(),rect.top(),width,rect.height()+width),brush);
        if (fragment.top) rectangle(QRectF(rect.left(),rect.top(),rect.width()+width,width),brush);
        if (fragment.bottom) rectangle(QRectF(rect.left(),rect.bottom(),rect.width()+width,width),brush);
    }
    while (next) { auto *unused=next; next=next->nextSibling(); root->removeChildNode(unused); delete unused; }
    if (softwareNode) softwareNode->markDirty(QSGNode::DirtyGeometry);
    return root;
}
}
