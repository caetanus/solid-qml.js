#include "test_qml_css.h"

#include "qmlcss/QMLCss.h"

#include <QtQml/qqml.h>

#include <QColor>
#include <QFont>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QSignalSpy>
#include <QTest>
#include <QTextOption>
#include <QUrl>

#include <cmath>

namespace {

// Locate the CssRect/CssFill contentHolder that owns `child` (the Item whose children are the
// layout participants): it is the child's immediate parent item. The layout root is one level up.
QQuickItem *contentHolderOf(QQuickItem *child)
{
    return child ? child->parentItem() : nullptr;
}

// The REAL QtQuick Text a CssText composes (it forwards color/font/wrap/align onto it).
QQuickItem *composedText(QObject *cssText)
{
    auto *item = qobject_cast<QQuickItem *>(cssText);
    if (!item)
        return nullptr;
    for (QQuickItem *k : item->childItems()) {
        if (QByteArray(k->metaObject()->className()).contains("QQuickText"))
            return k;
    }
    return nullptr;
}

} // namespace

void QmlCssTests::initTestCase()
{
    // Process-global, so every test's `import qmlcss` (and the CssFill shim that composes
    // CssRect) resolves. Same call consumers make.
    QmlCss::registerTypes();
}

void QmlCssTests::cssRectLoadsAndRestyles()
    {
        CssTheme theme;
        theme.loadFromString(QStringLiteral("#box { background-color: #112233; border-radius: 7px; }"));

        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);

        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick
            import qmlcss

            CssRect {
                width: 24
                height: 24
                cssId: "box"
            }
        )", QUrl());

        QScopedPointer<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));

        const QVariantMap initial = object->property("style").toMap();
        QCOMPARE(initial.value(QStringLiteral("background-color")).toString(), QStringLiteral("#112233"));
        QCOMPARE(initial.value(QStringLiteral("border-radius")).toString(), QStringLiteral("7px"));

        theme.loadFromString(QStringLiteral("#box { background-color: #445566; }"));
        const QVariantMap reloaded = object->property("style").toMap();
        QCOMPARE(reloaded.value(QStringLiteral("background-color")).toString(), QStringLiteral("#445566"));
}

void QmlCssTests::cssTextUsesStandaloneDefaults()
    {
        CssTheme theme;
        theme.loadFromString(QStringLiteral("#label { color: rgba(10, 20, 30, 0.5); font-size: 18px; }"));

        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);

        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick
            import qmlcss

            CssText {
                cssId: "label"
                text: "Label"
            }
        )", QUrl());

        QScopedPointer<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));

        QCOMPARE(object->property("style").toMap().value(QStringLiteral("font-size")).toString(),
                 QStringLiteral("18px"));
        // color is forwarded onto the composed Text.
        QQuickItem *label = composedText(object.data());
        QVERIFY2(label, "CssText did not compose a Text");
        const QColor color = label->property("color").value<QColor>();
        QCOMPARE(color.red(), 10);
        QCOMPARE(color.green(), 20);
        QCOMPARE(color.blue(), 30);
}

void QmlCssTests::cssItemAppliesToParent()
    {
        CssTheme theme;
        theme.loadFromString(QStringLiteral("#plain { background-color: #abcdef; border-radius: 5px; }"));

        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);

        QQmlComponent component(&engine);
        // `import qmlcss` proves the C++ registration (not the removed qrc QML file).
        component.setData(R"(
            import QtQuick
            import qmlcss

            Rectangle {
                width: 24
                height: 24

                CssItem {
                    cssId: "plain"
                }
            }
        )", QUrl());

        QScopedPointer<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));

        const QColor color = object->property("color").value<QColor>();
        QCOMPARE(color, QColor(QStringLiteral("#abcdef")));
        QCOMPARE(object->property("radius").toReal(), 5.0);
}

// --- Layout (geometry) tests ----------------------------------------------------------------
//
// These instantiate a CssRect container with CssRect children, register BOTH the CssTheme (as
// `cssTheme`) and a CssLayoutEngine (as `cssLayout`), then drive the box model synchronously via
// CssLayoutEngine::layout(root, contentHolder) and read back each child's assigned geometry.
// Children set `cssPrimitive: ""` so the theme registry never overwrites their explicit `style`.

void QmlCssTests::layoutFlexColumnStacks()
{
    CssTheme theme;
    CssLayoutEngine layoutEngine(&theme);

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);

    // A flex-column whose content (3×30 = 90) exceeds the container's 50px main size. The items must
    // STACK at their natural height and overflow — NOT shrink to fit. This guards the regression that
    // a default flex-shrink:1 introduced (auto-sized columns collapsing their content) which the
    // earlier unit tests missed — the reason "tests must also show in the app".
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss

        CssRect {
            cssPrimitive: ""
            width: 100; height: 50
            style: ({ "display": "flex", "flex-direction": "column", "gap": "0px" })

            CssRect { objectName: "a"; cssPrimitive: ""; style: ({ "height": "30px" }) }
            CssRect { objectName: "b"; cssPrimitive: ""; style: ({ "height": "30px" }) }
            CssRect { objectName: "c"; cssPrimitive: ""; style: ({ "height": "30px" }) }
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *root = qobject_cast<QQuickItem *>(object.data());
    QVERIFY(root);

    auto *a = root->findChild<QQuickItem *>(QStringLiteral("a"));
    auto *b = root->findChild<QQuickItem *>(QStringLiteral("b"));
    auto *c = root->findChild<QQuickItem *>(QStringLiteral("c"));
    QVERIFY(a && b && c);

    layoutEngine.layout(root, contentHolderOf(a));

    // Each keeps its 30px height (no shrink), stacked down the column: a@0, b@30, c@60 (overflowing).
    QVERIFY2(std::abs(a->height() - 30.0) < 0.5, qPrintable(QString::number(a->height())));
    QVERIFY2(std::abs(b->height() - 30.0) < 0.5, qPrintable(QString::number(b->height())));
    QVERIFY2(std::abs(c->height() - 30.0) < 0.5, qPrintable(QString::number(c->height())));
    QVERIFY2(std::abs(a->y() - 0.0) < 0.5, qPrintable(QString::number(a->y())));
    QVERIFY2(std::abs(b->y() - 30.0) < 0.5, qPrintable(QString::number(b->y())));
    QVERIFY2(std::abs(c->y() - 60.0) < 0.5, qPrintable(QString::number(c->y())));
}

void QmlCssTests::layoutClampsMaxAndMinWidth()
{
    CssTheme theme;
    CssLayoutEngine layoutEngine(&theme);

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);

    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss

        CssRect {
            cssPrimitive: ""
            width: 300; height: 60
            style: ({ "display": "flex", "flex-direction": "row", "gap": "0px" })

            CssRect {
                objectName: "capped"; cssPrimitive: ""
                style: ({ "width": "200px", "max-width": "50px" })
            }
            CssRect {
                objectName: "floored"; cssPrimitive: ""
                style: ({ "width": "20px", "min-width": "80px" })
            }
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *root = qobject_cast<QQuickItem *>(object.data());
    QVERIFY(root);

    auto *capped = root->findChild<QQuickItem *>(QStringLiteral("capped"));
    auto *floored = root->findChild<QQuickItem *>(QStringLiteral("floored"));
    QVERIFY(capped && floored);

    layoutEngine.layout(root, contentHolderOf(capped));

    // max-width clamps the 200px request down to 50; min-width floors the 20px request up to 80.
    QVERIFY2(std::abs(capped->width() - 50.0) < 0.5, qPrintable(QString::number(capped->width())));
    QVERIFY2(std::abs(floored->width() - 80.0) < 0.5, qPrintable(QString::number(floored->width())));
    // The floored item follows the capped one: x = 50 (capped's clamped width, gap 0).
    QVERIFY2(std::abs(floored->x() - 50.0) < 0.5, qPrintable(QString::number(floored->x())));
}

void QmlCssTests::layoutHonoursFlexOrder()
{
    CssTheme theme;
    CssLayoutEngine layoutEngine(&theme);

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);

    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss

        CssRect {
            cssPrimitive: ""
            width: 300; height: 40
            style: ({ "display": "flex", "flex-direction": "row", "gap": "0px" })

            CssRect { objectName: "a"; cssPrimitive: ""; style: ({ "width": "30px", "order": "2" }) }
            CssRect { objectName: "b"; cssPrimitive: ""; style: ({ "width": "30px", "order": "0" }) }
            CssRect { objectName: "c"; cssPrimitive: ""; style: ({ "width": "30px", "order": "1" }) }
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *root = qobject_cast<QQuickItem *>(object.data());
    QVERIFY(root);

    auto *a = root->findChild<QQuickItem *>(QStringLiteral("a"));
    auto *b = root->findChild<QQuickItem *>(QStringLiteral("b"));
    auto *c = root->findChild<QQuickItem *>(QStringLiteral("c"));
    QVERIFY(a && b && c);

    layoutEngine.layout(root, contentHolderOf(a));

    // Visual order follows ascending `order`: b(0) @0, c(1) @30, a(2) @60.
    QVERIFY2(std::abs(b->x() - 0.0) < 0.5, qPrintable(QString::number(b->x())));
    QVERIFY2(std::abs(c->x() - 30.0) < 0.5, qPrintable(QString::number(c->x())));
    QVERIFY2(std::abs(a->x() - 60.0) < 0.5, qPrintable(QString::number(a->x())));
    QVERIFY(b->x() < c->x());
    QVERIFY(c->x() < a->x());
}

void QmlCssTests::layoutAppliesFlexBasis()
{
    CssTheme theme;
    CssLayoutEngine layoutEngine(&theme);

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);

    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss

        CssRect {
            cssPrimitive: ""
            width: 400; height: 40
            style: ({ "display": "flex", "flex-direction": "row", "gap": "0px" })

            CssRect { objectName: "a"; cssPrimitive: ""; style: ({ "flex-basis": "40px" }) }
            CssRect { objectName: "b"; cssPrimitive: ""; style: ({ "flex-basis": "80px" }) }
            CssRect { objectName: "c"; cssPrimitive: ""; style: ({ "flex-basis": "120px" }) }
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *root = qobject_cast<QQuickItem *>(object.data());
    QVERIFY(root);

    auto *a = root->findChild<QQuickItem *>(QStringLiteral("a"));
    auto *b = root->findChild<QQuickItem *>(QStringLiteral("b"));
    auto *c = root->findChild<QQuickItem *>(QStringLiteral("c"));
    QVERIFY(a && b && c);

    layoutEngine.layout(root, contentHolderOf(a));

    // Each item's base main-axis size is its flex-basis (no flex-grow, so no free-space growth).
    QVERIFY2(std::abs(a->width() - 40.0) < 0.5, qPrintable(QString::number(a->width())));
    QVERIFY2(std::abs(b->width() - 80.0) < 0.5, qPrintable(QString::number(b->width())));
    QVERIFY2(std::abs(c->width() - 120.0) < 0.5, qPrintable(QString::number(c->width())));
    // Laid out left-to-right, so x offsets accumulate the bases.
    QVERIFY2(std::abs(a->x() - 0.0) < 0.5, qPrintable(QString::number(a->x())));
    QVERIFY2(std::abs(b->x() - 40.0) < 0.5, qPrintable(QString::number(b->x())));
    QVERIFY2(std::abs(c->x() - 120.0) < 0.5, qPrintable(QString::number(c->x())));
}

