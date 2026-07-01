#include "test_css.h"

#include "qmlcss/csstheme.h"

#include <QEasingCurve>
#include <QJSEngine>
#include <QJSValue>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTest>

void CssThemeTests::parsesIdClassAndSourceOrder()
{
    CssTheme theme;
    QSignalSpy loadedSpy(&theme, &CssTheme::loadedChanged);

    theme.loadFromString(QStringLiteral(R"(
        * { color: #eeeeee; }
        #battery { background-color: #222222; color: #ffffff; }
        #battery.charging { background-color: #218f4f; }
        #battery { color: #101010; }
    )"));

    QVERIFY(theme.isLoaded());
    QCOMPARE(loadedSpy.count(), 1);

    const QVariantMap base = theme.resolve(QStringLiteral("battery"));
    QCOMPARE(base.value(QStringLiteral("background-color")).toString(), QStringLiteral("#222222"));
    QCOMPARE(base.value(QStringLiteral("color")).toString(), QStringLiteral("#101010"));

    const QVariantMap charging = theme.resolve(QStringLiteral("battery"), {QStringLiteral("charging")});
    QCOMPARE(charging.value(QStringLiteral("background-color")).toString(), QStringLiteral("#218f4f"));
    QCOMPARE(charging.value(QStringLiteral("color")).toString(), QStringLiteral("#101010"));

    // `transparent` and the normalized `#00000000` must both parse to a VALID
    // fully-transparent colour — an invalid QColor renders as opaque black, which is
    // the "inactive workspace → black" regression.
    QVERIFY(theme.parseColor(QStringLiteral("transparent")).isValid());
    QCOMPARE(theme.parseColor(QStringLiteral("transparent")).alpha(), 0);
    const QColor hex8 = theme.parseColor(QStringLiteral("#00000000"));
    QVERIFY2(hex8.isValid(), "QColor(\"#00000000\") is INVALID → renders black");
    QCOMPARE(hex8.alpha(), 0);
}

void CssThemeTests::resolvesDescendantSelectorsWithContext()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(R"(
        button { color: #111111; }
        #workspaces button { color: #eeeeee; padding-left: 2px; }
        #workspaces button.focused { color: #ffffff; background-color: #2f97d1; }
        #workspaces button.focused::after { background: #abcdef; }
        #tray button.focused { color: #ff00ff; }
    )"));

    const QVariantMap topLevel = theme.resolve(QStringLiteral("button"));
    QVERIFY(!topLevel.contains(QStringLiteral("color")));

    const QVariantMap workspaceButton = theme.resolveWith(QStringLiteral("workspaces"), QStringLiteral("button"));
    QCOMPARE(workspaceButton.value(QStringLiteral("color")).toString(), QStringLiteral("#eeeeee"));
    QCOMPARE(workspaceButton.value(QStringLiteral("padding-left")).toString(), QStringLiteral("2px"));

    const QVariantMap focusedWorkspaceButton = theme.resolveWith(QStringLiteral("workspaces"),
                                                                 QStringLiteral("button"),
                                                                 {QStringLiteral("focused")});
    QCOMPARE(focusedWorkspaceButton.value(QStringLiteral("color")).toString(), QStringLiteral("#ffffff"));
    QCOMPARE(focusedWorkspaceButton.value(QStringLiteral("background-color")).toString(), QStringLiteral("#2f97d1"));

    const QVariantMap trayButton = theme.resolveWith(QStringLiteral("tray"), QStringLiteral("button"), {QStringLiteral("focused")});
    QCOMPARE(trayButton.value(QStringLiteral("color")).toString(), QStringLiteral("#ff00ff"));

    // The active-fill overlay reads `#workspaces button.focused::after` via resolveWith
    // with a pseudo-element. If this returns empty, the QML falls back to a default fill
    // (the "inactive workspace flashes black" path).
    const QVariantMap afterOverlay = theme.resolveWith(QStringLiteral("workspaces"),
                                                       QStringLiteral("button"),
                                                       {QStringLiteral("focused")},
                                                       QStringLiteral("after"));
    QCOMPARE(afterOverlay.value(QStringLiteral("background")).toString(), QStringLiteral("#abcdef"));
    // And the ordinary (no-pseudo) resolve must NOT pick up the ::after rule.
    QVERIFY(!focusedWorkspaceButton.contains(QStringLiteral("background")));
}

void CssThemeTests::exactResolveIgnoresUniversalRules()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(R"(
        * { color: #eeeeee; }
        #clock { color: #ffffff; }
        #clock.warning { background-color: #ff0000; }
        #workspaces #clock { color: #00ff00; }
    )"));

    const QVariantMap exact = theme.resolveExact(QStringLiteral("clock"));
    QCOMPARE(exact.size(), 1);
    QCOMPARE(exact.value(QStringLiteral("color")).toString(), QStringLiteral("#ffffff"));

    const QVariantMap warning = theme.resolveExact(QStringLiteral("clock"), {QStringLiteral("warning")});
    QCOMPARE(warning.value(QStringLiteral("color")).toString(), QStringLiteral("#ffffff"));
    QCOMPARE(warning.value(QStringLiteral("background-color")).toString(), QStringLiteral("#ff0000"));
}

void CssThemeTests::stripsCommentsAndAtRules()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(R"(
        /* This should not create a selector. */
        @define-color accent #63b3ed;
        #cpu {
            color: @accent;
            graph-width: 22px;
        }
    )"));

    const QVariantMap cpu = theme.resolve(QStringLiteral("cpu"));
    // @define-color is expanded: the @accent reference resolves to its value
    // and the @define-color declaration itself is stripped.
    QCOMPARE(cpu.value(QStringLiteral("color")).toString(), QStringLiteral("#63b3ed"));
    QCOMPARE(cpu.value(QStringLiteral("graph-width")).toString(), QStringLiteral("22px"));
}

