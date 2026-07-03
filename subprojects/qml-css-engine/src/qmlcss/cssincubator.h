#pragma once

#include <QPointer>
#include <QQmlComponent>
#include <QQmlIncubator>
#include <QQuickItem>

// Async branch mount (QQmlIncubator, driven by the window's incubation controller): while
// `active`, incubates `sourceComponent` asynchronously and — like Repeater does for its
// delegates — reparents the ready item to OUR parent (the box's contentHolder), so the item
// is a direct layout child and the CssLayoutEngine sizes it normally. This element itself
// carries no CSS signature and is ignored by the layout. Deactivation destroys the item.
//
// Emitted by the transpiler for <Switch>/<Match> branches, so a page switch responds on the
// next frame and the page streams in instead of blocking the click.
class CssIncubator : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(QQmlComponent *sourceComponent READ sourceComponent WRITE setSourceComponent
                   NOTIFY sourceComponentChanged)

public:
    explicit CssIncubator(QQuickItem *parent = nullptr);
    ~CssIncubator() override;

    bool active() const { return m_active; }
    void setActive(bool v);

    QQmlComponent *sourceComponent() const { return m_component; }
    void setSourceComponent(QQmlComponent *c);

signals:
    void activeChanged();
    void sourceComponentChanged();
    void itemChanged();

protected:
    void componentComplete() override;

private:
    class Incubator : public QQmlIncubator {
    public:
        explicit Incubator(CssIncubator *owner)
            : QQmlIncubator(Asynchronous)
            , m_owner(owner)
        {
        }

    protected:
        void statusChanged(Status s) override;

    private:
        CssIncubator *m_owner;
    };

    void sync();     // reconcile active/component with the incubation/item state
    void ready();    // incubation finished: adopt + reparent the item
    void teardown(); // cancel incubation and destroy the item

    bool m_active = false;
    QQmlComponent *m_component = nullptr;
    Incubator *m_incubator = nullptr;
    QPointer<QQuickItem> m_item;
};
