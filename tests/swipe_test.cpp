// <Swipe>: a row pushed sideways. A click still reaches the child under it; a sideways drag (mouse)
// or a two-finger sideways scroll (touchpad: wheel events with pixel deltas) slides the row, the
// box's state names the action it is past, and letting go past a threshold fires swiped(action);
// short of it, or a vertical move, nothing happens.
#include "qmlcss/QMLCss.h"
#include "widgets/solidwidgets.h"

#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScopedPointer>
#include <QTest>
#include <QWheelEvent>

class SwipeTest : public QObject {
    Q_OBJECT

private:
    QmlCss::CssTheme m_theme;
    QmlCss::CssLayoutEngine m_layout { &m_theme };
    QQmlEngine *m_engine = nullptr;
    QScopedPointer<QObject> m_root;
    QQuickWindow *m_window = nullptr;
    QQuickItem *m_swipe = nullptr;

    void drag(QPoint from, QPoint by)
    {
        QTest::mousePress(m_window, Qt::LeftButton, {}, from);
        const int steps = 12;
        for (int k = 1; k <= steps; ++k)
            QTest::mouseMove(m_window, from + by * k / steps);
    }

    void wheel(QPoint delta, Qt::ScrollPhase phase)
    {
        const QPointF at(200, 30);
        QWheelEvent ev(at, m_window->mapToGlobal(at), delta, delta * 2, Qt::NoButton, Qt::NoModifier, phase, false);
        QCoreApplication::sendEvent(m_window, &ev);
    }

    QString state() const
    {
        const QVariantList s = m_swipe->property("cssState").toList();
        return s.isEmpty() ? QString() : s.first().toString();
    }

private slots:
    void initTestCase()
    {
        QmlCss::registerTypes();
        SolidWidgets::registerTypes();
        m_engine = new QQmlEngine(this);
        m_engine->rootContext()->setContextProperty(QStringLiteral("cssTheme"), &m_theme);
        m_engine->rootContext()->setContextProperty(QStringLiteral("cssLayout"), &m_layout);
        QQmlComponent c(m_engine);
        c.setData(R"(import QtQuick
import solidqml.Widgets as W
Window {
    width: 400; height: 100; visible: true
    property int clicks: 0
    property string got: ""
    W.Swipe {
        objectName: "sw"
        width: 400; height: 60
        rightLabel: "Arquivar"; farRightLabel: "Lixo"; leftLabel: "Adiar"
        onSwiped: (a) => got = a
        MouseArea { width: 400; height: 60; onClicked: clicks++ }
    }
}
)", QUrl());
        m_root.reset(c.create());
        QVERIFY2(m_root, qPrintable(c.errorString()));
        m_window = qobject_cast<QQuickWindow *>(m_root.data());
        QVERIFY(m_window);
        QVERIFY(QTest::qWaitForWindowExposed(m_window));
        m_swipe = m_window->findChild<QQuickItem *>(QStringLiteral("sw"));
        QVERIFY(m_swipe);
    }

    void init()
    {
        m_root->setProperty("got", QString());
        QTRY_COMPARE(m_swipe->property("dx").toReal(), 0.0);
    }

    void aClickReachesTheChild()
    {
        const int before = m_root->property("clicks").toInt();
        QTest::mouseClick(m_window, Qt::LeftButton, {}, QPoint(100, 30));
        QCOMPARE(m_root->property("clicks").toInt(), before + 1);
        QCOMPARE(m_swipe->property("dx").toReal(), 0.0);
    }

    void aShortPushRightArchives()
    {
        const int before = m_root->property("clicks").toInt();
        drag(QPoint(50, 30), QPoint(100, 2));
        QVERIFY(m_swipe->property("dx").toReal() > 90);
        QCOMPARE(m_swipe->property("action").toString(), QStringLiteral("right"));
        QCOMPARE(state(), QStringLiteral("right"));
        QCOMPARE(m_swipe->property("hint").toString(), QStringLiteral("Arquivar"));
        QTest::mouseRelease(m_window, Qt::LeftButton, {}, QPoint(150, 32));
        QTRY_COMPARE(m_root->property("got").toString(), QStringLiteral("right"));
        QCOMPARE(m_root->property("clicks").toInt(), before);   // the drag was no click
    }

    void aLongPushRightGoesFurther()
    {
        drag(QPoint(20, 30), QPoint(280, 0));
        QCOMPARE(state(), QStringLiteral("far-right"));
        QCOMPARE(m_swipe->property("hint").toString(), QStringLiteral("Lixo"));
        QTest::mouseRelease(m_window, Qt::LeftButton, {}, QPoint(300, 30));
        QTRY_COMPARE(m_root->property("got").toString(), QStringLiteral("far-right"));
    }

    void aPushLeftWithoutFarLabelStaysNear()
    {
        drag(QPoint(380, 30), QPoint(-300, 0));
        QCOMPARE(state(), QStringLiteral("left"));
        QTest::mouseRelease(m_window, Qt::LeftButton, {}, QPoint(80, 30));
        QTRY_COMPARE(m_root->property("got").toString(), QStringLiteral("left"));
    }

    void shortOfTheThresholdSpringsBack()
    {
        drag(QPoint(50, 30), QPoint(30, 0));
        QCOMPARE(state(), QString());
        QTest::mouseRelease(m_window, Qt::LeftButton, {}, QPoint(80, 30));
        QTRY_COMPARE(m_swipe->property("dx").toReal(), 0.0);
        QTest::qWait(250);
        QCOMPARE(m_root->property("got").toString(), QString());
    }

