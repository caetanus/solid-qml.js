import QtQuick

// The engine's QML primitives live under qrc:/qmlcss and are pulled in by linking the
// qml-css-engine dependency. `cssTheme` / `cssLayout` are provided from C++ (see main.cpp).
// CssRect is a classic C++ type registered into the `qmlcss` module (see main.cpp).
import qmlcss

Window {
    visible: true
    width: 360
    height: 160
    title: "qml-css-engine — hello"
    color: "#1e1e2e"

    // A CSS-styled, CSS-laid-out card. #card centers its child via flexbox; #greeting is a
    // plain label that inherits nothing and is matched purely by its cssId.
    CssRect {
        anchors.centerIn: parent
        width: 280
        height: 96
        cssId: "card"

        CssText {
            cssId: "greeting"
            text: "Hello, qml-css-engine!"
        }
    }
}