void QmlCssTests::layoutVisibilityHiddenKeepsSpace()
{
    CssTheme theme;
    CssLayoutEngine layoutEngine(&theme);

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);

    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss

        CssRect {
            cssPrimitive: ""
            width: 300; height: 60
            style: ({ "display": "flex", "flex-direction": "row", "gap": "0px" })

            CssRect {
                objectName: "ghost"; cssPrimitive: ""
                style: ({ "width": "50px", "visibility": "hidden" })
            }
            CssRect { objectName: "sibling"; cssPrimitive: ""; style: ({ "width": "30px" }) }
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *root = qobject_cast<QQuickItem *>(object.data());
    QVERIFY(root);

    auto *ghost = root->findChild<QQuickItem *>(QStringLiteral("ghost"));
    auto *sibling = root->findChild<QQuickItem *>(QStringLiteral("sibling"));
    QVERIFY(ghost && sibling);

    layoutEngine.layout(root, contentHolderOf(ghost));

    // visibility:hidden → painted invisible and inert, but STILL occupies its box in the flow.
    QCOMPARE(ghost->property("opacity").toReal(), 0.0);
    QCOMPARE(ghost->property("enabled").toBool(), false);
    QVERIFY2(ghost->width() > 0.5, qPrintable(QString::number(ghost->width())));
    // The sibling does NOT recede into the hidden item's space: it starts after the 50px box.
    QVERIFY2(std::abs(sibling->x() - 50.0) < 0.5, qPrintable(QString::number(sibling->x())));
}

void QmlCssTests::layoutGridTemplateAreas()
{
    CssTheme theme;
    CssLayoutEngine layoutEngine(&theme);

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);

    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss

        CssRect {
            cssPrimitive: ""
            width: 300; height: 200
            style: ({
                "display": "grid",
                "grid-template-columns": "100px 100px",
                "grid-template-rows": "40px 40px",
                "grid-template-areas": '"head head" "side main"',
                "gap": "10px"
            })

            CssRect { objectName: "head"; cssPrimitive: ""; style: ({ "grid-area": "head" }) }
            CssRect { objectName: "side"; cssPrimitive: ""; style: ({ "grid-area": "side" }) }
            CssRect { objectName: "main"; cssPrimitive: ""; style: ({ "grid-area": "main" }) }
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *root = qobject_cast<QQuickItem *>(object.data());
    QVERIFY(root);

    auto *head = root->findChild<QQuickItem *>(QStringLiteral("head"));
    auto *side = root->findChild<QQuickItem *>(QStringLiteral("side"));
    auto *main = root->findChild<QQuickItem *>(QStringLiteral("main"));
    QVERIFY(head && side && main);

    layoutEngine.layout(root, contentHolderOf(head));

    // `head` spans both columns: width = 100 + 10(gap) + 100 = 210, anchored at (0, 0).
    QVERIFY2(std::abs(head->width() - 210.0) < 0.5, qPrintable(QString::number(head->width())));
    QVERIFY2(std::abs(head->x() - 0.0) < 0.5, qPrintable(QString::number(head->x())));
    QVERIFY2(std::abs(head->y() - 0.0) < 0.5, qPrintable(QString::number(head->y())));
    QVERIFY2(std::abs(head->height() - 40.0) < 0.5, qPrintable(QString::number(head->height())));

    // Second row (y = 40 + 10 = 50): side @ col0, main @ col1 (x = 100 + 10 = 110).
    QVERIFY2(std::abs(side->x() - 0.0) < 0.5, qPrintable(QString::number(side->x())));
    QVERIFY2(std::abs(side->y() - 50.0) < 0.5, qPrintable(QString::number(side->y())));
    QVERIFY2(std::abs(side->width() - 100.0) < 0.5, qPrintable(QString::number(side->width())));

    QVERIFY2(std::abs(main->x() - 110.0) < 0.5, qPrintable(QString::number(main->x())));
    QVERIFY2(std::abs(main->y() - 50.0) < 0.5, qPrintable(QString::number(main->y())));
    QVERIFY2(std::abs(main->width() - 100.0) < 0.5, qPrintable(QString::number(main->width())));
}

void QmlCssTests::layoutFlexShrinkOverflow()
{
    CssTheme theme;
    CssLayoutEngine layoutEngine(&theme);

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);

    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss

        CssRect {
            cssPrimitive: ""
            width: 100; height: 40
            style: ({ "display": "flex", "flex-direction": "row", "gap": "0px" })

            CssRect { objectName: "a"; cssPrimitive: ""; style: ({ "width": "60px", "flex-shrink": "1" }) }
            CssRect { objectName: "b"; cssPrimitive: ""; style: ({ "width": "60px", "flex-shrink": "1" }) }
            CssRect { objectName: "c"; cssPrimitive: ""; style: ({ "width": "60px", "flex-shrink": "1" }) }
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *root = qobject_cast<QQuickItem *>(object.data());
    QVERIFY(root);

    auto *a = root->findChild<QQuickItem *>(QStringLiteral("a"));
    auto *b = root->findChild<QQuickItem *>(QStringLiteral("b"));
    auto *c = root->findChild<QQuickItem *>(QStringLiteral("c"));
    QVERIFY(a && b && c);

    layoutEngine.layout(root, contentHolderOf(a));

    // 3×60 = 180 overflows the 100 container; with default flex-shrink:1 each item deflates by the
    // same scaled factor to 100/3 ≈ 33.33, so together they exactly fit the container.
    QVERIFY2(a->width() < 60.0, qPrintable(QString::number(a->width())));
    QVERIFY2(std::abs(a->width() - 100.0 / 3.0) < 0.5, qPrintable(QString::number(a->width())));
    const double sum = a->width() + b->width() + c->width();
    QVERIFY2(std::abs(sum - 100.0) < 0.5, qPrintable(QString::number(sum)));
    // Positions accumulate the shrunk widths (no gaps): a@0, b@~33.33, c@~66.67.
    QVERIFY2(std::abs(b->x() - a->width()) < 0.5, qPrintable(QString::number(b->x())));
    QVERIFY2(std::abs(c->x() - (a->width() + b->width())) < 0.5, qPrintable(QString::number(c->x())));
}

void QmlCssTests::layoutAlignContentCenter()
{
    CssTheme theme;
    CssLayoutEngine layoutEngine(&theme);

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);

    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss

        CssRect {
            cssPrimitive: ""
            width: 100; height: 200
            style: ({
                "display": "flex", "flex-direction": "row", "flex-wrap": "wrap",
                "gap": "0px", "align-content": "center"
            })

            CssRect { objectName: "a"; cssPrimitive: ""; style: ({ "width": "60px", "height": "20px" }) }
            CssRect { objectName: "b"; cssPrimitive: ""; style: ({ "width": "60px", "height": "20px" }) }
            CssRect { objectName: "c"; cssPrimitive: ""; style: ({ "width": "60px", "height": "20px" }) }
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *root = qobject_cast<QQuickItem *>(object.data());
    QVERIFY(root);

    auto *a = root->findChild<QQuickItem *>(QStringLiteral("a"));
    auto *b = root->findChild<QQuickItem *>(QStringLiteral("b"));
    auto *c = root->findChild<QQuickItem *>(QStringLiteral("c"));
    QVERIFY(a && b && c);

    layoutEngine.layout(root, contentHolderOf(a));

    // Each 60px item overflows the 100px row → three wrapped lines of 20px height each (total 60).
    // align-content: center centres the 3-line block in the 200px cross box: free = 200-60 = 140,
    // so the first line starts at 70.
    QVERIFY2(std::abs(a->y() - 70.0) < 0.5, qPrintable(QString::number(a->y())));
    QVERIFY2(a->y() > 0.5, qPrintable(QString::number(a->y())));
    // Lines stack downward by their natural height (20 each): b@90, c@110.
    QVERIFY2(std::abs(b->y() - 90.0) < 0.5, qPrintable(QString::number(b->y())));
    QVERIFY2(std::abs(c->y() - 110.0) < 0.5, qPrintable(QString::number(c->y())));
}

void QmlCssTests::layoutBoxSizingBorderBox()
{
    CssTheme theme;
    CssLayoutEngine layoutEngine(&theme);

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);

    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss

        CssRect {
            cssPrimitive: ""
            width: 400; height: 60
            style: ({ "display": "flex", "flex-direction": "row", "gap": "0px" })

            CssRect {
                objectName: "border"; cssPrimitive: ""
                style: ({ "width": "100px", "padding": "10px", "box-sizing": "border-box" })
            }
            CssRect {
                objectName: "content"; cssPrimitive: ""
                style: ({ "width": "100px", "padding": "10px", "box-sizing": "content-box" })
            }
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *root = qobject_cast<QQuickItem *>(object.data());
    QVERIFY(root);

    auto *borderBox = root->findChild<QQuickItem *>(QStringLiteral("border"));
    auto *contentBox = root->findChild<QQuickItem *>(QStringLiteral("content"));
    QVERIFY(borderBox && contentBox);

    layoutEngine.layout(root, contentHolderOf(borderBox));

    // border-box: the 100px width INCLUDES the 2×10 padding, so the laid-out box stays 100.
    QVERIFY2(std::abs(borderBox->width() - 100.0) < 0.5, qPrintable(QString::number(borderBox->width())));
    // content-box (CSS default): the 100px is the content box, so the border box = 100 + 2×10 = 120.
    QVERIFY2(std::abs(contentBox->width() - 120.0) < 0.5, qPrintable(QString::number(contentBox->width())));
    // The content-box item follows the 100px border-box item (gap 0).
    QVERIFY2(std::abs(contentBox->x() - 100.0) < 0.5, qPrintable(QString::number(contentBox->x())));
}

