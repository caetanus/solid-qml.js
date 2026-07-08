#pragma once

#include "qmlcss/cssfill.h"
#include "qmlcss/cssimage.h"
#include "qmlcss/cssrect.h"
#include "qmlcss/csstext.h"

// The primitive components — ports of Div.qml / Text.qml / Image.qml (one cached type serves
// every <div>/<text>/<img>; the transpiler overrides cssPrimitive for non-default tags).
namespace SolidWidgets {

// Div — the block primitive: a CssRect whose primitive defaults to "div".
class Div : public QmlCss::CssRect {
    Q_OBJECT

public:
    explicit Div(QQuickItem *parent = nullptr)
        : QmlCss::CssRect(parent)
    {
        setCssPrimitive(QStringLiteral("div"));
    }
};

// Text — the text-run primitive: a CssText whose primitive defaults to "text".
class Text : public QmlCss::CssText {
    Q_OBJECT

public:
    explicit Text(QQuickItem *parent = nullptr)
        : QmlCss::CssText(parent)
    {
        setCssPrimitive(QStringLiteral("text"));
    }
};

// Image — the <img> component: CssImage (object-fit + rounded clip) with the `src` slot the
// transpiler binds (CssImage.source is a QUrl; the string coerces).
class Image : public QmlCss::CssImage {
    Q_OBJECT
    Q_PROPERTY(QUrl src READ src WRITE setSrc NOTIFY srcChanged)

public:
    using QmlCss::CssImage::CssImage;

    QUrl src() const { return m_src; }
    void setSrc(const QUrl &v)
    {
        if (m_src == v)
            return;
        m_src = v;
        setSource(v);
        emit srcChanged();
    }

signals:
    void srcChanged();

private:
    QUrl m_src;
};

// Fieldset — the <fieldset> box: plain CssFill, primitive "fieldset" (no Templates chrome; the
// border/box look is author CSS). The transpiler hoists the <legend> to the front of the children.
class Fieldset : public QmlCss::CssFill {
    Q_OBJECT

public:
    explicit Fieldset(QQuickItem *parent = nullptr)
        : QmlCss::CssFill(parent)
    {
        setCssPrimitive(QStringLiteral("fieldset"));
    }
};

// StackView — phase-1 contract: children are pages, only child[current] is visible (the
// transpiler bakes per-page visible guards; an invisible child is out of flow).
class StackView : public QmlCss::CssRect {
    Q_OBJECT

public:
    explicit StackView(QQuickItem *parent = nullptr)
        : QmlCss::CssRect(parent)
    {
        setCssPrimitive(QStringLiteral("stack"));
    }
};

} // namespace SolidWidgets
