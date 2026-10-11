// SolidWidgets::Select — the <select>/<option> T.ComboBox wrapper (src/widgets/select.{h,cpp}).
// Covers the three Q_PROPERTYs the transpiler's dynamic <select> plan relies on (model, values,
// disabledFlags): they default to an empty list (so a <select> that never sets disabledFlags —
// every one before that plan — behaves exactly as before), round-trip through the getter/setter
// with the expected NOTIFY signal, and — the actual mechanism a dynamic <select> depends on to
// "follow the list signal" — a `model: someContextProperty` QML binding re-evaluates when that
// context property changes (Select's Q_PROPERTY NOTIFY makes it a live binding target, not a
// one-shot copy).
#include "qmlcss/QMLCss.h"
#include "widgets/solidwidgets.h"

#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QScopedPointer>
#include <QSignalSpy>
#include <QTest>
#include <QVariantList>

class SelectTest : public QObject {
    Q_OBJECT

private:
    QmlCss::CssTheme m_theme;
    QmlCss::CssLayoutEngine m_layout { &m_theme };

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

    // Every <select> written before the dynamic-options plan never sets disabledFlags: the delegate
    // reads `ctl.disabledFlags[index]`, and an empty list there must stay falsy (no row disabled).
    void defaultsAreEmptyAndCurrentIndexIsZero()
    {
        QQmlEngine engine;
        wire(engine);
        QScopedPointer<QObject> o(create(engine, "import solidqml.Widgets\nSelect { model: [\"A\"]; values: [\"a\"] }"));
        QVERIFY(o);
        QCOMPARE(o->property("disabledFlags").value<QVariantList>(), QVariantList());
        // 0, not ComboBox's empty-model -1 (see select.h) — the controlled RestoreNone Binding the
        // transpiler emits pushes a real value, so a stray -1 would blank the box on first paint.
        QCOMPARE(o->property("currentIndex").toInt(), 0);
    }

    void modelValuesAndDisabledFlagsRoundTripWithNotify()
    {
        QQmlEngine engine;
        wire(engine);
        QScopedPointer<QObject> o(create(engine, "import solidqml.Widgets\nSelect {}"));
        QVERIFY(o);

        QSignalSpy modelSpy(o.data(), SIGNAL(modelChanged()));
        QSignalSpy valuesSpy(o.data(), SIGNAL(valuesChanged()));
        QSignalSpy disabledSpy(o.data(), SIGNAL(disabledFlagsChanged()));

        o->setProperty("model", QVariantList { QStringLiteral("Alpha"), QStringLiteral("Beta") });
        o->setProperty("values", QVariantList { QStringLiteral("a"), QStringLiteral("b") });
        o->setProperty("disabledFlags", QVariantList { false, true });

        QCOMPARE(o->property("model").value<QVariantList>(), QVariantList({ QStringLiteral("Alpha"), QStringLiteral("Beta") }));
        QCOMPARE(o->property("values").value<QVariantList>(), QVariantList({ QStringLiteral("a"), QStringLiteral("b") }));
        QCOMPARE(o->property("disabledFlags").value<QVariantList>(), QVariantList({ false, true }));
        QCOMPARE(modelSpy.count(), 1);
        QCOMPARE(valuesSpy.count(), 1);
        QCOMPARE(disabledSpy.count(), 1);

        // Setting the SAME value again is a no-op (the setters compare before assigning) — no
        // redundant NOTIFY, matching model()/values()'s existing contract.
        o->setProperty("disabledFlags", QVariantList { false, true });
        QCOMPARE(disabledSpy.count(), 1);
    }

    // The mechanism a dynamic <select> actually depends on: `model: someListProperty` is a QML
    // BINDING, not a one-shot assignment — it must re-evaluate when the bound list changes, exactly
    // like the transpiler's `model: (fonts).map(...)` Binding does when the `fonts` signal fires.
    void modelFollowsAReactiveContextProperty()
    {
        QQmlEngine engine;
        wire(engine);
        engine.rootContext()->setContextProperty(QStringLiteral("fontsCtx"), QVariantList { QStringLiteral("Arial") });
        QScopedPointer<QObject> o(create(engine, "import solidqml.Widgets\nSelect { model: fontsCtx; values: fontsCtx }"));
        QVERIFY(o);
        QCOMPARE(o->property("model").value<QVariantList>(), QVariantList({ QStringLiteral("Arial") }));

        engine.rootContext()->setContextProperty(QStringLiteral("fontsCtx"),
            QVariantList { QStringLiteral("Arial"), QStringLiteral("Times") });
        QCOMPARE(o->property("model").value<QVariantList>(),
            QVariantList({ QStringLiteral("Arial"), QStringLiteral("Times") }));
    }
};

QTEST_MAIN(SelectTest)
#include "select_test.moc"