void QmlCssTests::buttonLabelInheritsColor()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral("button { color: #ffffff; }"));

    CssLayoutEngine layoutEngine(&theme);

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);

    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss

        CssFill {
            cssPrimitive: "button"
            width: 100; height: 40

            CssText { objectName: "label"; text: "Click" }
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *root = qobject_cast<QQuickItem *>(object.data());
    QVERIFY(root);

    // The button's resolved style carries the white foreground colour.
    QCOMPARE(root->property("inheritedColor").toString(), QStringLiteral("#ffffff"));

    auto *label = root->findChild<QQuickItem *>(QStringLiteral("label"));
    QVERIFY(label);
    // The label has no colour of its own, so it inherits white from the button parent —
    // forwarded onto its composed Text.
    QQuickItem *labelText = composedText(label);
    QVERIFY2(labelText, "CssText did not compose a Text");
    const QColor color = labelText->property("color").value<QColor>();
    QCOMPARE(color, QColor(255, 255, 255));
}

// --- New CSS property mapping tests ---------------------------------------------------------

void QmlCssTests::cssRectZIndex()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral("#zbox { z-index: 5; }"));

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);

    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss

        CssRect {
            width: 24; height: 24
            cssId: "zbox"
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));

    QCOMPARE(object->property("z").toReal(), 5.0);
}

void QmlCssTests::cssRectOverflowHidden()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral("#clipped { overflow: hidden; }"));

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);

    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss

        CssRect {
            width: 24; height: 24
            cssId: "clipped"
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));

    QCOMPARE(object->property("clip").toBool(), true);
}

void QmlCssTests::cssTextDecorationUnderline()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral("#uline { text-decoration: underline; }"));

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);

    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss

        CssText {
            cssId: "uline"
            text: "Hello"
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));

    QQuickItem *label = composedText(object.data());
    QVERIFY2(label, "CssText did not compose a Text");
    const QFont font = label->property("font").value<QFont>();
    QCOMPARE(font.underline(), true);
    QCOMPARE(font.strikeOut(), false);
}

void QmlCssTests::cssTextWhiteSpaceNowrap()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral("#nowrap { white-space: nowrap; }"));

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);

    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss

        CssText {
            cssId: "nowrap"
            text: "Hello world"
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));

    QQuickItem *label = composedText(object.data());
    QVERIFY2(label, "CssText did not compose a Text");
    // Text.NoWrap maps to QTextOption::NoWrap == 0
    QCOMPARE(label->property("wrapMode").toInt(), static_cast<int>(QTextOption::NoWrap));
}

void QmlCssTests::cssTextOverflowEllipsis()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral("#ellipsis { text-overflow: ellipsis; white-space: nowrap; }"));

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);

    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss

        CssText {
            cssId: "ellipsis"
            text: "A very long piece of text that should be elided"
            width: 50
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));

    QQuickItem *label = composedText(object.data());
    QVERIFY2(label, "CssText did not compose a Text");
    // Text.ElideRight maps to Qt::ElideRight == 1
    QCOMPARE(label->property("elide").toInt(), static_cast<int>(Qt::ElideRight));
    // The composed Text filled the assigned 50px width (so it actually elides).
    QCOMPARE(label->width(), 50.0);
}