    void aVerticalMoveIsNotASwipe()
    {
        drag(QPoint(100, 10), QPoint(8, 45));
        QCOMPARE(m_swipe->property("dx").toReal(), 0.0);
        QTest::mouseRelease(m_window, Qt::LeftButton, {}, QPoint(108, 55));
    }

    void aTouchpadSwipeLeftSnoozes()
    {
        wheel(QPoint(-20, 0), Qt::ScrollBegin);
        for (int k = 0; k < 5; ++k)
            wheel(QPoint(-20, 1), Qt::ScrollUpdate);
        QCOMPARE(state(), QStringLiteral("left"));
        wheel(QPoint(0, 0), Qt::ScrollEnd);
        QTRY_COMPARE(m_root->property("got").toString(), QStringLiteral("left"));
    }

    void aTouchpadWithoutPhasesEndsOnAPause()
    {
        for (int k = 0; k < 6; ++k)
            wheel(QPoint(20, 0), Qt::NoScrollPhase);
        QCOMPARE(state(), QStringLiteral("right"));
        QTRY_COMPARE(m_root->property("got").toString(), QStringLiteral("right"));
    }

    void aPressDuringTheSlideOutDoesNotCancelIt()
    {
        drag(QPoint(50, 30), QPoint(100, 0));
        QTest::mouseRelease(m_window, Qt::LeftButton, {}, QPoint(150, 30));
        QTest::mouseClick(m_window, Qt::LeftButton, {}, QPoint(300, 30));   // at once, mid-slide
        QTRY_COMPARE(m_root->property("got").toString(), QStringLiteral("right"));
        QTRY_COMPARE(m_swipe->property("dx").toReal(), 0.0);   // nothing took it away: it comes back
    }

    void aClickOnARowLeftSlidOutPutsItBack()
    {
        drag(QPoint(50, 30), QPoint(100, 0));
        QTest::mouseRelease(m_window, Qt::LeftButton, {}, QPoint(150, 30));
        QTRY_COMPARE(m_root->property("got").toString(), QStringLiteral("right"));
        m_root->setProperty("got", QString());
        QTest::mousePress(m_window, Qt::LeftButton, {}, QPoint(380, 30));
        QVERIFY(m_swipe->property("dx").toReal() != 0);   // held where it is
        QTest::mouseRelease(m_window, Qt::LeftButton, {}, QPoint(380, 30));
        QTRY_COMPARE(m_swipe->property("dx").toReal(), 0.0);
        QCOMPARE(m_root->property("got").toString(), QString());   // no second action
    }

    void aLostGrabPutsTheRowBack()
    {
        drag(QPoint(50, 30), QPoint(100, 0));
        QVERIFY(m_swipe->property("dx").toReal() > 90);
        m_swipe->ungrabMouse();
        QTRY_COMPARE(m_swipe->property("dx").toReal(), 0.0);
        QTest::mouseRelease(m_window, Qt::LeftButton, {}, QPoint(150, 30));
        QTest::qWait(250);
        QCOMPARE(m_root->property("got").toString(), QString());
    }

    void insideAFlickableSwipesAndScrolls()
    {
        QQmlComponent c(m_engine);
        c.setData(R"(import QtQuick
import solidqml.Widgets as W
Window {
    width: 400; height: 100; visible: true
    property string got: ""
    Flickable {
        objectName: "fl"
        anchors.fill: parent
        contentHeight: 1000
        flickableDirection: Flickable.VerticalFlick
        W.Swipe {
            objectName: "sw2"
            width: 400; height: 60
            rightLabel: "Arquivar"
            onSwiped: (a) => got = a
            MouseArea { width: 400; height: 60 }
        }
    }
}
)", QUrl());
        QScopedPointer<QObject> root(c.create());
        QVERIFY2(root, qPrintable(c.errorString()));
        auto *w = qobject_cast<QQuickWindow *>(root.data());
        QVERIFY(QTest::qWaitForWindowExposed(w));
        auto *sw = w->findChild<QQuickItem *>(QStringLiteral("sw2"));
        auto *fl = w->findChild<QQuickItem *>(QStringLiteral("fl"));
        QVERIFY(sw && fl);
        // Sideways: the row's.
        QTest::mousePress(w, Qt::LeftButton, {}, QPoint(40, 30));
        for (int k = 1; k <= 12; ++k) { QTest::qWait(10); QTest::mouseMove(w, QPoint(40 + k * 10, 30)); }
        QVERIFY(sw->property("dx").toReal() > 90);
        QTest::mouseRelease(w, Qt::LeftButton, {}, QPoint(160, 30));
        QTRY_COMPARE(root->property("got").toString(), QStringLiteral("right"));
        QTRY_COMPARE(sw->property("dx").toReal(), 0.0);
        // Up and down: the Flickable's.
        QTest::mousePress(w, Qt::LeftButton, {}, QPoint(100, 80));
        for (int k = 1; k <= 12; ++k) { QTest::qWait(10); QTest::mouseMove(w, QPoint(102, 80 - k * 6)); }
        QTest::mouseRelease(w, Qt::LeftButton, {}, QPoint(102, 8));
        QCOMPARE(sw->property("dx").toReal(), 0.0);
        QTRY_VERIFY(fl->property("contentY").toReal() > 0);
    }

    void aVerticalScrollIsTheLists()
    {
        const QPointF at(200, 30);
        QWheelEvent ev(at, m_window->mapToGlobal(at), QPoint(2, -40), QPoint(4, -80), Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false);
        QCoreApplication::sendEvent(m_window, &ev);
        QVERIFY(!ev.isAccepted());
        QCOMPARE(m_swipe->property("dx").toReal(), 0.0);
        wheel(QPoint(0, 0), Qt::ScrollEnd);
    }
};

QTEST_MAIN(SwipeTest)
#include "swipe_test.moc"
