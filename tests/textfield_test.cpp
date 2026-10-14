// SolidWidgets::TextField — the <input> wrapper (src/widgets/textinputs.{h,cpp}). The transpiler
// emits `onInput={(e) => f(e.currentTarget.value)}` as `onTextEdited: { f(text) }`: the handler
// reads the wrapper's `text`. T.TextField emits textEdited BEFORE textChanged, so the wrapper
// must carry the new text before it relays textEdited — else every handler sees the text as it
// was one keystroke earlier, and the last key typed never reaches the app's state.
#include "qmlcss/QMLCss.h"
#include "widgets/solidwidgets.h"

#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScopedPointer>
#include <QTest>

class TextFieldTest : public QObject {
    Q_OBJECT

private:
    QmlCss::CssTheme m_theme;
    QmlCss::CssLayoutEngine m_layout { &m_theme };

private slots:
    void initTestCase()
    {
        QmlCss::registerTypes();
        SolidWidgets::registerTypes();
    }

    void textEditedSeesTheKeyJustTyped()
    {
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &m_theme);
        engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &m_layout);
        QQmlComponent component(&engine);
        component.setData("import QtQuick\nimport solidqml.Widgets\n"
                          "Window { width: 300; height: 60; visible: true\n"
                          "  property var seen: []\n"
                          "  TextField { objectName: \"f\"; width: 280; height: 40\n"
                          "    onTextEdited: seen = seen.concat([text]) } }", QUrl());
        QScopedPointer<QObject> o(component.create());
        QVERIFY2(o, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(o.data());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *field = o->findChild<QQuickItem *>(QStringLiteral("f"));
        QVERIFY(field);
        // The editor is the wrapper's inner T.TextField: the first child that takes text input.
        QQuickItem *editor = nullptr;
        const auto items = field->findChildren<QQuickItem *>();
        for (QQuickItem *i : items)
            if (i->flags() & QQuickItem::ItemAcceptsInputMethod) { editor = i; break; }
        QVERIFY(editor);
        editor->forceActiveFocus();
        QTRY_VERIFY(editor->hasActiveFocus());

        for (char c : { 'a', 'b', 'c' }) QTest::keyClick(window, c);
        const QStringList seen = o->property("seen").toStringList();
        QCOMPARE(seen, QStringList({ QStringLiteral("a"), QStringLiteral("ab"), QStringLiteral("abc") }));
        QCOMPARE(field->property("text").toString(), QStringLiteral("abc"));
    }
};

QTEST_MAIN(TextFieldTest)
#include "textfield_test.moc"