// --- C++ CssRect (composition translation of CssRect.qml) ------------------------------------
//
// Proves the C++ type registers via `import qmlcss`, resolves its style through cssTheme.loadCss,
// composes a REAL QtQuick Shape for the box (background / border / radius pushed onto it), AND is
// a layout container: a declared child lands in the inner contentHolder and is laid out by the
// C++ box model.
void QmlCssTests::cssRectCppComposesShapeAndContains()
{
    CssTheme theme;
    // A gradient forces the SHAPE path of the Shape x Rectangle policy (a plain solid box
    // now takes the cheap Rectangle path — covered by rectanglePolicyFastPathAndSwap).
    theme.loadFromString(QStringLiteral(
        "#box { background: linear-gradient(180deg, #2244aa, #112255); "
        "border: 2px solid #ffcc00; border-radius: 8px; "
        "display: flex; flex-direction: row; gap: 0px; }"));

    CssLayoutEngine layout(&theme);

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layout);

    QQmlComponent component(&engine);
    // `import qmlcss` proves the C++ registration (not a qrc QML file).
    component.setData(R"(
        import QtQuick
        import qmlcss

        CssRect {
            width: 120
            height: 40
            cssId: "box"

            CssRect { objectName: "kid"; cssPrimitive: ""; style: ({ "width": "40px", "height": "20px" }) }
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *rect = qobject_cast<CssRect *>(object.data());
    QVERIFY(rect);

    // loadCss pushed the resolved style.
    const QVariantMap style = rect->property("style").toMap();
    QCOMPARE(style.value(QStringLiteral("background")).toString(),
             QStringLiteral("linear-gradient(180deg, #2244aa, #112255)"));
    QCOMPARE(style.value(QStringLiteral("border-radius")).toString(), QStringLiteral("8px"));

    // The composed render subtree root carries the resolved CSS values (bg/border/radius) that
    // drive the real Shape fill — identified as the CssRect child exposing `radiusStr`.
    QQuickItem *renderRoot = nullptr;
    for (QQuickItem *k : rect->childItems()) {
        if (k->property("radiusStr").isValid()) {
            renderRoot = k;
            break;
        }
    }
    QVERIFY2(renderRoot, "composed render subtree missing");
    QCOMPARE(renderRoot->property("radiusStr").toString(), QStringLiteral("8px"));
    QCOMPARE(renderRoot->property("borderWidth").toReal(), 2.0);
    QCOMPARE(renderRoot->property("borderColorOpaque").value<QColor>(), QColor(QStringLiteral("#ffcc00")));
    // Gradient path (the policy sends solid boxes to the Rectangle fast path instead).
    QVERIFY(renderRoot->property("hasGradient").toBool());
    const QVariantMap gradient = renderRoot->property("gradient").toMap();
    QCOMPARE(gradient.value(QStringLiteral("stops")).toList().size(), 2);

    // The box fill is a genuine QtQuick Shape (NOT a Rectangle), somewhere under the render root.
    bool hasShape = false;
    bool hasRectangle = false;
    for (QQuickItem *d : renderRoot->findChildren<QQuickItem *>()) {
        const QByteArray cls(d->metaObject()->className());
        if (cls.contains("QQuickShape") && !cls.contains("QQuickShapePath"))
            hasShape = true;
        if (cls.contains("QQuickRectangle"))
            hasRectangle = true;
    }
    QVERIFY2(hasShape, "no composed QtQuick Shape fill");
    QVERIFY2(!hasRectangle, "composition used a Rectangle (owner's rule: Shape only)");

    // Container: the declared child lands in the inner contentHolder (child.parent.parent == rect),
    // and the C++ box model laid it out at its 40px width.
    auto *kid = rect->findChild<QQuickItem *>(QStringLiteral("kid"));
    QVERIFY(kid);
    QQuickItem *holder = kid->parentItem();
    QVERIFY(holder);
    QCOMPARE(holder->parentItem(), rect);
    QVERIFY2(std::abs(kid->width() - 40.0) < 0.5, qPrintable(QString::number(kid->width())));
}

// --- C++ CssHr (composition translation of CssHr.qml) ----------------------------------------
//
// Proves the C++ type registers via the classic API, instantiates from QML as `qmlcss.CssHr`,
// resolves its top border through cssTheme.borderSide, and — crucially — that the child it
// composes is the REAL QtQuick Shape (ShapePath + PathRectangle), carrying the resolved
// colour/height full-width (not a repaint, and not a Rectangle).

void QmlCssTests::cssHrCppComposesRealShape()
{
    // Classic one-line registration — no qmltyperegistrar / QML_ELEMENT machinery.
    qmlRegisterType<CssHr>("qmlcss", 1, 0, "CssHr");

    CssTheme theme;
    theme.loadFromString(QStringLiteral(
        "#rule { border-top: 3px solid #ff8800; }"));

    QQmlEngine engine;
    CssLayoutEngine layout(&theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layout);

    QQmlComponent component(&engine);
    // `import qmlcss` proves the C++ registration (not the qrc QML file).
    component.setData(R"(
        import QtQuick
        import qmlcss

        CssHr {
            width: 120
            cssId: "rule"
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));

    auto *hr = qobject_cast<CssHr *>(object.data());
    QVERIFY(hr);

    // loadCss pushed the resolved style; the `line` binding read the top border off it.
    const QVariantMap style = hr->property("style").toMap();
    QCOMPARE(style.value(QStringLiteral("border-top")).toString(), QStringLiteral("3px solid #ff8800"));

    const QVariantMap line = hr->property("line").toMap();
    QCOMPARE(line.value(QStringLiteral("width")).toReal(), 3.0);
    QCOMPARE(line.value(QStringLiteral("color")).value<QColor>(), QColor(QStringLiteral("#ff8800")));
    QCOMPARE(line.value(QStringLiteral("visible")).toBool(), true);

    // implicitHeight tracks the line width (max(1, width)).
    QCOMPARE(hr->implicitHeight(), 3.0);

    // The composed child must be a genuine QtQuick Shape (NOT a Rectangle), styled with the line
    // colour/height and pinned full-width (the C++ anchor equivalent).
    const QList<QQuickItem *> kids = hr->childItems();
    QCOMPARE(kids.size(), 1);
    QQuickItem *shape = kids.first();
    QVERIFY(shape);
    const QByteArray cls(shape->metaObject()->className());
    QVERIFY2(cls.contains("QQuickShape"), cls);
    QVERIFY2(!cls.contains("QQuickRectangle"), cls);
    // fillColor / rectHeight are the aliases the composed Shape exposes onto its ShapePath /
    // PathRectangle — the real fill colour and rounded-rect height.
    QCOMPARE(shape->property("fillColor").value<QColor>(), QColor(QStringLiteral("#ff8800")));
    QCOMPARE(shape->property("rectHeight").toReal(), 3.0);
    QCOMPARE(shape->property("rectWidth").toReal(), 120.0);
    QCOMPARE(shape->height(), 3.0);
    QCOMPARE(shape->property("visible").toBool(), true);
    QCOMPARE(shape->width(), 120.0); // followed the parent's width
    QCOMPARE(shape->x(), 0.0);
    QCOMPARE(shape->y(), 0.0);

    // The ShapePath really carries a PathRectangle child (proves it is a genuine vector fill).
    QObject *shapePath = shape->findChild<QObject *>();
    QVERIFY(shapePath);
    QVERIFY2(QByteArray(shapePath->metaObject()->className()).contains("QQuickShapePath"),
             shapePath->metaObject()->className());
}

// --- C++ CssImage (composition translation of CssImage.qml) ----------------------------------
//
// Helper: classify the composed children of a CssImage by their real Qt class names.
namespace {
struct ImageParts {
    QQuickItem *image = nullptr;  // QQuickImage
    QQuickItem *effect = nullptr; // QQuickMultiEffect
    QQuickItem *shape = nullptr;  // QQuickShape (rounded-rect mask)
};
ImageParts partsOf(QQuickItem *root)
{
    ImageParts p;
    const QList<QQuickItem *> kids = root->childItems();
    for (QQuickItem *k : kids) {
        const QByteArray cls(k->metaObject()->className());
        if (cls.contains("QQuickImage"))
            p.image = k;
        else if (cls.contains("QQuickMultiEffect"))
            p.effect = k;
        else if (cls.contains("QQuickShape") && !cls.contains("QQuickShapePath"))
            p.shape = k;
    }
    return p;
}
} // namespace

// Rounded case: a non-zero border-radius must compose a REAL Image + MultiEffect + Shape mask,
// with the effect masking (maskEnabled) and the Image hidden (drawn through the effect).
void QmlCssTests::cssImageCppComposesImageEffectAndMask()
{
    qmlRegisterType<CssImage>("qmlcss", 1, 0, "CssImage");

    CssTheme theme;
    theme.loadFromString(QStringLiteral(
        "#avatar { border-radius: 12px; object-fit: cover; }"));

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);

    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss

        CssImage {
            width: 100
            height: 100
            cssId: "avatar"
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *img = qobject_cast<CssImage *>(object.data());
    QVERIFY(img);

    // loadCss pushed the resolved style.
    const QVariantMap style = img->property("style").toMap();
    QCOMPARE(style.value(QStringLiteral("border-radius")).toString(), QStringLiteral("12px"));

    const ImageParts p = partsOf(img);
    QVERIFY2(p.image, "composed Image missing");
    QVERIFY2(p.effect, "composed MultiEffect missing");
    QVERIFY2(p.shape, "composed Shape mask missing");

    // object-fit: cover -> Image.PreserveAspectCrop (2).
    QCOMPARE(p.image->property("fillMode").toInt(), 2);
    // Rounded -> the Image is hidden (the MultiEffect draws it through the mask).
    QCOMPARE(p.image->isVisible(), false);

    // The MultiEffect masks, is visible, and points at the REAL composed Image / Shape.
    QCOMPARE(p.effect->property("maskEnabled").toBool(), true);
    QCOMPARE(p.effect->isVisible(), true);
    QCOMPARE(p.effect->property("source").value<QQuickItem *>(), p.image);
    QCOMPARE(p.effect->property("maskSource").value<QQuickItem *>(), p.shape);

    // The mask's rounded-rect radius follows the resolved border-radius and fills the item.
    QCOMPARE(p.shape->property("maskRadius").toReal(), 12.0);
    QCOMPARE(p.shape->property("maskWidth").toReal(), 100.0);
    QCOMPARE(p.shape->property("maskHeight").toReal(), 100.0);
}

// No-radius case: the Image is shown DIRECT, and the MultiEffect mask is disabled/hidden.
void QmlCssTests::cssImageCppNoRadiusShowsImageDirect()
{
    qmlRegisterType<CssImage>("qmlcss", 1, 0, "CssImage");

    CssTheme theme;
    theme.loadFromString(QStringLiteral(
        "#plain { object-fit: contain; }"));

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);

    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss

        CssImage {
            width: 80
            height: 40
            cssId: "plain"
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *img = qobject_cast<CssImage *>(object.data());
    QVERIFY(img);

    const ImageParts p = partsOf(img);
    QVERIFY2(p.image, "composed Image missing");
    QVERIFY2(p.effect, "composed MultiEffect missing");

    // object-fit: contain -> Image.PreserveAspectFit (1).
    QCOMPARE(p.image->property("fillMode").toInt(), 1);
    // No radius -> the Image draws directly, the effect is off.
    QCOMPARE(p.image->isVisible(), true);
    QCOMPARE(p.effect->isVisible(), false);
    QCOMPARE(p.effect->property("maskEnabled").toBool(), false);
}

// --- C++ CssText (composition translation of CssText.qml) ------------------------------------
//
// Proves CssText registers via `import qmlcss`, composes a REAL QtQuick Text (not a Rectangle,
// not a repaint), forwards the CSS-driven props (color/font/wrap/align) onto it, and inherits the
// container's colour/font/align through the CSS-inheritance chain.
void QmlCssTests::cssTextCppComposesRealText()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(
        "#box { color: #ff0000; font-size: 20px; text-align: center; } "
        "#label { white-space: nowrap; }"));

    CssLayoutEngine layout(&theme);

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layout);

    QQmlComponent component(&engine);
    // `import qmlcss` proves the C++ registration (not the removed qrc QML file).
    component.setData(R"(
        import QtQuick
        import qmlcss

        CssRect {
            width: 200; height: 40
            cssId: "box"

            CssText { objectName: "label"; cssId: "label"; text: "Hi" }
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *box = qobject_cast<QQuickItem *>(object.data());
    QVERIFY(box);

    auto *label = box->findChild<CssText *>(QStringLiteral("label"));
    QVERIFY(label);

    // loadCss pushed the label's own style (white-space).
    QCOMPARE(label->property("style").toMap().value(QStringLiteral("white-space")).toString(),
             QStringLiteral("nowrap"));

    // The composed child is a genuine QtQuick Text (NOT a Rectangle).
    const QList<QQuickItem *> kids = label->childItems();
    QQuickItem *text = nullptr;
    for (QQuickItem *k : kids) {
        const QByteArray cls(k->metaObject()->className());
        QVERIFY2(!cls.contains("QQuickRectangle"), cls);
        if (cls.contains("QQuickText"))
            text = k;
    }
    QVERIFY2(text, "CssText did not compose a real Text");

    // Forwarded content + literal text format.
    QCOMPARE(text->property("text").toString(), QStringLiteral("Hi"));
    QCOMPARE(text->property("textFormat").toInt(), 0); // Text.PlainText

    // Inherited from the container: colour (red), font-size (20px -> 15pt) and centre alignment.
    QCOMPARE(text->property("color").value<QColor>(), QColor(255, 0, 0));
    const QFont font = text->property("font").value<QFont>();
    QVERIFY2(std::abs(font.pointSizeF() - 15.0) < 0.5, qPrintable(QString::number(font.pointSizeF())));
    QCOMPARE(text->property("horizontalAlignment").toInt(), static_cast<int>(Qt::AlignHCenter));

    // The label's own white-space: nowrap forwarded onto the Text.
    QCOMPARE(text->property("wrapMode").toInt(), static_cast<int>(QTextOption::NoWrap));
}

// --- C++ CssFill (composition translation of CssFill.qml) ------------------------------------
//
// Proves CssFill (registered via the classic API, instantiated as `qmlcss.CssFill`) composes,
// for a url() background: a REAL solid-colour Shape behind, a REAL QtQuick Image, and a REAL
// CssRect renderer (our own type, cssPrimitive "" and fill forced transparent so the image shows
// through while the border still frames) — AND that it hosts a declared child in its contentHolder,
// laid out by the C++ box model.
void QmlCssTests::cssFillCppComposesImageRectAndHostsChildren()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(
        "#panel { background-color: #204080; background-image: url(qrc:/nope.png); "
        "background-size: contain; border: 2px solid #ffcc00; "
        "display: flex; flex-direction: row; gap: 0px; }"));

    CssLayoutEngine layout(&theme);

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layout);

    QQmlComponent component(&engine);
    // `import qmlcss` proves the C++ registration (not the removed qrc QML file).
    component.setData(R"(
        import QtQuick
        import qmlcss

        CssFill {
            width: 120
            height: 60
            cssId: "panel"

            CssRect { objectName: "kid"; cssPrimitive: ""; style: ({ "width": "40px", "height": "20px" }) }
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *fill = qobject_cast<CssFill *>(object.data());
    QVERIFY(fill);

    // loadCss pushed the resolved style.
    const QVariantMap style = fill->property("style").toMap();
    QCOMPARE(style.value(QStringLiteral("background-color")).toString(), QStringLiteral("#204080"));
    QCOMPARE(style.value(QStringLiteral("background-image")).toString(), QStringLiteral("url(qrc:/nope.png)"));

    // Classify the composed layers by their real Qt class names.
    QQuickItem *image = nullptr;
    QQuickItem *rect = nullptr;
    QQuickItem *solidShape = nullptr;
    for (QQuickItem *k : fill->childItems()) {
        const QByteArray cls(k->metaObject()->className());
        if (cls.contains("QQuickImage"))
            image = k;
        else if (cls.contains("CssRect"))
            rect = k;
        else if (cls.contains("QQuickShape") && !cls.contains("QQuickShapePath"))
            solidShape = k;
    }
    QVERIFY2(image, "composed Image missing");
    QVERIFY2(rect, "composed CssRect renderer missing");
    QVERIFY2(solidShape, "composed solid-background Shape missing");

    // The Image carries the resolved url() source, is visible, and object-fit maps background-size:
    // contain -> Image.PreserveAspectFit (1).
    QCOMPARE(image->property("source").toUrl(), QUrl(QStringLiteral("qrc:/nope.png")));
    QCOMPARE(image->isVisible(), true);
    QCOMPARE(image->property("fillMode").toInt(), 1);

    // The solid Shape shows the background-color behind the image.
    QCOMPARE(solidShape->isVisible(), true);
    QCOMPARE(solidShape->property("fillColor").value<QColor>(), QColor(QStringLiteral("#204080")));

    // The CssRect renderer got the same style, and its fill is transparent (image present) while the
    // border is still resolved — proven through its own composed render root.
    QCOMPARE(rect->property("cssPrimitive").toString(), QString());
    QCOMPARE(rect->property("defaultColor").value<QColor>(), QColor(Qt::transparent));
    QQuickItem *renderRoot = nullptr;
    for (QQuickItem *k : rect->childItems()) {
        if (k->property("radiusStr").isValid()) {
            renderRoot = k;
            break;
        }
    }
    QVERIFY2(renderRoot, "CssRect render subtree missing");
    QCOMPARE(renderRoot->property("borderWidth").toReal(), 2.0);
    QCOMPARE(renderRoot->property("borderColorOpaque").value<QColor>(), QColor(QStringLiteral("#ffcc00")));

    // Container: the declared child lands in CssFill's inner contentHolder (child.parent.parent ==
    // fill), and the C++ box model laid it out at its 40px width — NOT inside the renderer CssRect.
    auto *kid = fill->findChild<QQuickItem *>(QStringLiteral("kid"));
    QVERIFY(kid);
    QQuickItem *holder = kid->parentItem();
    QVERIFY(holder);
    QCOMPARE(holder->parentItem(), fill);
    QVERIFY2(std::abs(kid->width() - 40.0) < 0.5, qPrintable(QString::number(kid->width())));
}

// --- C++ CssDropShadow (composition translation of CssDropShadow.qml) -----------------------
//
// Proves CssDropShadow registers via `import qmlcss`, composes a REAL QtQuick.Effects
// MultiEffect as its only child, and correctly drives the shadow* props (including the
// shadowBlur 0..1 normalisation over 32 px) from a QVariantMap shadow descriptor.
void QmlCssTests::cssDropShadowCppComposesMultiEffect()
{
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        CssDropShadow {
            width: 100; height: 40
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *ds = qobject_cast<CssDropShadow *>(object.data());
    QVERIFY(ds);

    // Initially: no shadow descriptor → hasShadow false.
    QCOMPARE(ds->hasShadow(), false);

    // Find the composed REAL MultiEffect child.
    QQuickItem *effect = nullptr;
    for (QQuickItem *k : ds->childItems()) {
        if (QByteArray(k->metaObject()->className()).contains("QQuickMultiEffect"))
            effect = k;
    }
    QVERIFY2(effect, "composed MultiEffect missing");
    QCOMPARE(effect->property("shadowEnabled").toBool(), false);

    // Set a shadow descriptor and verify it is pushed onto the MultiEffect.
    QVariantMap shadow;
    shadow[QStringLiteral("x")]     = 2.0;
    shadow[QStringLiteral("y")]     = 3.0;
    shadow[QStringLiteral("blur")]  = 16.0;         // blur/32 = 0.5
    shadow[QStringLiteral("color")] = QColor(QStringLiteral("#ff8800"));
    ds->setShadow(shadow);

    QCOMPARE(ds->hasShadow(), true);
    QCOMPARE(effect->property("shadowEnabled").toBool(), true);
    QCOMPARE(effect->property("shadowColor").value<QColor>(), QColor(QStringLiteral("#ff8800")));
    QCOMPARE(effect->property("shadowHorizontalOffset").toReal(), 2.0);
    QCOMPARE(effect->property("shadowVerticalOffset").toReal(), 3.0);
    // QML: shadowBlur = Math.min(1, 16 / 32) = 0.5
    QVERIFY2(std::abs(effect->property("shadowBlur").toReal() - 0.5) < 1e-6,
             qPrintable(QString::number(effect->property("shadowBlur").toReal())));

    // autoPaddingEnabled must be true (matches the original QML).
    QCOMPARE(effect->property("autoPaddingEnabled").toBool(), true);

    // Clearing the shadow (no color key) resets shadowEnabled.
    ds->setShadow({});
    QCOMPARE(ds->hasShadow(), false);
    QCOMPARE(effect->property("shadowEnabled").toBool(), false);
}

// --- C++ CssKeyframes (composition translation of CssKeyframes.qml) --------------------------
//
// Proves CssKeyframes registers via `import qmlcss`, composes a REAL QtQuick NumberAnimation,
// and correctly interpolates the keyframe value onto target[animatedProperty] when progress is
// set directly (testing _numAt without requiring the animation event loop to tick).
void QmlCssTests::cssKeyframesCppInterpolatesTarget()
{
    QQmlEngine engine;

    // A target QQuickItem whose `opacity` we'll animate.
    QQmlComponent targetComp(&engine);
    targetComp.setData("import QtQuick\nItem { width: 1; height: 1 }", QUrl());
    QScopedPointer<QObject> targetObj(targetComp.create());
    QVERIFY2(targetObj, qPrintable(targetComp.errorString()));
    auto *target = qobject_cast<QQuickItem *>(targetObj.data());
    QVERIFY(target);

    // Instantiate CssKeyframes from QML to exercise the full componentComplete path.
    QQmlComponent kfComp(&engine);
    kfComp.setData(R"(
        import QtQuick
        import qmlcss
        CssKeyframes {}
    )", QUrl());
    QScopedPointer<QObject> kfObj(kfComp.create());
    QVERIFY2(kfObj, qPrintable(kfComp.errorString()));

    // CssKeyframes is invisible and 0x0 (matching the QML original).
    auto *kf = qobject_cast<QQuickItem *>(kfObj.data());
    QVERIFY(kf);
    QCOMPARE(kf->isVisible(), false);
    QCOMPARE(kf->width(), 0.0);
    QCOMPARE(kf->height(), 0.0);

    // Build two keyframes: opacity 0->1.
    QVariantMap f0, f1, p0, p1;
    p0[QStringLiteral("opacity")] = QStringLiteral("0.0");
    p1[QStringLiteral("opacity")] = QStringLiteral("1.0");
    f0[QStringLiteral("offset")]     = 0.0;
    f0[QStringLiteral("properties")] = p0;
    f1[QStringLiteral("offset")]     = 1.0;
    f1[QStringLiteral("properties")] = p1;
    QVariantList frames;
    frames << f0 << f1;

    kfObj->setProperty("frames", frames);
    kfObj->setProperty("animatedProperty", QStringLiteral("opacity"));
    kfObj->setProperty("target", QVariant::fromValue(target));

    // Directly set progress (bypasses the NumberAnimation event loop) and check _numAt.
    kfObj->setProperty("progress", 0.0);
    QVERIFY2(std::abs(target->property("opacity").toReal() - 0.0) < 0.01,
             qPrintable(QString::number(target->property("opacity").toReal())));

    kfObj->setProperty("progress", 0.5);
    QVERIFY2(std::abs(target->property("opacity").toReal() - 0.5) < 0.01,
             qPrintable(QString::number(target->property("opacity").toReal())));

    kfObj->setProperty("progress", 1.0);
    QVERIFY2(std::abs(target->property("opacity").toReal() - 1.0) < 0.01,
             qPrintable(QString::number(target->property("opacity").toReal())));

    // Quarter-way: 0.25 -> opacity 0.25 (linear).
    kfObj->setProperty("progress", 0.25);
    QVERIFY2(std::abs(target->property("opacity").toReal() - 0.25) < 0.01,
             qPrintable(QString::number(target->property("opacity").toReal())));
}

// --- C++ CssRect @keyframes animation driver -----------------------------------------------
//
// Proves that CssRect's live @keyframes driver is wired: a CssRect with
// `animation: spin 200ms linear infinite` (and a matching @keyframes block in the theme)
// actually ticks animTick via a REAL NumberAnimation and drives rotation() through applyAnim.
//
// Two cases:
//  1. Positive: animation active → rotation() moves away from 0 within 2s.
//  2. Negative: static transform only (no animation key) → rotation() is the parsed value.
//
// Style is injected directly via setProperty("style", map) to bypass the selector-resolution
// path (which has a pre-existing failure unrelated to this driver — parsesKeyframesAndAnimation).
void QmlCssTests::cssRectCppKeyframesDriverLive()
{
    // --- shared setup ---
    // Load @keyframes spin (0deg → 180deg) into the theme.  We don't need a selector rule —
    // we inject style directly, so only keyframes lookup matters here.
    CssTheme theme;
    theme.loadFromString(QStringLiteral(
        "@keyframes spin { from { transform: rotate(0deg); } to { transform: rotate(180deg); } }"));

    CssLayoutEngine layout(&theme);

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layout);

    // --- positive: animation is live, rotation leaves 0 ---
    {
        QQmlComponent comp(&engine);
        comp.setData(R"(
            import QtQuick
            import qmlcss
            CssRect { width: 60; height: 60 }
        )", QUrl());
        QScopedPointer<QObject> obj(comp.create());
        QVERIFY2(obj, qPrintable(comp.errorString()));
        auto *rect = qobject_cast<CssRect *>(obj.data());
        QVERIFY(rect);

        // Inject style with `animation: spin 200ms linear infinite` and no static transform.
        // This triggers recompute() → buildAnimStops (2 stops) → _animActive=true → NumberAnimation
        // starts → tick fires → applyAnim writes _animRotate → applyTransform sets rotation().
        QVariantMap animStyle;
        animStyle.insert(QStringLiteral("animation"), QStringLiteral("spin 200ms linear infinite"));
        rect->setProperty("style", animStyle);

        // Within 2s (>>200ms animation period) rotation() must leave 0. QTRY_VERIFY pumps the
        // event loop, allowing the NumberAnimation to fire its first tick.
        QTRY_VERIFY_WITH_TIMEOUT(rect->rotation() > 0.5, 2000);
    }

    // --- negative: static transform, no animation — rotation() is the CSS value ---
    {
        QQmlComponent comp(&engine);
        comp.setData(R"(
            import QtQuick
            import qmlcss
            CssRect { width: 60; height: 60 }
        )", QUrl());
        QScopedPointer<QObject> obj(comp.create());
        QVERIFY2(obj, qPrintable(comp.errorString()));
        auto *rect = qobject_cast<CssRect *>(obj.data());
        QVERIFY(rect);

        // Static transform only — _animActive=false, static branch in applyTransform.
        QVariantMap staticStyle;
        staticStyle.insert(QStringLiteral("transform"), QStringLiteral("rotate(45deg)"));
        rect->setProperty("style", staticStyle);

        QVERIFY2(std::abs(rect->rotation() - 45.0) < 0.5,
                 qPrintable(QString::number(rect->rotation())));
    }
}

// --- C++ CssIcon (composition translation of CssIcon.qml) ------------------------------------
//
// Proves CssIcon registers via `import qmlcss`, composes a REAL QtQuick Image and a REAL
// QtQuick.Effects MultiEffect, and correctly drives the icon source URL from a QVariantMap style
// (icon-name key → themeicon URL) and a fallbackSource, as well as the colorization props.
void QmlCssTests::cssIconCppComposesImageAndEffect()
{
    QQmlEngine engine;

    // 1. Basic construction: compose Image + MultiEffect.
    QQmlComponent comp(&engine);
    comp.setData(R"(
        import QtQuick
        import qmlcss
        CssIcon {
            width: 48; height: 48
        }
    )", QUrl());

    QScopedPointer<QObject> object(comp.create());
    QVERIFY2(object, qPrintable(comp.errorString()));
    auto *icon = qobject_cast<CssIcon *>(object.data());
    QVERIFY(icon);

    // Find the composed REAL Image and MultiEffect children.
    QQuickItem *image = nullptr;
    QQuickItem *effect = nullptr;
    for (QQuickItem *k : icon->childItems()) {
        const QByteArray cls(k->metaObject()->className());
        if (cls.contains("QQuickImage"))
            image = k;
        else if (cls.contains("QQuickMultiEffect"))
            effect = k;
    }
    QVERIFY2(image, "composed Image missing");
    QVERIFY2(effect, "composed MultiEffect missing");

    // Image starts invisible (effect is the renderer); effect also invisible (no source loaded yet).
    QCOMPARE(image->isVisible(), false);
    // Initially no source is set, so effect is hidden too.
    QCOMPARE(effect->isVisible(), false);

    // Image fillMode == Image.PreserveAspectFit (1).
    QCOMPARE(image->property("fillMode").toInt(), 1);

    // Effect source points at the composed image.
    QCOMPARE(effect->property("source").value<QQuickItem *>(), image);

    // 2. Colorization props: default colorize=true, color=white.
    QVERIFY2(std::abs(effect->property("colorization").toReal() - 1.0) < 1e-6,
             qPrintable(QString::number(effect->property("colorization").toReal())));
    QCOMPARE(effect->property("colorizationColor").value<QColor>(), QColor(Qt::white));

    // 3. setColorize(false) → colorization 0.
    icon->setColorize(false);
    QVERIFY2(std::abs(effect->property("colorization").toReal() - 0.0) < 1e-6,
             qPrintable(QString::number(effect->property("colorization").toReal())));

    // 4. setColor → colorizationColor updated.
    icon->setColorize(true);
    icon->setColor(QColor(QStringLiteral("#ff4400")));
    QCOMPARE(effect->property("colorizationColor").value<QColor>(), QColor(QStringLiteral("#ff4400")));

    // 5. fallbackSource: setFallbackSource drives the Image source URL.
    icon->setFallbackSource(QStringLiteral("qrc:/icons/fallback.svg"));
    const QString fbSrc = image->property("source").toUrl().toString();
    QCOMPARE(fbSrc, QStringLiteral("qrc:/icons/fallback.svg"));

    // 6. icon-name CSS key → "image://themeicon/<name>|<hexcolor-without-#>".
    //    The color is #ff4400 (set above), hex = "ff4400".
    QVariantMap style;
    style[QStringLiteral("icon-name")] = QStringLiteral("search");
    icon->setStyle(style);
    // Qt encodes the pipe '|' as '%7C' when converting to QUrl internally; decode first.
    const QString themeUrl = image->property("source").toUrl().toString();
    const QString themeDecoded = QUrl::fromPercentEncoding(themeUrl.toUtf8());
    QVERIFY2(themeDecoded.startsWith(QLatin1String("image://themeicon/search|")),
             qPrintable(themeDecoded));
    // The hex part after '|' must be the color without '#': ff4400.
    QVERIFY2(themeDecoded.endsWith(QLatin1String("ff4400")),
             qPrintable(themeDecoded));

    // 7. icon CSS key overrides fallbackSource; sourceFromCssUrl strips url(...).
    QVariantMap style2;
    style2[QStringLiteral("icon")] = QStringLiteral("url(qrc:/icons/star.png)");
    icon->setStyle(style2);
    const QString urlSrc = image->property("source").toUrl().toString();
    QCOMPARE(urlSrc, QStringLiteral("qrc:/icons/star.png"));

    // 8. iconSize auto-derives from geometry (width=height=48, no explicit set).
    // icon was created 48×48 — auto iconSize should be 48.
    QCOMPARE(icon->iconSize(), 48);
}

// --- C++ CssFillLayer (composition translation of CssFillLayer.qml) -------------------------
//
// Proves CssFillLayer registers via `import qmlcss`, composes a REAL QtQuick Shape as its only
// child, and correctly drives the Shape's opacity from peakAlpha (solid color alpha or max stop
// alpha), so the fill renders opaque and alpha is carried on opacity — exactly as the QML did.
void QmlCssTests::cssFillLayerCppComposesShape()
{
    QQmlEngine engine;

    // --- Test 1: solid colour with alpha 0.5 → composed Shape opacity == 0.5 ---------------
    QQmlComponent comp1(&engine);
    comp1.setData(R"(
        import QtQuick
        import qmlcss
        CssFillLayer {
            width: 50; height: 50
            spec: ({ type: "color", color: Qt.rgba(1, 0, 0, 0.5) })
            radii: [0, 0, 0, 0]
        }
    )", QUrl());

    QScopedPointer<QObject> o1(comp1.create());
    QVERIFY2(o1, qPrintable(comp1.errorString()));
    auto *layer1 = qobject_cast<QQuickItem *>(o1.data());
    QVERIFY(layer1);

    // Exactly one composed child: the real Shape.
    const QList<QQuickItem *> kids1 = layer1->childItems();
    QCOMPARE(kids1.size(), 1);
    QQuickItem *shape1 = kids1.first();
    QVERIFY(shape1);
    QVERIFY2(QByteArray(shape1->metaObject()->className()).contains("QQuickShape"),
             shape1->metaObject()->className());
    QVERIFY2(!QByteArray(shape1->metaObject()->className()).contains("QQuickRectangle"),
             shape1->metaObject()->className());

    // peakAlpha = solid color alpha = 0.5 → Shape opacity == 0.5 (alpha on opacity, fill opaque).
    QVERIFY2(std::abs(shape1->property("opacity").toReal() - 0.5) < 0.01,
             qPrintable(QString::number(shape1->property("opacity").toReal())));

    // --- Test 2: linear gradient, stops with alpha 1.0 and 0.8 → peakAlpha == 1.0 ----------
    QQmlComponent comp2(&engine);
    comp2.setData(R"(
        import QtQuick
        import qmlcss
        CssFillLayer {
            width: 60; height: 20
            spec: ({
                type: "linear", angle: 90,
                stops: [
                    { position: 0.0, color: Qt.rgba(1, 0, 0, 1.0) },
                    { position: 1.0, color: Qt.rgba(0, 0, 1, 0.8) }
                ]
            })
            radii: [0, 0, 0, 0]
        }
    )", QUrl());

    QScopedPointer<QObject> o2(comp2.create());
    QVERIFY2(o2, qPrintable(comp2.errorString()));
    auto *layer2 = qobject_cast<QQuickItem *>(o2.data());
    QVERIFY(layer2);

    const QList<QQuickItem *> kids2 = layer2->childItems();
    QCOMPARE(kids2.size(), 1);
    QQuickItem *shape2 = kids2.first();
    QVERIFY(shape2);
    QVERIFY2(QByteArray(shape2->metaObject()->className()).contains("QQuickShape"),
             shape2->metaObject()->className());

    // peakAlpha = max(1.0, 0.8) = 1.0.
    QVERIFY2(std::abs(shape2->property("opacity").toReal() - 1.0) < 0.01,
             qPrintable(QString::number(shape2->property("opacity").toReal())));
}

// --- C++ CssItem — migrated from cssItemAppliesToParent, now uses `import qmlcss` ----------
//
// Proves CssItem registers via `import qmlcss`, infers "rect" from its Rectangle parent (which
// has both `radius` and `color` properties), and correctly applies background-color and
// border-radius onto that parent — same assertions as the original QML-based test.
void QmlCssTests::cssItemCppAppliesToParent()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral("#item2 { background-color: #112233; border-radius: 8px; }"));

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);

    QQmlComponent component(&engine);
    // `import qmlcss` proves the C++ registration (not the removed qrc QML file).
    component.setData(R"(
        import QtQuick
        import qmlcss

        Rectangle {
            width: 30
            height: 30

            CssItem {
                cssId: "item2"
            }
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));

    // CssItem inferred "rect" from the Rectangle parent and applied the CSS props.
    const QColor color = object->property("color").value<QColor>();
    QCOMPARE(color, QColor(QStringLiteral("#112233")));
    QCOMPARE(object->property("radius").toReal(), 8.0);
}