void CssThemeTests::parsesColors()
{
    CssTheme theme;

    QCOMPARE(theme.parseColor(QStringLiteral("transparent")), QColor(0, 0, 0, 0));
    QCOMPARE(theme.parseColor(QStringLiteral("#112233")), QColor(QStringLiteral("#112233")));
    QCOMPARE(theme.parseColor(QStringLiteral("rgb(1, 2, 3)")), QColor(1, 2, 3));

    const QColor rgba = theme.parseColor(QStringLiteral("rgba(10, 20, 30, 0.5)"));
    QCOMPARE(rgba.red(), 10);
    QCOMPARE(rgba.green(), 20);
    QCOMPARE(rgba.blue(), 30);
    QVERIFY(rgba.alpha() >= 127 && rgba.alpha() <= 128);
}

void CssThemeTests::parsesCssDurationsAndEasings()
{
    CssTheme theme;

    QCOMPARE(theme.parseDuration(QStringLiteral("180ms"), 1), 180);
    QCOMPARE(theme.parseDuration(QStringLiteral("0.76s"), 1), 760);
    QCOMPARE(theme.parseDuration(QStringLiteral("240"), 1), 240);
    QCOMPARE(theme.parseDuration(QStringLiteral("nope"), 321), 321);

    QCOMPARE(theme.parseEasing(QStringLiteral("linear"), static_cast<int>(QEasingCurve::InQuad)),
             static_cast<int>(QEasingCurve::Linear));
    QCOMPARE(theme.parseEasing(QStringLiteral("ease-in-out"), static_cast<int>(QEasingCurve::Linear)),
             static_cast<int>(QEasingCurve::InOutQuad));
    QCOMPARE(theme.parseEasing(QStringLiteral("out-cubic"), static_cast<int>(QEasingCurve::Linear)),
             static_cast<int>(QEasingCurve::OutCubic));
    QCOMPARE(theme.parseEasing(QStringLiteral("unknown"), static_cast<int>(QEasingCurve::OutBack)),
             static_cast<int>(QEasingCurve::OutBack));

    // Standard CSS `transition` shorthand: property + duration + timing-function,
    // in any order; the first <time> is the duration, the second the delay.
    const QVariantMap t = theme.parseTransition(QStringLiteral("background 180ms ease-in-out"));
    QCOMPARE(t.value(QStringLiteral("property")).toString(), QStringLiteral("background"));
    QCOMPARE(t.value(QStringLiteral("duration")).toInt(), 180);
    QCOMPARE(t.value(QStringLiteral("easing")).toInt(), static_cast<int>(QEasingCurve::InOutQuad));

    const QVariantMap t2 = theme.parseTransition(QStringLiteral("color 0.2s 50ms linear"));
    QCOMPARE(t2.value(QStringLiteral("property")).toString(), QStringLiteral("color"));
    QCOMPARE(t2.value(QStringLiteral("duration")).toInt(), 200);
    QCOMPARE(t2.value(QStringLiteral("delay")).toInt(), 50);
    QCOMPARE(t2.value(QStringLiteral("easing")).toInt(), static_cast<int>(QEasingCurve::Linear));

    // Only a duration → property defaults to "all".
    const QVariantMap t3 = theme.parseTransition(QStringLiteral("120ms"));
    QCOMPARE(t3.value(QStringLiteral("property")).toString(), QStringLiteral("all"));
    QCOMPARE(t3.value(QStringLiteral("duration")).toInt(), 120);
}

