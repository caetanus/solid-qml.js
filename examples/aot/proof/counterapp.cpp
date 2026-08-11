#include "counterapp.h"

#include "aot/sqbuild.h"
#include "aot/sqruntime.h"

#include "qmlcss/cssrect.h"
#include "widgets/button.h"
#include "widgets/primitives.h" // SolidWidgets::Div

#include <QQmlContext>

namespace aot {

// Counter.qml:
//   W.Div { cssClass:["counter"]
//     W.Button { text: "" + (label) + ": " + (count); onClicked: count = count + 1 } }
QQuickItem *buildCounter(QQmlContext *ctx, CounterState *state)
{
    auto *root = new SolidWidgets::Div();
    sq::begin(root, ctx);
    root->setCssClass(sq::classes({ "counter" }));

    auto *btn = new SolidWidgets::Button();
    sq::begin(btn, ctx);

    // onClicked: count = count + 1
    QObject::connect(btn, &SolidWidgets::Button::clicked, state, [state] {
        state->setCount(sq::add(state->count(), QVariant(1)));
    });

    // text: "" + (label) + ": " + (count) — a connect-driven binding over the deps (label, count).
    auto update = [btn, state] {
        btn->setText(sq::str(sq::add(
            sq::add(sq::add(QVariant(QString()), state->label()), QVariant(QStringLiteral(": "))),
            state->count())));
    };
    QObject::connect(state, &CounterState::labelChanged, btn, update);
    QObject::connect(state, &CounterState::countChanged, btn, update);

    sq::complete(btn);
    update(); // initial evaluation, after the label CssText exists

    sq::append(root, btn);
    sq::complete(root);
    return root;
}

// App.generated.qml (content): Css.CssRect{cssPrimitive:"window"} > W.Div{.app} > Counter × 3.
QQuickItem *buildCounterApp(QQmlContext *ctx)
{
    auto *winBox = new QmlCss::CssRect();
    sq::begin(winBox, ctx);
    winBox->setCssPrimitive(QStringLiteral("window"));
    winBox->setCssClass(sq::classes({ "qml-window" }));

    auto *appDiv = new SolidWidgets::Div();
    sq::begin(appDiv, ctx);
    appDiv->setCssClass(sq::classes({ "app" }));

    for (const char *label : { "A", "B", "C" }) {
        auto *state = new CounterState(appDiv); // lifetime tied to the app subtree
        state->setLabel(QString::fromUtf8(label));
        sq::append(appDiv, buildCounter(ctx, state));
    }

    sq::complete(appDiv);
    sq::append(winBox, appDiv);
    sq::complete(winBox);
    return winBox;
}

} // namespace aot