// --- C++ Contrast (ported from Contrast.js) --------------------------------------------------
//
// (a) Pure C++ validation: contrastRatio(white, black) ≈ 21, toHex red, blendOver at 0.5,
//     needsDarkIcon on light/dark bg, naturalContrastColor already passing the target.
void QmlCssTests::contrastCppPure()
{
    Contrast c;

    // contrastRatio(white, black) must be close to 21 (WCAG maximum).
    const QColor white = QColor::fromRgbF(1.0, 1.0, 1.0, 1.0);
    const QColor black = QColor::fromRgbF(0.0, 0.0, 0.0, 1.0);
    const double ratio = c.contrastRatio(white, black);
    QVERIFY2(std::abs(ratio - 21.0) < 0.5, qPrintable(QString::number(ratio)));

    // toHex of pure red (#ff0000).
    const QColor red = QColor::fromRgbF(1.0, 0.0, 0.0, 1.0);
    QCOMPARE(c.toHex(red), QStringLiteral("#ff0000"));

    // blendOver: semi-transparent white over black → grey ~(0.5, 0.5, 0.5, 1).
    const QColor semiWhite = QColor::fromRgbF(1.0, 1.0, 1.0, 0.5);
    const QColor blended = c.blendOver(semiWhite, black);
    QVERIFY2(std::abs(blended.redF()   - 0.5) < 0.01, qPrintable(QString::number(blended.redF())));
    QVERIFY2(std::abs(blended.greenF() - 0.5) < 0.01, qPrintable(QString::number(blended.greenF())));
    QVERIFY2(std::abs(blended.blueF()  - 0.5) < 0.01, qPrintable(QString::number(blended.blueF())));
    QCOMPARE(blended.alphaF(), 1.0);

    // needsDarkIcon: white bg is light (luminance ≥ 0.5) → true; black bg → false.
    QCOMPARE(c.needsDarkIcon(white), true);
    QCOMPARE(c.needsDarkIcon(black), false);

    // naturalContrastColor: fg already satisfying the target is returned unchanged.
    // white vs black contrast ≈ 21; target 4.5 → should return white as-is.
    const QColor result = c.naturalContrastColor(white, black, 4.5);
    QCOMPARE(result, white);
}

