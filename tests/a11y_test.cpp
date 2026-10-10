// Accessibility of the CSS primitives (Text/Image in solidqml.Widgets): the semantics a screen reader
// sees, and the cost contract — while no AT client has asked, an element carries one bool (the
// QQuickItemPrivate::isAccessible flag), not an Accessible attached object + connections.
#include "qmlcss/QMLCss.h"
#include "widgets/solidwidgets.h"

#include <QAccessible>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScopedPointer>
#include <QTest>
#include <QtQuick/private/qquickaccessibleattached_p.h>
#include <QtQuick/private/qquickitem_p.h>

namespace {

// Depth-first search of the accessible tree for an interface whose Name is `name`.
bool findByName(QAccessibleInterface *iface, const QString &name, int depth = 0)
{
    if (!iface || depth > 64)
        return false;
    if (iface->text(QAccessible::Name) == name)
        return true;
    for (int i = 0, n = iface->childCount(); i < n; ++i) {
        if (findByName(iface->child(i), name, depth + 1))
            return true;
    }
    return false;
}

} // namespace

class A11yTest : public QObject {
    Q_OBJECT

private:
    QmlCss::CssTheme m_theme;
    QmlCss::CssLayoutEngine m_layout { &m_theme };

    // The loader's init: engine types, widget types, and the cssTheme/cssLayout context properties.
    void wire(QQmlEngine &engine)
    {
        engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &m_theme);
        engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &m_layout);
    }

    static QObject *create(QQmlEngine &engine, const char *qml)
    {
        QQmlComponent component(&engine);
        component.setData(qml, QUrl());
        QObject *o = component.create();
        if (!o)
            qWarning().noquote() << component.errorString();
        return o;
    }

private slots:
    void initTestCase()
    {
        QmlCss::registerTypes();
        SolidWidgets::registerTypes();
    }

    void textIsDiscoverableWithoutAttachedObject()
    {
        QQmlEngine engine;
        wire(engine);
        QScopedPointer<QObject> o(create(engine, "import solidqml.Widgets\nText { text: \"hello\" }"));
        QVERIFY(o);
        auto *item = qobject_cast<QQuickItem *>(o.data());
        QVERIFY(item);
        QCOMPARE(qmlAttachedPropertiesObject<QQuickAccessibleAttached>(item, false), nullptr);
        QVERIFY(QQuickItemPrivate::get(item)->isAccessible);
    }

    void textRoleAndNameAreLive()
    {
        QQmlEngine engine;
        wire(engine);
        QScopedPointer<QObject> o(create(engine, "import solidqml.Widgets\nText { text: \"hello\" }"));
        QVERIFY(o);
        QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(o.data());
        QVERIFY(iface);
        QCOMPARE(iface->role(), QAccessible::StaticText);
        QCOMPARE(iface->text(QAccessible::Name), QStringLiteral("hello"));
        o->setProperty("text", QStringLiteral("bye"));
        QCOMPARE(iface->text(QAccessible::Name), QStringLiteral("bye"));

        QScopedPointer<QObject> h(create(engine, "import solidqml.Widgets\nText { cssPrimitive: \"h2\"; text: \"Title\" }"));
        QVERIFY(h);
        QAccessibleInterface *hi = QAccessible::queryAccessibleInterface(h.data());
        QVERIFY(hi);
        QCOMPARE(hi->role(), QAccessible::Heading);
        QCOMPARE(hi->text(QAccessible::Name), QStringLiteral("Title"));
    }

    void imageAltIsTheName()
    {
        QQmlEngine engine;
        wire(engine);
        QScopedPointer<QObject> o(create(engine, "import solidqml.Widgets\nImage { alt: \"logo\" }"));
        QVERIFY(o);
        auto *item = qobject_cast<QQuickItem *>(o.data());
        QVERIFY(item);
        QCOMPARE(qmlAttachedPropertiesObject<QQuickAccessibleAttached>(item, false), nullptr);
        QVERIFY(QQuickItemPrivate::get(item)->isAccessible);
        QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(o.data());
        QVERIFY(iface);
        QCOMPARE(iface->role(), QAccessible::Graphic);
        QCOMPARE(iface->text(QAccessible::Name), QStringLiteral("logo"));
        o->setProperty("alt", QStringLiteral("brand"));
        QCOMPARE(iface->text(QAccessible::Name), QStringLiteral("brand"));
    }

    void treeReachesTextFromWindow()
    {
        QQmlEngine engine;
        wire(engine);
        QScopedPointer<QObject> o(create(engine,
            "import solidqml.Widgets\nDiv { width: 200; height: 100; Text { text: \"hello\" } }"));
        QVERIFY(o);
        auto *root = qobject_cast<QQuickItem *>(o.data());
        QVERIFY(root);
        QQuickWindow window;
        window.resize(200, 100);
        root->setParentItem(window.contentItem());
        QAccessibleInterface *wi = QAccessible::queryAccessibleInterface(&window);
        QVERIFY(wi);
        QVERIFY(findByName(wi, QStringLiteral("hello")));
        root->setParentItem(nullptr);
    }
};

QTEST_MAIN(A11yTest)
#include "a11y_test.moc"
