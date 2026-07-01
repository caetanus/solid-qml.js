import QtQuick

// Drives a target's numeric property through a CSS `@keyframes` sequence.
//
// Feed it the frames (`cssTheme.keyframes(name)`), the property to animate, and the
// `animation` params (`cssTheme.parseAnimation(...)`). It loops a normalized `progress`
// 0→1 over `duration` and writes the interpolated keyframe value to target[property] —
// so one cycle walks the whole `@keyframes` (e.g. opacity 0%→50%→100% = 0→1→0). Numeric
// properties only (opacity, etc.); the single easing applies to the whole cycle.
Item {
    id: root
    visible: false
    width: 0
    height: 0

    property var frames: []
    property string animatedProperty: ""
    property var target: null
    property int duration: 1000
    property int easingType: Easing.InOutSine
    property int iterations: -1 // -1 = infinite
    property bool running: false

    property real progress: 0.0

    function _numAt(p) {
        var ks = root.frames
        if (!ks || ks.length === 0)
            return NaN
        if (p <= ks[0].offset)
            return parseFloat(ks[0].properties[root.animatedProperty])
        for (var i = 0; i < ks.length - 1; ++i) {
            var a = ks[i]
            var b = ks[i + 1]
            if (p >= a.offset && p <= b.offset) {
                var span = b.offset - a.offset
                var t = span > 0 ? (p - a.offset) / span : 0
                var va = parseFloat(a.properties[root.animatedProperty])
                var vb = parseFloat(b.properties[root.animatedProperty])
                return va + (vb - va) * t
            }
        }
        return parseFloat(ks[ks.length - 1].properties[root.animatedProperty])
    }

    onProgressChanged: {
        if (!root.target || root.animatedProperty.length === 0)
            return
        var v = root._numAt(root.progress)
        if (!isNaN(v))
            root.target[root.animatedProperty] = v
    }

    NumberAnimation on progress {
        from: 0.0
        to: 1.0
        duration: Math.max(1, root.duration)
        loops: root.iterations < 0 ? Animation.Infinite : Math.max(1, root.iterations)
        easing.type: root.easingType
        running: root.running && root.frames && root.frames.length > 0
    }
}