// (b) QML singleton validation: `import qmlcss` + `Contrast.toHex(...)` — proves registration.
void QmlCssTests::contrastQmlSingleton()
{
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        Item {
            property string hexRed: Contrast.toHex(Qt.rgba(1, 0, 0, 1))
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    QCOMPARE(object->property("hexRed").toString(), QStringLiteral("#ff0000"));
}

// The QML original duck-typed: `if (holder.parent.requestRelayout) holder.parent.requestRelayout()`.
// A plain-Item grandparent (window content, transpiler wrapper, ...) has no requestRelayout();
// it must be skipped silently — invokeMethod-by-name warns "No such method" on every notify.
void QmlCssTests::layoutNotifyParentSkipsNonBoxAncestors()
{
    static QStringList warnings;
    warnings.clear();
    const auto previous = qInstallMessageHandler(
        [](QtMsgType type, const QMessageLogContext &, const QString &msg) {
            if (type == QtWarningMsg)
                warnings.append(msg);
        });

    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        Item {            // plain grandparent — no requestRelayout()
            Item {        // holder
                CssText { text: "hi" }
            }
        }
    )", QUrl());
    QScopedPointer<QObject> object(component.create());

    CssTheme theme;
    CssLayoutEngine layout(&theme);
    auto *grandparent = qobject_cast<QQuickItem *>(object.data());
    QVERIFY(grandparent);
    QQuickItem *holder = grandparent->childItems().value(0);
    QVERIFY(holder);
    QQuickItem *text = holder->childItems().value(0);
    QVERIFY(text);

    layout.notifyParentLayout(text);

    qInstallMessageHandler(previous);
    for (const QString &w : std::as_const(warnings))
        QVERIFY2(!w.contains(QLatin1String("No such method")),
                 qPrintable(QStringLiteral("notifyParentLayout warned: ") + w));
}

