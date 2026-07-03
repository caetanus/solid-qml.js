# Integration guide

This page walks through integrating `qml-css-engine` into an existing Qt 6 / Meson
project from first principle to a working styled UI.

## Prerequisites

- Qt 6 with `Core`, `Gui`, `Qml`, `Quick` (and `QtQuick.Effects` / `QtQuick.Shapes`
  available at runtime — they ship with Qt 6)
- Meson ≥ 0.63
- C++20

## Step 1 — Add the subproject

Place the `qml-css-engine` source tree inside your project's `subprojects/` directory:

```sh
# Option A: git submodule
git submodule add https://github.com/your-org/qml-css-engine subprojects/qml-css-engine

# Option B: meson wrap (if you maintain a wrap file)
# subprojects/qml-css-engine.wrap
```

In your `meson.build`, declare and consume the dependency:

```meson
qmlcss             = subproject('qml-css-engine')
qml_css_engine_dep = qmlcss.get_variable('qml_css_engine_dep')

executable(
  'my-app',
  sources,
  dependencies: [qt6_dep, qml_css_engine_dep],
)
```

`qml_css_engine_dep` packages everything: the static library, the include path
(`src/`), the Qt 6 dependency, and the compiled QML resources. No separate
`qresources:` or `install_data` is needed.

> **Note:** The build system compiles `qml/qmlcss.qrc` into the library. Consumers
> get the QML files automatically through the linked static library — no extra QRC
> registration step on the consumer side.

## Step 2 — Set up the C++ engine

Include the two headers and create the objects before loading the QML engine:

```cpp
#include "qmlcss/csstheme.h"
#include "qmlcss/csslayout.h"

// In your application startup, before engine.load():
QQmlApplicationEngine engine;

auto *cssTheme  = new CssTheme(&engine);          // parent = engine for lifetime
auto *cssLayout = new CssLayoutEngine(cssTheme, &engine);

engine.rootContext()->setContextProperty("cssTheme",  cssTheme);
engine.rootContext()->setContextProperty("cssLayout", cssLayout);
```

> **Context property names** — the QML components reference `cssTheme` and
> `cssLayout` as bare names. You must use exactly those names.

### Load the stylesheet

```cpp
// Single file:
cssTheme->load("/path/to/style.css");

// Multiple files cascaded (later files override earlier ones):
cssTheme->loadLayered({"/path/to/base.css", "/path/to/theme.css"});
```

Files are watched with `QFileSystemWatcher`. When any loaded file changes on disk,
`CssTheme` re-reads the whole layer and pushes updated styles to every registered
item automatically — hot-reload at no extra cost.

### Track the viewport (for `@media` queries)

If your CSS uses `@media (max-width: …)` or viewport units (`vw`/`vh`), call
`setViewport` when the window resizes:

```cpp
// In your root QML's onWidthChanged / onHeightChanged, or from C++:
cssTheme->setViewport(windowWidth, windowHeight);
```

From QML (simplest approach):

```qml
Window {
    id: win
    onWidthChanged:  cssTheme.setViewport(width, height)
    onHeightChanged: cssTheme.setViewport(width, height)
}
```

### Inject generated CSS (advanced)

If you have a build step that synthesises inline-style rules into CSS (e.g. from a
transpiler), append them as the highest-priority layer at startup:

```qml
// In your root QML component:
Component.onCompleted: cssTheme.loadLayeredString(generatedCss)
```

`loadLayeredString` stores the string and re-appends it after every file-change
reload, so it always wins the cascade without being overwritten.

## Step 3 — Import the QML components

The components are C++ types registered by ONE call — `QmlCss::registerTypes()` from `qmlcss/QMLCss.h` — under `import qmlcss 1.0`. Import them in any QML file:

```qml
import qmlcss 1.0 as Css
```

You can use any alias; `Css` is conventional.

## Step 4 — Write your first styled component

```qml
import QtQuick
import qmlcss 1.0 as Css

Css.CssRect {
    id: card
    cssId: "card"
    width: 240
    height: 80

    Css.CssText {
        cssId: "card-label"
        text: "Hello"
    }
}
```

```css
/* style.css */
#card {
    background: linear-gradient(135deg, #1a1a2e, #16213e);
    border-radius: 10px;
    padding: 16px;
    display: flex;
    align-items: center;
    gap: 8px;
}

#card-label {
    color: #e0e0e0;
    font-size: 14px;
    font-weight: bold;
}
```

## Step 5 — State classes

Assign `cssClass` (authored state) to select CSS rules:

```qml
Css.CssRect {
    cssId: "button"
    cssClass: pressed ? ["active"] : []
}
```

```css
#button { background-color: #333; }
#button.active { background-color: #555; }
```

`CssRect` also adds `"hover"` to `cssState` automatically via its built-in
`HoverHandler`, so `:hover` rules work without any extra code.

## Step 6 — Named parts

Use `cssPart` to style a sub-element of a module without inheriting the container's
own background:

```qml
Css.CssRect {
    cssId: "panel"
    /* … */

    Css.CssRect {
        cssId: "panel"
        cssPart: "graph"
    }
}
```

```css
#panel { background-color: #222; padding: 4px; }
#panel.graph { background-color: #444; height: 32px; }
```

The part element resolves only rules that explicitly carry `.graph`; it does not
inherit `#panel`'s own `background-color`.

## End-to-end minimal example

The following is a self-contained example that compiles:

**`meson.build`**

```meson
project('my-app', 'cpp', default_options: ['cpp_std=c++20'])

qt6 = import('qt6')
qt6_dep = dependency('qt6', modules: ['Core', 'Gui', 'Qml', 'Quick'])

qmlcss             = subproject('qml-css-engine')
qml_css_engine_dep = qmlcss.get_variable('qml_css_engine_dep')

app_moc = qt6.preprocess(moc_headers: files('main.h'))

executable('my-app',
  files('main.cpp'), app_moc,
  dependencies: [qt6_dep, qml_css_engine_dep])
```

**`main.cpp`**

```cpp
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include "qmlcss/csstheme.h"
#include "qmlcss/csslayout.h"

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QQmlApplicationEngine engine;

    auto *cssTheme  = new CssTheme(&engine);
    auto *cssLayout = new CssLayoutEngine(cssTheme, &engine);
    engine.rootContext()->setContextProperty("cssTheme",  cssTheme);
    engine.rootContext()->setContextProperty("cssLayout", cssLayout);
    cssTheme->load(QStringLiteral("/path/to/style.css"));

    engine.load(QUrl(QStringLiteral("qrc:/main.qml")));
    return app.exec();
}
```

**`main.qml`**

```qml
import QtQuick
import QtQuick.Window
import qmlcss 1.0 as Css

Window {
    width: 300; height: 120; visible: true
    onWidthChanged:  cssTheme.setViewport(width, height)
    onHeightChanged: cssTheme.setViewport(width, height)

    Css.CssRect {
        anchors.fill: parent
        cssId: "root"

        Css.CssText {
            text: "Styled with CSS"
            cssId: "title"
        }
    }
}
```

**`style.css`**

```css
#root {
    background: linear-gradient(to bottom, #1a1a2e, #0f3460);
    padding: 20px;
    display: flex;
    align-items: center;
    justify-content: center;
}

#title {
    color: #e0e0e0;
    font-size: 18px;
    font-weight: bold;
}
```
