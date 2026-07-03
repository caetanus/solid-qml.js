#pragma once

#include <QPointer>
#include <QQmlComponent>
#include <QQuickItem>
#include <QVariantList>
#include <QVector>

// Keyed repeater with FLYWEIGHT semantics — Solid's <For>: when the model array changes,
// entries whose VALUE survives keep their existing delegate (moved/re-stacked to the new
// position, never recreated); removed entries destroy theirs; only genuinely new entries
// instantiate. A plain QML Repeater tears everything down on any wholesale model change —
// a drag-and-drop reorder recreated every card.
//
// Like Repeater, delegates are reparented to OUR parent (the box's contentHolder), so the
// layout engine treats them as direct children; document order (childItems order) follows
// the model via stacking. Each delegate's context exposes `modelData` and `index`.
class CssRepeater : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QVariant model READ model WRITE setModel NOTIFY modelChanged)
    Q_PROPERTY(QQmlComponent *delegate READ delegate WRITE setDelegate NOTIFY delegateChanged)

public:
    explicit CssRepeater(QQuickItem *parent = nullptr);
    ~CssRepeater() override;

    QVariant model() const { return m_model; }
    void setModel(const QVariant &v);

    QQmlComponent *delegate() const { return m_delegate; }
    void setDelegate(QQmlComponent *c);

signals:
    void modelChanged();
    void delegateChanged();

protected:
    void componentComplete() override;

private:
    struct Row {
        QVariant data;
        QPointer<QQuickItem> item;
        QPointer<QQmlContext> context;
    };

    void rebuild();          // diff m_rows against the new model
    void destroyRow(Row &row);
    void restack();          // enforce model order among the holder's children
    void notifyLayout();

    QVariant m_model;
    QQmlComponent *m_delegate = nullptr;
    QVector<Row> m_rows;
};