// End-to-end web-parity scoping: `.nav button` must style ONLY buttons under a `.nav`
// element (the chain is collected from the item tree by applyCssTo), and toggling a class
// on the ANCESTOR (`button.active` here) must restyle scoped DESCENDANT rules
// (`.nav button.active text`) — the text child itself never changes its own identity.
void QmlCssTests::ancestorScopedRulesStyleTheTree()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(R"(
        text { color: #101010; }
        .nav button { background-color: #112233; }
        .nav button.active text { color: #ffffff; }
    )"));

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);

    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        Item {
            width: 400; height: 200
            CssRect {
                objectName: "nav"
                cssClass: ["nav"]
                cssPrimitive: "div"
                CssFill {
                    objectName: "inside"
                    cssPrimitive: "button"
                    CssText { objectName: "label"; cssPrimitive: "text"; text: "hi" }
                }
            }
            CssFill {
                objectName: "outside"
                cssPrimitive: "button"
            }
        }
    )", QUrl());

    QScopedPointer<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));

    QObject *inside = root->findChild<QObject *>(QStringLiteral("inside"));
    QObject *outside = root->findChild<QObject *>(QStringLiteral("outside"));
    QObject *label = root->findChild<QObject *>(QStringLiteral("label"));
    QVERIFY(inside && outside && label);

    // Scoped: only the button under .nav gets the background.
    QCOMPARE(inside->property("style").toMap().value(QStringLiteral("background-color")).toString(),
             QStringLiteral("#112233"));
    QVERIFY(!outside->property("style").toMap().contains(QStringLiteral("background-color")));

    // Label starts at the bare `text {}` colour.
    QCOMPARE(label->property("style").toMap().value(QStringLiteral("color")).toString(),
             QStringLiteral("#101010"));

    // Turning the BUTTON active must restyle the LABEL (ancestor-scoped rule).
    inside->setProperty("cssClass", QVariant(QStringList{QStringLiteral("active")}));
    QCOMPARE(label->property("style").toMap().value(QStringLiteral("color")).toString(),
             QStringLiteral("#ffffff"));
}

// vh heights must follow setViewport without any item geometry change: the layout engine
// re-queues every known root on viewportChanged (single-event tiling resizes went stale).
void QmlCssTests::viewportChangeRelayoutsVhHeights()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(".page { height: calc(100vh - 40px); }"));
    theme.setViewport(800, 400);

    QQmlEngine engine;
    CssLayoutEngine layout(&theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layout);

    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        CssRect {
            cssPrimitive: ""
            width: 800; height: 1000
            style: ({ "display": "flex", "flex-direction": "column" })
            CssRect {
                objectName: "page"
                cssClass: ["page"]
                cssPrimitive: "div"
            }
        }
    )", QUrl());

    QScopedPointer<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));
    QObject *page = root->findChild<QObject *>(QStringLiteral("page"));
    QVERIFY(page);
    QTRY_COMPARE(page->property("height").toReal(), 360.0); // 400 - 40

    // Height-only viewport change (the tiling case): no item geometry changes, ONLY the
    // viewport — the page must still follow.
    theme.setViewport(800, 600);
    QTRY_COMPARE(page->property("height").toReal(), 560.0); // 600 - 40
}

// Popup/Menu contents are reparented by Qt to the window Overlay, severing the visual
// parent chain that descendant selectors are matched against. The declared
// `property Item cssAncestor: owner` must re-anchor the walk at the logical owner so
// `.wg-select .popup` still styles the popup — and rules NOT scoped to the owner must
// keep NOT matching.
void QmlCssTests::cssAncestorReanchorsDetachedSubtree()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(R"(
        .wg-select .popup { background-color: #ffffff; }
        .other .popup { background-color: #ff0000; }
    )"));

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);

    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        Item {
            width: 400; height: 200
            CssFill {
                id: sel
                objectName: "select"
                cssClass: ["wg-select"]
                cssPrimitive: "select"
            }
            Item {
                objectName: "overlay" // plain Item: simulates the window Overlay (no CSS identity)
                CssFill {
                    objectName: "popup"
                    cssPrimitive: "div"
                    cssClass: ["popup"]
                    property Item cssAncestor: sel
                }
                CssFill {
                    objectName: "orphan"
                    cssPrimitive: "div"
                    cssClass: ["popup"] // same class, NO re-anchor: scoped rules must not match
                }
            }
        }
    )", QUrl());

    QScopedPointer<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));

    QObject *popup = root->findChild<QObject *>(QStringLiteral("popup"));
    QObject *orphan = root->findChild<QObject *>(QStringLiteral("orphan"));
    QVERIFY(popup && orphan);

    QCOMPARE(popup->property("style").toMap().value(QStringLiteral("background-color")).toString(),
             QStringLiteral("#ffffff"));
    QVERIFY(!orphan->property("style").toMap().contains(QStringLiteral("background-color")));
}

// text-shadow must not blank the label: MultiEffect sources the composed Text, and a
// visible:false source yields an EMPTY texture on the RHI path (the caption vanished).
// The label stays visible; the effect paints the same glyphs + shadow above it.
void QmlCssTests::cssTextShadowKeepsLabelVisible()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(
        ".cap { text-shadow: 0 2px 8px rgba(0, 0, 0, 0.65); color: #ffffff; }"));

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        CssText { cssClass: ["cap"]; cssPrimitive: "text"; text: "hi" }
    )", QUrl());
    QScopedPointer<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));

    QQuickItem *label = composedText(root.data());
    QVERIFY(label);
    QVERIFY2(label->isVisible(), "text-shadow hid the composed Text (empty-texture source)");
}

// The web paints `background-color` behind ANY element, text included — CssText composes a
// Shape underlay (never Rectangle) sized to its box. No background declared → underlay hidden.
void QmlCssTests::cssTextCppPaintsBackground()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(
        ".tile { background-color: #313244; border-radius: 4px; color: #cdd6f4; }"));

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);

    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        Item {
            width: 300; height: 60
            CssText { objectName: "tiled"; cssClass: ["tile"]; cssPrimitive: "text"; text: "hi"; width: 260; height: 20 }
            CssText { objectName: "bare"; cssPrimitive: "text"; text: "plain" }
        }
    )", QUrl());
    QScopedPointer<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));

    QObject *tiled = root->findChild<QObject *>(QStringLiteral("tiled"));
    QObject *bare = root->findChild<QObject *>(QStringLiteral("bare"));
    QVERIFY(tiled && bare);

    QObject *bg = tiled->findChild<QObject *>(QStringLiteral("cssTextBg"));
    QVERIFY2(bg, "CssText did not compose the background underlay");
    QCOMPARE(bg->property("bgColor").value<QColor>(), QColor(QStringLiteral("#313244")));
    QCOMPARE(bg->property("bgRadius").toReal(), 4.0);
    QVERIFY(bg->property("visible").toBool());
    // The underlay tracks the item's box.
    QCOMPARE(bg->property("width").toReal(), 260.0);
    QCOMPARE(bg->property("height").toReal(), 20.0);

    // No background declared → the underlay stays hidden (or is never created).
    QObject *bareBg = bare->findChild<QObject *>(QStringLiteral("cssTextBg"));
    QVERIFY(!bareBg || !bareBg->property("visible").toBool());
}

// The transpiler's <Show> gate is a `visible:` BINDING on the delegate root. CssRect used to
// run setVisible(display != "none") on EVERY style apply — an imperative write that severs the
// binding, so every <Show> inside a styled tree leaked visible. Only a declared `display` may
// drive visible.
void QmlCssTests::styleApplyPreservesVisibleBinding()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(".badge { background-color: #41cd52; }"));

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);

    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        Item {
            width: 100; height: 40
            property bool showIt: false
            CssRect { objectName: "badge"; cssClass: ["badge"]; cssPrimitive: "div"; visible: parent.showIt }
        }
    )", QUrl());
    QScopedPointer<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));

    auto *badge = root->findChild<QQuickItem *>(QStringLiteral("badge"));
    QVERIFY(badge);
    // The style (no `display`) was applied on completion; the binding must have survived.
    QCOMPARE(badge->isVisible(), false);
    root->setProperty("showIt", true);
    QCOMPARE(badge->isVisible(), true);

    // And `display: none` still hides — by PAINT (opacity 0) and input (disabled), with
    // `visible` (and its binding) untouched; the layout skips it by style.
    theme.loadFromString(QStringLiteral(".badge { display: none; }"));
    theme.loadCss(badge);
    QCOMPARE(badge->opacity(), 0.0);
    QVERIFY(!badge->isEnabled());
    QCOMPARE(badge->isVisible(), true); // author binding still owns `visible`

    // Removing display:none restores paint AND the binding still works.
    theme.loadFromString(QStringLiteral(".badge { background-color: #41cd52; }"));
    theme.loadCss(badge);
    QCOMPARE(badge->opacity(), 1.0);
    root->setProperty("showIt", false);
    QCOMPARE(badge->isVisible(), false); // the <Show> guard survived the round-trip
}

