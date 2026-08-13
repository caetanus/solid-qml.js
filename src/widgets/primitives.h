#pragma once

#include "qmlcss/cssfill.h"
#include "qmlcss/cssimage.h"
#include "qmlcss/cssrect.h"
#include "qmlcss/csstext.h"

#include "a11y.h"

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
// Accessibility: a text run declares itself to screen readers (StaticText, or Heading for h1…h6 —
// read off the same cssPrimitive the cascade styles by), with the rendered text as its name.
// Without this the CSS primitives are invisible to AT: unlike the widgets, they wrap no
// QtQuick.Templates control that would supply the semantics.
class Text : public QmlCss::CssText {
    Q_OBJECT

public:
    explicit Text(QQuickItem *parent = nullptr)
        : QmlCss::CssText(parent)
    {
        setCssPrimitive(QStringLiteral("text"));
    }

protected:
    // Attached accessibility needs a live QML context, so it is wired at completion (the
    // constructor runs before the item is associated with the engine).
    void componentComplete() override
    {
        QmlCss::CssText::componentComplete();
        const auto sync = [this] { A11y::describeText(this, cssPrimitive(), text()); };
        connect(this, &QmlCss::CssText::textChanged, this, sync);
        connect(this, &QmlCss::CssText::cssPrimitiveChanged, this, sync);
        sync();
    }
};

// Image — the <img> component: CssImage (object-fit + rounded clip) with the `src` slot the
// transpiler binds (CssImage.source is a QUrl; the string coerces).
class Image : public QmlCss::CssImage {
    Q_OBJECT
    Q_PROPERTY(QUrl src READ src WRITE setSrc NOTIFY srcChanged)
    // <img alt="…">: the accessible name. An image with no alt stays unnamed — the HTML contract
    // for decorative images, which AT then skips.
    Q_PROPERTY(QString alt READ alt WRITE setAlt NOTIFY altChanged)

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

    QString alt() const { return m_alt; }
    void setAlt(const QString &v)
    {
        if (m_alt == v)
            return;
        m_alt = v;
        if (isComponentComplete())
            A11y::describeImage(this, m_alt);
        emit altChanged();
    }

protected:
    void componentComplete() override
    {
        QmlCss::CssImage::componentComplete();
        A11y::describeImage(this, m_alt); // Graphic role; alt is the accessible name
    }

signals:
    void srcChanged();
    void altChanged();

private:
    QUrl m_src;
    QString m_alt;
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
