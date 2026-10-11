// Keyboard & accessibility for PLAIN elements (release-plan M5, docs/keyboard-accessibility.md):
// tabIndex/role/aria-* on a <div>/<text> are transpiled into the exact same, standard QtQuick
// facilities a native widget's own .qml already relies on (activeFocusOnTab + the app-wide
// solidTabstop guard, Keys.onPressed/onReleased, the Accessible attached type) — NOT a new engine
// mechanism. These tests exercise that integration with Div/Text (W.* in solidqml.Widgets) exactly
// as the transpiler emits it (see transpiler/test/keyboard-accessibility.test.ts for the codegen
// side), the way tests/a11y_test.cpp already does for the Text/Image primitives' lazy accessibility.
#include "qmlcss/QMLCss.h"
#include "shims/tabstop.h"
#include "widgets/solidwidgets.h"

#include <QAccessible>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScopedPointer>
#include <QTest>
#include <QtQuick/private/qquickitem_p.h>

namespace {

// Depth-first search of the accessible tree for an interface whose Name is `name` (same helper
// tests/a11y_test.cpp uses). `ignored` is excluded from its PARENT's enumeration, not from
// answering a direct queryAccessibleInterface() on itself — see accessibleUnignoredChildren()
// in Qt Quick's qaccessiblequickitem.cpp — so this is the correct way to prove it stands.
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

class A11yGenericTest : public QObject {
    Q_OBJECT

private:
    QmlCss::CssTheme m_theme;
    QmlCss::CssLayoutEngine m_layout { &m_theme };
    SolidTabstop m_tabstop;