// `transition: opacity <ms>` must ANIMATE the item's opacity on restyle, not snap — the
// carousel crossfade depends on it. Sampled mid-flight with generous margins (CI-safe).
void QmlCssTests::transitionAnimatesItemOpacity()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(R"(
        .slide { opacity: 1; transition: opacity 400ms linear; }
        .slide.off { opacity: 0; }
    )"));

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        CssRect { objectName: "s"; cssClass: ["slide"]; cssPrimitive: "div"; width: 100; height: 50 }
    )", QUrl());
    QScopedPointer<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));
    auto *s = qobject_cast<QQuickItem *>(root.data());
    QVERIFY(s);
    QCOMPARE(s->opacity(), 1.0);

    s->setProperty("cssClass", QVariant(QStringList{QStringLiteral("slide"), QStringLiteral("off")}));
    QTest::qWait(120); // ~30% through a 400ms linear fade
    const qreal mid = s->opacity();
    QVERIFY2(mid > 0.02 && mid < 0.98, qPrintable(QStringLiteral("opacity snapped: %1").arg(mid)));
    QTRY_VERIFY_WITH_TIMEOUT(s->opacity() < 0.02, 2000); // settles at the target
}

// `transition: width <ms>` must animate the layout-applied width — the sidebar collapse.
void QmlCssTests::transitionAnimatesLayoutWidth()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(R"(
        .side { width: 200px; transition: width 400ms linear; }
        .side.collapsed { width: 60px; }
    )"));
    CssLayoutEngine layoutEngine(&theme);

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        CssRect {
            cssPrimitive: ""
            width: 400; height: 100
            style: ({ "display": "flex", "flex-direction": "row" })
            CssRect { objectName: "side"; cssClass: ["side"]; cssPrimitive: "div" }
        }
    )", QUrl());
    QScopedPointer<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));
    auto *side = root->findChild<QQuickItem *>(QStringLiteral("side"));
    QVERIFY(side);
    QTRY_COMPARE_WITH_TIMEOUT(side->width(), 200.0, 2000);

    side->setProperty("cssClass", QVariant(QStringList{QStringLiteral("side"), QStringLiteral("collapsed")}));
    QTest::qWait(120);
    const qreal mid = side->width();
    QVERIFY2(mid < 198.0 && mid > 62.0, qPrintable(QStringLiteral("width snapped: %1").arg(mid)));
    QTRY_VERIFY_WITH_TIMEOUT(std::abs(side->width() - 60.0) < 1.0, 2000);
}

// A `.card:hover` rule must flip cssHoverStyled on a matching CssRect (which then composes
// its own HoverHandler), and the engine-tracked hover state must restyle: forcing the
// internal flag resolves the :hover declarations.
void QmlCssTests::hoverRuleEnablesEngineHoverTracking()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(R"(
        .card { background-color: #ffffff; }
        .card:hover { background-color: #ff0000; }
        .plain { background-color: #eeeeee; }
    )"));

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        Item {
            CssRect { objectName: "card"; cssClass: ["card"]; cssPrimitive: "div"; width: 50; height: 50 }
            CssRect { objectName: "plain"; cssClass: ["plain"]; cssPrimitive: "div"; width: 50; height: 50 }
        }
    )", QUrl());
    QScopedPointer<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));

    QObject *card = root->findChild<QObject *>(QStringLiteral("card"));
    QObject *plain = root->findChild<QObject *>(QStringLiteral("plain"));
    QVERIFY(card && plain);
    QVERIFY2(card->property("cssHoverStyled").toBool(), "matching :hover rule not detected");
    QVERIFY(!plain->property("cssHoverStyled").toBool());

    // Simulate the tracked state (the composed HoverHandler writes the same property).
    card->setProperty("cssEngineHover", true);
    QCOMPARE(card->property("style").toMap().value(QStringLiteral("background-color")).toString(),
             QStringLiteral("#ff0000"));
    card->setProperty("cssEngineHover", false);
    QCOMPARE(card->property("style").toMap().value(QStringLiteral("background-color")).toString(),
             QStringLiteral("#ffffff"));
}

// `overflow-y: auto` (or scroll) turns the box's content area into a REAL Flickable —
// wheel/drag scrolling comes from QtQuick, not a reimplementation. contentHeight tracks the
// laid-out children so it scrolls exactly the overflow.
void QmlCssTests::overflowScrollComposesFlickable()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(R"(
        .list { overflow-y: auto; display: flex; flex-direction: column; }
        .row { height: 60px; }
    )"));
    CssLayoutEngine layoutEngine(&theme);

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        CssRect {
            objectName: "list"
            cssClass: ["list"]
            cssPrimitive: "div"
            width: 200; height: 150
            CssRect { cssClass: ["row"]; cssPrimitive: "div" }
            CssRect { cssClass: ["row"]; cssPrimitive: "div" }
            CssRect { cssClass: ["row"]; cssPrimitive: "div" }
            CssRect { cssClass: ["row"]; cssPrimitive: "div" }
            CssRect { cssClass: ["row"]; cssPrimitive: "div" }
        }
    )", QUrl());
    QScopedPointer<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));

    // 5 x 60px rows in a 150px viewport -> a Flickable with ~300px of content.
    QQuickItem *flick = nullptr;
    const auto kids = root->findChildren<QQuickItem *>();
    for (QQuickItem *k : kids) {
        if (QString::fromLatin1(k->metaObject()->className()).contains(QLatin1String("Flickable"))) {
            flick = k;
            break;
        }
    }
    QVERIFY2(flick, "overflow-y: auto did not compose a Flickable");
    QTRY_VERIFY_WITH_TIMEOUT(flick->property("contentHeight").toReal() >= 295.0, 2000);
    QVERIFY(flick->clip());

    // It actually scrolls: content offset moves.
    flick->setProperty("contentY", 120.0);
    QCOMPARE(flick->property("contentY").toReal(), 120.0);
}

// Flyweight <For>: reorder must reuse the SAME delegate items (moved, not recreated).
void QmlCssTests::cssRepeaterReordersWithoutRecreating()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(".row { height: 10px; }"));
    CssLayoutEngine layoutEngine(&theme);

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        CssRect {
            id: box
            cssPrimitive: ""
            width: 200; height: 200
            property var order: ["a", "b", "c"]
            style: ({ "display": "flex", "flex-direction": "column" })
            CssRepeater {
                model: box.order
                delegate: Component {
                    CssRect { objectName: "row-" + modelData; cssClass: ["row"]; cssPrimitive: "div" }
                }
            }
        }
    )", QUrl());
    QScopedPointer<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));

    auto rows = [&]() {
        QHash<QString, QQuickItem *> out;
        for (QQuickItem *k : root->findChildren<QQuickItem *>()) {
            if (k->objectName().startsWith(QLatin1String("row-")))
                out.insert(k->objectName(), k);
        }
        return out;
    };
    const auto before = rows();
    QCOMPARE(before.size(), 3);
    QQuickItem *a = before.value(QStringLiteral("row-a"));
    QQuickItem *c = before.value(QStringLiteral("row-c"));
    QVERIFY(a && c);

    // Reorder: c moves first — the INSTANCES must survive.
    root->setProperty("order", QVariant(QStringList{QStringLiteral("c"), QStringLiteral("a"), QStringLiteral("b")}));
    const auto after = rows();
    QCOMPARE(after.size(), 3);
    QCOMPARE(after.value(QStringLiteral("row-a")), a); // same pointer: reused, not recreated
    QCOMPARE(after.value(QStringLiteral("row-c")), c);

    // Layout follows the new order after the flush: c above a.
    QTRY_VERIFY_WITH_TIMEOUT(c->y() < a->y(), 2000);

    // Removal destroys only the removed row; the rest survive.
    root->setProperty("order", QVariant(QStringList{QStringLiteral("c"), QStringLiteral("a")}));
    QTRY_COMPARE_WITH_TIMEOUT(rows().size(), 2, 2000);
    QCOMPARE(rows().value(QStringLiteral("row-a")), a);
}

// The Shape x Rectangle policy: a rectangle-safe style composes a REAL QQuickRectangle
// (batchable scene-graph node) and NO Shape shell; a reapply that demands Shape features
// (gradient) swaps the composition — and back.
void QmlCssTests::rectanglePolicyFastPathAndSwap()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(R"(
        .card { background-color: #2244aa; border: 2px solid #ffcc00; border-radius: 6px 6px 0 0; }
        .card.fancy { background: linear-gradient(180deg, #2244aa, #112255); }
    )"));
    CssLayoutEngine layoutEngine(&theme);

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        CssRect { objectName: "card"; cssClass: ["card"]; cssPrimitive: "div"; width: 100; height: 40 }
    )", QUrl());
    QScopedPointer<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));

    auto findClass = [&](const char *cls) -> QQuickItem * {
        for (QQuickItem *k : root->findChildren<QQuickItem *>()) {
            if (QString::fromLatin1(k->metaObject()->className()).contains(QLatin1String(cls)))
                return k;
        }
        return nullptr;
    };

    // Fast path: a Rectangle, no Shape.
    QQuickItem *rect = findClass("QQuickRectangle");
    QVERIFY2(rect, "rectangle-safe style did not take the fast Rectangle path");
    QVERIFY(!findClass("QQuickShape"));
    QCOMPARE(rect->property("color").value<QColor>(), QColor(QStringLiteral("#2244aa")));
    QCOMPARE(rect->property("topLeftRadius").toReal(), 6.0);
    QCOMPARE(rect->property("bottomRightRadius").toReal(), 0.0);

    // Reapply with a gradient: the policy swaps to the Shape shell.
    root->setProperty("cssClass", QVariant(QStringList{QStringLiteral("card"), QStringLiteral("fancy")}));
    QTRY_VERIFY_WITH_TIMEOUT(findClass("QQuickShape") != nullptr, 2000);
    QTRY_VERIFY_WITH_TIMEOUT(findClass("QQuickRectangle") == nullptr, 2000);

    // And back.
    root->setProperty("cssClass", QVariant(QStringList{QStringLiteral("card")}));
    QTRY_VERIFY_WITH_TIMEOUT(findClass("QQuickRectangle") != nullptr, 2000);
    QTRY_VERIFY_WITH_TIMEOUT(findClass("QQuickShape") == nullptr, 2000);
}

QTEST_MAIN(QmlCssTests)