void CssThemeTests::loadCssAppliesReappliesAndPrunes()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(
        "#clock { background-color: #ff0000; }"
        "#waybar { color: #00ff00; background-color: #0000aa; }"));

    CssTargetStub stub;
    stub.m_cssId = QStringLiteral("clock");
    stub.m_cssAlternateId = QStringLiteral("waybar"); // waybar drop-in alias

    theme.loadCss(&stub);
    // Primary #clock wins background-color; the alias #waybar contributes `color`.
    QCOMPARE(stub.m_style.value(QStringLiteral("background-color")).toString(), QStringLiteral("#ff0000"));
    QCOMPARE(stub.m_style.value(QStringLiteral("color")).toString(), QStringLiteral("#00ff00"));

    // Reverse slot: reloading the theme re-pushes to the registered stub with no re-call.
    theme.loadFromString(QStringLiteral("#clock { background-color: #0000ff; }"));
    QCOMPARE(stub.m_style.value(QStringLiteral("background-color")).toString(), QStringLiteral("#0000ff"));

    // Registered cssClass: a state change auto-restyles via the observed NOTIFY — the
    // applet registers once and never re-calls loadCss.
    theme.loadFromString(QStringLiteral(
        "#clock { background-color: #111111; }"
        "#clock.focused { background-color: #222222; }"));
    QCOMPARE(stub.m_style.value(QStringLiteral("background-color")).toString(), QStringLiteral("#111111"));
    stub.setProperty("cssClass", QStringLiteral("focused")); // emits cssClassChanged → engine re-applies
    QCOMPARE(stub.m_style.value(QStringLiteral("background-color")).toString(), QStringLiteral("#222222"));

    // A part target (`#tray.item`) resolves ONLY the part, NOT the bare `#tray` base —
    // so a sub-element doesn't inherit the container's own background. Isolated engine so
    // it doesn't disturb the #clock assertions above/below.
    {
        CssTheme partTheme;
        partTheme.loadFromString(QStringLiteral(
            "#tray { background-color: #aaaaaa; }"
            "#tray.item { background-color: #bbbbbb; }"));
        CssTargetStub part;
        part.m_cssId = QStringLiteral("tray");
        part.m_cssPart = QStringLiteral("item");
        partTheme.loadCss(&part);
        QCOMPARE(part.m_style.value(QStringLiteral("background-color")).toString(), QStringLiteral("#bbbbbb"));
    }

    // A non-conforming target (no cssId/style) is refused LOUDLY, not silently.
    NonCssStub bad;
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("lacks the CssQmlItem signature")));
    theme.loadCss(&bad);

    // Pruning: a destroyed target drops out of the registry; a later reload must not crash.
    {
        CssTargetStub *temp = new CssTargetStub;
        temp->m_cssId = QStringLiteral("clock");
        theme.loadCss(temp);
        QCOMPARE(temp->m_style.value(QStringLiteral("background-color")).toString(), QStringLiteral("#111111"));
        delete temp; // destroyed() prunes the registration
    }
    theme.loadFromString(QStringLiteral("#clock { background-color: #112233; }")); // must not touch a dead ref
    QCOMPARE(stub.m_style.value(QStringLiteral("background-color")).toString(), QStringLiteral("#112233"));
}

