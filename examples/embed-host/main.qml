// The "legacy" host scene: plain QtQuick chrome around a solid-qml island.
import QtQuick
import QtQuick.Window
import solidqml.Embed 1.0

Window {
    id: win
    width: 640
    height: 420
    visible: true
    color: "#dfe4ea"
    title: "legacy host + solid island"

    Text {
        id: legacyHeader
        x: 16; y: 12
        text: "LEGACY QTQUICK APP — the island below is solid-qml"
        font.pixelSize: 13
        color: "#40495a"
    }

    Rectangle {
        x: 16; y: 40
        width: win.width - 32
        height: win.height - 56
        color: "#ffffff"
        radius: 8
        border.color: "#b7c0cd"

        SolidIsland {
            id: island
            anchors.fill: parent
            anchors.margins: 12
            source: "Counter.qml"
            cssFiles: ["App.generated.css"]
            onReady: {
                rootItem.label = "host-driven"
                if (Qt.application.arguments.indexOf("--probe") >= 0) probe.start()
            }
            onError: (message) => console.log("ISLAND error: " + message)
        }
    }

    Timer {
        id: probe
        interval: 600; repeat: false
        onTriggered: {
            var r = island.rootItem;
            console.log("PROBE label=" + r.label + " count0=" + r.count);
            r.count = 41;               // host writes state…
            function findText(w) {
                var stack = [w];
                while (stack.length) {
                    var it = stack.pop();
                    for (var i = 0; i < it.children.length; i++) {
                        var c = it.children[i];
                        if (c.text !== undefined && String(c.text).indexOf(":") >= 0) return c;
                        stack.push(c);
                    }
                }
                return null;
            }
            var t = findText(island);
            console.log("PROBE binding reacted: '" + (t ? t.text : "?") + "' (expect host-driven: 41)");
            r.count = r.count + 1;      // …and the solid handler path still works
            console.log("PROBE after inc: '" + (t ? t.text : "?") + "'");
        }
    }
}
