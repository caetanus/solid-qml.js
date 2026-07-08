// MediaPlayer — an opt-in module (solidqml.Widgets.Media): QtMultimedia loads only when the
// <MediaPlayer> tag is used, the core engine never depends on it (the Chart/Scene3D pattern).
// A CssFill "div" wrapper hosts the VideoOutput plus a self-contained control strip: play/pause,
// a seek slider (the core W.Slider), and an elapsed/total clock. Styling hooks: the author's
// classes on the wrapper, `.media-controls`/`.media-time` on the strip.
//
// The transpiler emits:  WMedia.MediaPlayer { cssClass: […]; src: <url>; autoplay: <bool> }
import QtQuick
import QtMultimedia
import qmlcss 1.0 as Css
import solidqml.Widgets 1.0 as W

Css.CssFill {
    id: root
    property url src: ""
    property bool autoplay: false
    property alias playing: player.playing
    signal mediaError(string message)

    cssPrimitive: "div"
    implicitWidth: 480
    implicitHeight: 314

    function _clock(ms) {
        var s = Math.floor(ms / 1000);
        var m = Math.floor(s / 60);
        s = s % 60;
        return m + ":" + (s < 10 ? "0" : "") + s;
    }

    MediaPlayer {
        id: player
        source: root.src
        audioOutput: AudioOutput { }
        videoOutput: video
        onErrorOccurred: function (error, errorString) { root.mediaError(errorString); }
        onMediaStatusChanged: if (root.autoplay && mediaStatus === MediaPlayer.LoadedMedia) player.play()
    }

    Item {
        anchors.fill: parent

        Rectangle { // letterbox backdrop behind the video frame
            anchors.fill: video
            color: "#10161f"
        }
        VideoOutput {
            id: video
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: controls.top
        }

        Css.CssFill {
            id: controls
            cssPrimitive: "div"
            cssClass: ["media-controls"]
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 44

            Item {
                anchors.fill: parent

                Css.CssText {
                    id: playGlyph
                    cssPrimitive: ""
                    cssClass: ["media-play"]
                    x: 14
                    anchors.verticalCenter: parent.verticalCenter
                    text: player.playing ? "⏸" : "▶"
                }
                MouseArea {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: 44
                    cursorShape: Qt.PointingHandCursor
                    onClicked: player.playing ? player.pause() : player.play()
                }

                Css.CssText {
                    id: clock
                    cssPrimitive: ""
                    cssClass: ["media-time"]
                    anchors.right: parent.right
                    anchors.rightMargin: 14
                    anchors.verticalCenter: parent.verticalCenter
                    text: root._clock(player.position) + " / " + root._clock(player.duration)
                }

                W.Slider {
                    id: seek
                    anchors.left: parent.left
                    anchors.leftMargin: 48
                    anchors.right: clock.left
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    height: 20
                    from: 0
                    to: Math.max(1, player.duration)
                    onMoved: player.position = seek.value
                }
                Binding { // controlled: playback drives the knob; a drag writes through onMoved
                    target: seek
                    property: "value"
                    value: player.position
                    restoreMode: Binding.RestoreNone
                }
            }
        }
    }
}
