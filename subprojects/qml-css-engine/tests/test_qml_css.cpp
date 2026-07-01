#include "test_qml_css.h"

#include "qmlcss/csslayout.h"
#include "qmlcss/csstheme.h"

#include <QColor>
#include <QFont>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QSignalSpy>
#include <QTest>
#include <QTextOption>

#include <cmath>

namespace {

// Locate the CssRect/CssFill contentHolder that owns `child` (the Item whose children are the
// layout participants): it is the child's immediate parent item. The layout root is one level up.
QQuickItem *contentHolderOf(QQuickItem *child)
{
    return child ? child->parentItem() : nullptr;
}

} // namespace

void QmlCssTests::cssRectLoadsAndRestyles()
    {
        CssTheme theme;
        theme.loadFromString(QStringLiteral("#box { background-color: #112233; border-radius: 7px; }"));

        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);

        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick
            import "qrc:/qmlcss" as Css

            Css.CssRect {
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
            import "qrc:/qmlcss" as Css

            Css.CssText {
                cssId: "label"
                text: "Label"
            }
        )", QUrl());

        QScopedPointer<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));

        QCOMPARE(object->property("style").toMap().value(QStringLiteral("font-size")).toString(),
                 QStringLiteral("18px"));
        const QColor color = object->property("color").value<QColor>();
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
        component.setData(R"(
            import QtQuick
            import "qrc:/qmlcss" as Css

            Rectangle {
                width: 24
                height: 24

                Css.CssItem {
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
        import "qrc:/qmlcss" as Css

        Css.CssRect {
            cssPrimitive: ""
            width: 100; height: 50
            style: ({ "display": "flex", "flex-direction": "column", "gap": "0px" })

            Css.CssRect { objectName: "a"; cssPrimitive: ""; style: ({ "height": "30px" }) }
            Css.CssRect { objectName: "b"; cssPrimitive: ""; style: ({ "height": "30px" }) }
            Css.CssRect { objectName: "c"; cssPrimitive: ""; style: ({ "height": "30px" }) }
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
        import "qrc:/qmlcss" as Css

        Css.CssRect {
            cssPrimitive: ""
            width: 300; height: 60
            style: ({ "display": "flex", "flex-direction": "row", "gap": "0px" })

            Css.CssRect {
                objectName: "capped"; cssPrimitive: ""
                style: ({ "width": "200px", "max-width": "50px" })
            }
            Css.CssRect {
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
        import "qrc:/qmlcss" as Css

        Css.CssRect {
            cssPrimitive: ""
            width: 300; height: 40
            style: ({ "display": "flex", "flex-direction": "row", "gap": "0px" })

            Css.CssRect { objectName: "a"; cssPrimitive: ""; style: ({ "width": "30px", "order": "2" }) }
            Css.CssRect { objectName: "b"; cssPrimitive: ""; style: ({ "width": "30px", "order": "0" }) }
            Css.CssRect { objectName: "c"; cssPrimitive: ""; style: ({ "width": "30px", "order": "1" }) }
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
        import "qrc:/qmlcss" as Css

        Css.CssRect {
            cssPrimitive: ""
            width: 400; height: 40
            style: ({ "display": "flex", "flex-direction": "row", "gap": "0px" })

            Css.CssRect { objectName: "a"; cssPrimitive: ""; style: ({ "flex-basis": "40px" }) }
            Css.CssRect { objectName: "b"; cssPrimitive: ""; style: ({ "flex-basis": "80px" }) }
            Css.CssRect { objectName: "c"; cssPrimitive: ""; style: ({ "flex-basis": "120px" }) }
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
        import "qrc:/qmlcss" as Css

        Css.CssRect {
            cssPrimitive: ""
            width: 300; height: 60
            style: ({ "display": "flex", "flex-direction": "row", "gap": "0px" })

            Css.CssRect {
                objectName: "ghost"; cssPrimitive: ""
                style: ({ "width": "50px", "visibility": "hidden" })
            }
            Css.CssRect { objectName: "sibling"; cssPrimitive: ""; style: ({ "width": "30px" }) }
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
        import "qrc:/qmlcss" as Css

        Css.CssRect {
            cssPrimitive: ""
            width: 300; height: 200
            style: ({
                "display": "grid",
                "grid-template-columns": "100px 100px",
                "grid-template-rows": "40px 40px",
                "grid-template-areas": '"head head" "side main"',
                "gap": "10px"
            })

            Css.CssRect { objectName: "head"; cssPrimitive: ""; style: ({ "grid-area": "head" }) }
            Css.CssRect { objectName: "side"; cssPrimitive: ""; style: ({ "grid-area": "side" }) }
            Css.CssRect { objectName: "main"; cssPrimitive: ""; style: ({ "grid-area": "main" }) }
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
        import "qrc:/qmlcss" as Css

        Css.CssRect {
            cssPrimitive: ""
            width: 100; height: 40
            style: ({ "display": "flex", "flex-direction": "row", "gap": "0px" })

            Css.CssRect { objectName: "a"; cssPrimitive: ""; style: ({ "width": "60px", "flex-shrink": "1" }) }
            Css.CssRect { objectName: "b"; cssPrimitive: ""; style: ({ "width": "60px", "flex-shrink": "1" }) }
            Css.CssRect { objectName: "c"; cssPrimitive: ""; style: ({ "width": "60px", "flex-shrink": "1" }) }
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
        import "qrc:/qmlcss" as Css

        Css.CssRect {
            cssPrimitive: ""
            width: 100; height: 200
            style: ({
                "display": "flex", "flex-direction": "row", "flex-wrap": "wrap",
                "gap": "0px", "align-content": "center"
            })

            Css.CssRect { objectName: "a"; cssPrimitive: ""; style: ({ "width": "60px", "height": "20px" }) }
            Css.CssRect { objectName: "b"; cssPrimitive: ""; style: ({ "width": "60px", "height": "20px" }) }
            Css.CssRect { objectName: "c"; cssPrimitive: ""; style: ({ "width": "60px", "height": "20px" }) }
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
        import "qrc:/qmlcss" as Css

        Css.CssRect {
            cssPrimitive: ""
            width: 400; height: 60
            style: ({ "display": "flex", "flex-direction": "row", "gap": "0px" })

            Css.CssRect {
                objectName: "border"; cssPrimitive: ""
                style: ({ "width": "100px", "padding": "10px", "box-sizing": "border-box" })
            }
            Css.CssRect {
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
        import "qrc:/qmlcss" as Css

        Css.CssFill {
            cssPrimitive: "button"
            width: 100; height: 40

            Css.CssText { objectName: "label"; text: "Click" }
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
    // The label has no colour of its own, so it inherits white from the button parent.
    const QColor color = label->property("color").value<QColor>();
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
        import "qrc:/qmlcss" as Css

        Css.CssRect {
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
        import "qrc:/qmlcss" as Css

        Css.CssRect {
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
        import "qrc:/qmlcss" as Css

        Css.CssText {
            cssId: "uline"
            text: "Hello"
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));

    const QFont font = object->property("font").value<QFont>();
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
        import "qrc:/qmlcss" as Css

        Css.CssText {
            cssId: "nowrap"
            text: "Hello world"
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));

    // Text.NoWrap maps to QTextOption::NoWrap == 0
    QCOMPARE(object->property("wrapMode").toInt(), static_cast<int>(QTextOption::NoWrap));
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
        import "qrc:/qmlcss" as Css

        Css.CssText {
            cssId: "ellipsis"
            text: "A very long piece of text that should be elided"
            width: 50
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));

    // Text.ElideRight maps to Qt::ElideRight == 1
    QCOMPARE(object->property("elide").toInt(), static_cast<int>(Qt::ElideRight));
}

QTEST_MAIN(QmlCssTests)