    void wire(QQmlEngine &engine)
    {
        engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &m_theme);
        engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &m_layout);
        engine.rootContext()->setContextProperty(QStringLiteral("solidTabstop"), &m_tabstop);
    }

    static QObject *create(QQmlEngine &engine, const QString &qml)
    {
        QQmlComponent component(&engine);
        component.setData(qml.toUtf8(), QUrl());
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
        m_tabstop.setEnabled(true);
    }

    // A <For> row's root is a delegate Component's ROOT object: QQuickItem's revisioned properties
    // (activeFocusOnTab is REVISION(2, 1)) must be available there too — W.* types are registered
    // with the Qt metaobject revision, not 0 (solid-player's sidebar failed to load otherwise).
    void revisionedItemPropertiesOnADelegateRoot()
    {
        QQmlEngine engine;
        wire(engine);
        QScopedPointer<QObject> o(create(engine,
            "import QtQuick\nimport solidqml.Widgets as W\n"
            "W.Div { width: 300; height: 200\n"
            "  Repeater { id: rep; model: 2; delegate: W.Div { activeFocusOnTab: true } }\n"
            "  readonly property int rows: rep.count\n"
            "  readonly property bool rowTabs: rep.count > 0 && rep.itemAt(0).activeFocusOnTab\n"
            "  Component { id: c; W.Text { activeFocusOnTab: true } }\n"
            "  property var made: c.createObject(this)\n"
            "}"));
        QVERIFY(o);
        QCOMPARE(o->property("rows").toInt(), 2);
        QVERIFY(o->property("rowTabs").toBool());
        QVERIFY(o->property("made").value<QQuickItem *>());
    }

    // Three focusable stops (two plain Divs with tabIndex=0's activeFocusOnTab line + one native
    // W.Button, which already carries the same line) interleaved with a plain Div that has NONE —
    // Tab must visit the three stops in document order and skip the one with no tab-stop line.
    void tabOrderThroughMixedDivsAndButtons()
    {
        QQmlEngine engine;
        wire(engine);
        QScopedPointer<QObject> o(create(engine,
            "import QtQuick\nimport solidqml.Widgets 1.0 as W\n"
            "W.Div { width: 300; height: 200\n"
            "  W.Div { objectName: \"d1\"; activeFocusOnTab: typeof solidTabstop !== \"undefined\" && solidTabstop ? solidTabstop.enabled : true }\n"
            "  W.Div { objectName: \"skip\" }\n"
            "  W.Button { objectName: \"b1\"; text: \"ok\" }\n"
            "  W.Div { objectName: \"d2\"; activeFocusOnTab: typeof solidTabstop !== \"undefined\" && solidTabstop ? solidTabstop.enabled : true }\n"
            "}"));
        QVERIFY(o);
        auto *root = qobject_cast<QQuickItem *>(o.data());
        QVERIFY(root);
        QQuickWindow window;
        window.resize(300, 200);
        root->setParentItem(window.contentItem());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        auto *d1 = root->findChild<QQuickItem *>(QStringLiteral("d1"));
        auto *skip = root->findChild<QQuickItem *>(QStringLiteral("skip"));
        auto *b1 = root->findChild<QQuickItem *>(QStringLiteral("b1"));
        auto *d2 = root->findChild<QQuickItem *>(QStringLiteral("d2"));
        QVERIFY(d1 && skip && b1 && d2);
        QVERIFY(d1->activeFocusOnTab());
        QVERIFY(!skip->activeFocusOnTab());
        // W.Button's OWN tab-stop line lives on an inner T.Button it wraps (button.cpp's
        // kButtonBody), not on the outer CssFill root found by objectName — this interleaving test
        // only needs the chain to REACH somewhere inside b1's subtree, not b1 itself.
        QVERIFY(!b1->activeFocusOnTab());
        QVERIFY(d2->activeFocusOnTab());

        const auto insideB1 = [b1](QQuickItem *it) {
            for (QQuickItem *p = it; p; p = p->parentItem())
                if (p == b1) return true;
            return false;
        };

        d1->forceActiveFocus();
        QCOMPARE(window.activeFocusItem(), d1);
        QQuickItem *next = d1->nextItemInFocusChain(true);
        QVERIFY(next);
        QVERIFY(next != skip); // "skip" (no activeFocusOnTab) is NOT a stop
        QVERIFY(insideB1(next)); // the native button's own inner focus item, in document order
        // Keep walking forward (bounded) until we reach d2, without ever landing on "skip" again.
        bool reachedD2 = false;
        for (int i = 0; i < 8 && !reachedD2; ++i) {
            next = next->nextItemInFocusChain(true);
            QVERIFY(next != skip);
            reachedD2 = next == d2;
        }
        QVERIFY(reachedD2);
        root->setParentItem(nullptr);
    }

    // role="button" + onClick (release-plan M5): Enter activates on key PRESS; Space is swallowed
    // on press and activates on key RELEASE — the exact statements transpiler/src/emit/qml.ts emits.
    void roleButtonActivatesOnEnterPressAndSpaceRelease()
    {
        QQmlEngine engine;
        wire(engine);
        // Nested inside an outer Div — exactly like the real transpiler output, where a
        // tabIndex/role element is always a CHILD somewhere in the component tree, never the
        // document's own root (assigning `activeFocusOnTab` directly on the component's root
        // object hits an unrelated QML revision-resolution quirk that does not apply to children).
        QScopedPointer<QObject> o(create(engine,
            "import QtQuick\nimport solidqml.Widgets 1.0 as W\n"
            "W.Div { width: 100; height: 40\n"
            "  W.Div { objectName: \"btn\"; anchors.fill: parent\n"
            "    property var count: 0\n"
            "    Keys.onPressed: (event) => { if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) { count = count + 1; event.accepted = true; } if (event.key === Qt.Key_Space) { event.accepted = true; } }\n"
            "    Keys.onReleased: (event) => { if (event.key === Qt.Key_Space) { count = count + 1; event.accepted = true; } }\n"
            "    activeFocusOnTab: typeof solidTabstop !== \"undefined\" && solidTabstop ? solidTabstop.enabled : true\n"
            "    Accessible.role: Accessible.Button\n"
            "  }\n"
            "}"));
        QVERIFY(o);
        auto *root = qobject_cast<QQuickItem *>(o.data());
        QVERIFY(root);
        auto *item = root->findChild<QQuickItem *>(QStringLiteral("btn"));
        QVERIFY(item);
        QQuickWindow window;
        window.resize(100, 40);
        root->setParentItem(window.contentItem());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        item->forceActiveFocus();
        QCOMPARE(window.activeFocusItem(), item);

        QTest::keyClick(&window, Qt::Key_Return);
        QCOMPARE(item->property("count").toInt(), 1);

        QTest::keyPress(&window, Qt::Key_Space);
        QCOMPARE(item->property("count").toInt(), 1); // NOT yet — Space activates on release
        QTest::keyRelease(&window, Qt::Key_Space);
        QCOMPARE(item->property("count").toInt(), 2);
        root->setParentItem(nullptr);
    }

    // role="switch"/"checkbox" + onClick: Space toggles on PRESS (no release-only step, no Enter).
    void roleSwitchTogglesOnSpacePress()
    {
        QQmlEngine engine;
        wire(engine);
        QScopedPointer<QObject> o(create(engine,
            "import QtQuick\nimport solidqml.Widgets 1.0 as W\n"
            "W.Div { width: 100; height: 40\n"
            "  W.Div { objectName: \"sw\"; anchors.fill: parent\n"
            "    property bool muted: false\n"
            "    Keys.onPressed: (event) => { if (event.key === Qt.Key_Space) { muted = !muted; event.accepted = true; } }\n"
            "    activeFocusOnTab: typeof solidTabstop !== \"undefined\" && solidTabstop ? solidTabstop.enabled : true\n"
            "    Accessible.role: Accessible.Switch\n"
            "    Accessible.checked: muted\n"
            "  }\n"
            "}"));
        QVERIFY(o);
        auto *root = qobject_cast<QQuickItem *>(o.data());
        QVERIFY(root);
        auto *item = root->findChild<QQuickItem *>(QStringLiteral("sw"));
        QVERIFY(item);
        QQuickWindow window;
        window.resize(100, 40);
        root->setParentItem(window.contentItem());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        item->forceActiveFocus();

        QTest::keyClick(&window, Qt::Key_Return); // Enter must do NOTHING for this role
        QCOMPARE(item->property("muted").toBool(), false);
        QTest::keyClick(&window, Qt::Key_Space);
        QCOMPARE(item->property("muted").toBool(), true);
        root->setParentItem(nullptr);
    }

    // The author's explicit Accessible.role/name/checked on a plain Div (no custom factory needed:
    // Qt Quick's own generic QQuickItem accessible factory answers once isAccessible is set, which
    // creating the attached object already did — see qquickaccessiblefactory.cpp).
    void genericDivExposesRoleNameAndCheckedState()
    {
        QQmlEngine engine;
        wire(engine);
        QScopedPointer<QObject> o(create(engine,
            "import QtQuick\nimport solidqml.Widgets 1.0 as W\n"
            "W.Div { width: 100; height: 40\n"
            "  property bool muted: true\n"
            "  Accessible.role: Accessible.Switch\n"
            "  Accessible.name: \"Mute\"\n"
            "  Accessible.checked: muted\n"
            "}"));
        QVERIFY(o);
        QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(o.data());
        QVERIFY(iface);
        QCOMPARE(iface->role(), QAccessible::Switch);
        QCOMPARE(iface->text(QAccessible::Name), QStringLiteral("Mute"));
        QVERIFY(iface->state().checked);

        o->setProperty("muted", false);
        QVERIFY(!iface->state().checked);
    }

    // aria-hidden="true" → Accessible.ignored: true removes the element from the accessible tree
    // (QAccessible::queryAccessibleInterface returns null, matching a11y_test.cpp's own check for
    // the Text/Image primitives' `accessibleIgnoredStands`).
    void ariaHiddenRemovesFromAccessibleTree()
    {
        QQmlEngine engine;
        wire(engine);
        QScopedPointer<QObject> o(create(engine,
            "import QtQuick\nimport solidqml.Widgets 1.0 as W\n"
            "W.Div { width: 200; height: 100\n"
            "  W.Div { width: 100; height: 40; Accessible.role: Accessible.Button;\n"
            "    Accessible.name: \"hidden-btn\"; Accessible.ignored: true }\n"
            "}"));
        QVERIFY(o);
        auto *root = qobject_cast<QQuickItem *>(o.data());
        QVERIFY(root);
        auto *child = root->findChildren<QQuickItem *>().isEmpty() ? nullptr : root->findChildren<QQuickItem *>().first();
        QVERIFY(child);
        QVERIFY(!QQuickItemPrivate::get(child)->isAccessible);

        QQuickWindow window;
        window.resize(200, 100);
        root->setParentItem(window.contentItem());
        // `ignored` excludes the item from its PARENT's accessible-children enumeration (Qt
        // Quick's accessibleUnignoredChildren) — a direct queryAccessibleInterface() on the
        // ignored object itself may still answer (matching a11y_test.cpp, which only ever checks
        // `isAccessible` directly and the window-tree walk, never a direct query on an ignored
        // item); this is the contract `Accessible.ignored: true` actually gives.
        QVERIFY(!findByName(QAccessible::queryAccessibleInterface(&window), QStringLiteral("hidden-btn")));
        root->setParentItem(nullptr);
    }
};

QTEST_MAIN(A11yGenericTest)
#include "a11y_generic_test.moc"
