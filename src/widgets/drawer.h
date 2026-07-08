#pragma once

#include "qmlcss/cssitem.h"

#include <QPointer>
#include <QQmlListProperty>

// Drawer (port of Drawer.qml). The C++ root is the zero-size in-tree anchor (a CssItem carrying
// the author's classes) so `.my-drawer .panel` rules scope through the cssAncestor re-anchor in
// the snippet; it paints nothing and flows as an empty box. The T.Drawer reparents itself to the
// window Overlay — background AND contentItem each carry `cssAncestor: root` (MANDATORY popup
// pitfall; sibling slots, together covering every descendant).
//
// The controlled `open` drives the drawer's visible (RestoreNone Binding in the emit writes
// root.open; the snippet forwards it and mirrors Qt-side closes — Esc / press outside — back,
// then fires closed()). The author's children route into the snippet contentItem's CONTENT slot
// (the engine's layout holder), buffered until the snippet composes.
namespace SolidWidgets {

class Drawer : public QmlCss::CssItem {
    Q_OBJECT
    Q_PROPERTY(int edge READ edge WRITE setEdge NOTIFY edgeChanged)
    Q_PROPERTY(qreal size READ size WRITE setSize NOTIFY sizeChanged)
    Q_PROPERTY(bool open READ open WRITE setOpen NOTIFY openChanged)
    Q_PROPERTY(QQmlListProperty<QObject> content READ contentSlot CONSTANT)
    Q_CLASSINFO("DefaultProperty", "content")

public:
    explicit Drawer(QQuickItem *parent = nullptr);

    int edge() const { return m_edge; }
    void setEdge(int v);
    qreal size() const { return m_size; }
    void setSize(qreal v);
    bool open() const { return m_open; }
    void setOpen(bool v);
    QQmlListProperty<QObject> contentSlot();

signals:
    void edgeChanged();
    void sizeChanged();
    void openChanged();
    void closed();

protected:
    void componentComplete() override;

private:
    static void content_append(QQmlListProperty<QObject> *prop, QObject *obj);
    static qsizetype content_count(QQmlListProperty<QObject> *prop);
    static QObject *content_at(QQmlListProperty<QObject> *prop, qsizetype index);

    void appendContent(QObject *obj);

    int m_edge = Qt::LeftEdge;
    qreal m_size = 0.34;
    bool m_open = false;
    QList<QObject *> m_pending;
    QPointer<QQuickItem> m_contentBox; // the snippet contentItem (CssFill ["content"])
};

} // namespace SolidWidgets
