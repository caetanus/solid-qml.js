import QtQuick
import QtQuick.Effects

// Reusable drop shadow built from a parsed CssTheme shadow map
// ({ x, y, blur, color }). Intended as a Text/Item `layer.effect`:
//
//   layer.enabled: myText.shadow.color !== undefined
//   layer.effect: QmlCss.CssDropShadow { shadow: myText.shadow }
MultiEffect {
    property var shadow: ({})
    readonly property bool hasShadow: shadow && shadow.color !== undefined

    shadowEnabled: hasShadow
    shadowColor: hasShadow ? shadow.color : "transparent"
    shadowHorizontalOffset: hasShadow ? shadow.x : 0
    shadowVerticalOffset: hasShadow ? shadow.y : 0
    // MultiEffect.shadowBlur is 0..1 over blurMax (32px default).
    shadowBlur: hasShadow ? Math.min(1, shadow.blur / 32) : 0
    autoPaddingEnabled: true
}