void CssThemeTests::loadCssAppliesClassOnlyTargets()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(
        ".panel { background-color: #123456; color: #abcdef; }"));

    CssTargetStub stub;
    stub.m_cssClass = QStringLiteral("panel");

    theme.loadCss(&stub);
    QCOMPARE(stub.m_style.value(QStringLiteral("background-color")).toString(), QStringLiteral("#123456"));
    QCOMPARE(stub.m_style.value(QStringLiteral("color")).toString(), QStringLiteral("#abcdef"));
}

void CssThemeTests::parsesKeyframesAndAnimation()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(
        "@keyframes blink { 0% { opacity: 0; } 50% { opacity: 1; } 100% { opacity: 0; } }"
        "#workspaces button.urgent::before { background: #ff0000; animation: blink 760ms ease-in-out infinite; }"));

    // @keyframes extracted + stored, sorted by offset.
    const QVariantList frames = theme.keyframes(QStringLiteral("blink"));
    QCOMPARE(frames.size(), 3);
    QCOMPARE(frames.at(0).toMap().value(QStringLiteral("offset")).toDouble(), 0.0);
    QCOMPARE(frames.at(1).toMap().value(QStringLiteral("offset")).toDouble(), 0.5);
    QCOMPARE(frames.at(2).toMap().value(QStringLiteral("offset")).toDouble(), 1.0);
    QCOMPARE(frames.at(1).toMap().value(QStringLiteral("properties")).toMap().value(QStringLiteral("opacity")).toString(),
             QStringLiteral("1"));

    // The @keyframes block must NOT leak into the ordinary rules — the ::before rule that
    // followed it still resolves, carrying its `animation` declaration.
    const QVariantMap before = theme.resolveWith(QStringLiteral("workspaces"), QStringLiteral("button"),
                                                 {QStringLiteral("urgent")}, QStringLiteral("before"));
    QVERIFY(!before.value(QStringLiteral("animation")).toString().isEmpty());

    // animation shorthand (order-independent; infinite → -1).
    const QVariantMap anim = theme.parseAnimation(QStringLiteral("blink 760ms ease-in-out infinite"));
    QCOMPARE(anim.value(QStringLiteral("name")).toString(), QStringLiteral("blink"));
    QCOMPARE(anim.value(QStringLiteral("duration")).toInt(), 760);
    QCOMPARE(anim.value(QStringLiteral("easing")).toInt(), static_cast<int>(QEasingCurve::InOutQuad));
    QCOMPARE(anim.value(QStringLiteral("iterations")).toInt(), -1);

    const QVariantMap anim2 = theme.parseAnimation(QStringLiteral("pulse 2s linear 3 alternate"));
    QCOMPARE(anim2.value(QStringLiteral("name")).toString(), QStringLiteral("pulse"));
    QCOMPARE(anim2.value(QStringLiteral("duration")).toInt(), 2000);
    QCOMPARE(anim2.value(QStringLiteral("iterations")).toInt(), 3);
    QCOMPARE(anim2.value(QStringLiteral("direction")).toString(), QStringLiteral("alternate"));
}

void CssThemeTests::appliesArrayCssClassFromQJSValue()
{
    // QML declares state as `cssClass: active ? ["active"] : []`. Read back via QObject::property
    // that array arrives as a QJSValue (NOT a QVariantList), so the engine must unwrap it —
    // otherwise every state class is silently dropped and only the base rule ever applies.
    CssTheme theme;
    theme.loadFromString(QStringLiteral(
        "#caffeine { background-color: #000000; }"
        "#caffeine.active { background-color: #ffffff; }"));

    CssTargetStub stub;
    stub.m_cssId = QStringLiteral("caffeine");
    theme.loadCss(&stub);
    QCOMPARE(stub.m_style.value(QStringLiteral("background-color")).toString(), QStringLiteral("#000000"));

    // A real QJSValue array, exactly as a QML `var` property yields.
    QJSEngine engine;
    const QJSValue jsArray = engine.evaluate(QStringLiteral("['active']"));
    QVERIFY(jsArray.isArray());
    stub.setProperty("cssClass", QVariant::fromValue(jsArray)); // emits NOTIFY → reverse-slot reapply
    QCOMPARE(stub.m_style.value(QStringLiteral("background-color")).toString(), QStringLiteral("#ffffff"));
}

QTEST_GUILESS_MAIN(CssThemeTests)
