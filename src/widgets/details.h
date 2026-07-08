#pragma once

#include "qmlcss/cssfill.h"

#include <QPointer>
#include <QQmlListProperty>
#include <QVariant>

// Details (port of Details.qml) — the <details>/<summary> disclosure. The wrapper CssFill
// (cssPrimitive "details") owns the disclosure state: `__open` follows the `open` prop until the
// first user toggle (the QML init-binding semantics), then goes independent. The summary header
// row (marker glyph + hover/toggle MouseArea) and the body box ride as two snippets; the
// transpiler passes the summary's OWN children via `summaryContent` and the disclosure body via
// the default property — both route into the respective snippet's CONTENT slot (the engine's
// layout holder), buffered until the snippets compose.
namespace SolidWidgets {

class Details : public QmlCss::CssFill {
    Q_OBJECT
    Q_PROPERTY(bool open READ open WRITE setOpen NOTIFY openChanged)
    Q_PROPERTY(QVariant summaryClass READ summaryClass WRITE setSummaryClass NOTIFY summaryClassChanged)
    // The QML wrapper exposed `__open` — snippets and probes read the effective state here.
    Q_PROPERTY(bool __open READ isOpenNow NOTIFY effectiveOpenChanged)
    Q_PROPERTY(QQmlListProperty<QObject> summaryContent READ summaryContent CONSTANT)
    Q_PROPERTY(QQmlListProperty<QObject> content READ contentSlot CONSTANT)
    Q_CLASSINFO("DefaultProperty", "content")

public:
    explicit Details(QQuickItem *parent = nullptr);

    bool open() const { return m_open; }
    void setOpen(bool v);
    QVariant summaryClass() const { return m_summaryClass; }
    void setSummaryClass(const QVariant &v);
    bool isOpenNow() const { return m_effectiveOpen; }
    QQmlListProperty<QObject> summaryContent();
    QQmlListProperty<QObject> contentSlot();

    // The summary MouseArea's click — first use breaks the follow-`open` init semantics.
    Q_INVOKABLE void __toggle();

signals:
    void openChanged();
    void summaryClassChanged();
    void effectiveOpenChanged();

protected:
    void componentComplete() override;

private:
    struct Slot {
        QPointer<QQuickItem> box; // snippet box whose "content" receives the children
        QList<QObject *> pending;
    };

    static void summary_append(QQmlListProperty<QObject> *prop, QObject *obj);
    static void content_append(QQmlListProperty<QObject> *prop, QObject *obj);

    void appendTo(Slot &slot, QObject *obj);
    void setEffectiveOpen(bool v);
    void syncState();

    bool m_open = false;
    bool m_effectiveOpen = false;
    bool m_userToggled = false;
    QVariant m_summaryClass = QVariantList();
    Slot m_summary;
    Slot m_body;
};

} // namespace SolidWidgets
