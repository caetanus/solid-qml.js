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
    // External SRT subtitles: parsed here (QtMultimedia only reads EMBEDDED tracks) and shown
    // as a synced overlay caption (`.media-subtitle`).
    property url subtitles: ""
    property alias playing: player.playing
    signal mediaError(string message)

    property var _cues: []
    property string _cue: ""

    // Initial property values don't emit changed signals — load once at completion too.
    Component.onCompleted: _loadSubtitles()
    onSubtitlesChanged: _loadSubtitles()

    function _loadSubtitles() {
        _cues = [];
        _cue = "";
        if (subtitles == "")
            return;
        // The loader's fetch() shim (QNetworkAccessManager) — reads file:// too; QML's own XHR
        // blocks local files in Qt 6 (QML_XHR_ALLOW_FILE_READ), and the house rule is fetch anyway.
        fetch(subtitles.toString()).then(function (r) { return r.text(); }).then(function (t) {
            root._cues = root._parseSrt(t);
        }).catch(function (e) { root.mediaError("subtitles: " + e); });
    }

    function _parseSrt(text) {
        var cues = [];
        var blocks = text.replace(/\r/g, "").split("\n\n");
        var t = /(\d+):(\d+):(\d+)[,.](\d+)\s*-->\s*(\d+):(\d+):(\d+)[,.](\d+)/;
        for (var i = 0; i < blocks.length; i++) {
            var lines = blocks[i].split("\n").filter(function (l) { return l.length > 0; });
            for (var j = 0; j < lines.length; j++) {
                var m = t.exec(lines[j]);
                if (!m) continue;
                var start = ((+m[1] * 60 + +m[2]) * 60 + +m[3]) * 1000 + +m[4];
                var end = ((+m[5] * 60 + +m[6]) * 60 + +m[7]) * 1000 + +m[8];
                cues.push({ start: start, end: end, text: lines.slice(j + 1).join("\n") });
                break;
            }
        }
        return cues;
    }

    function _syncCue(pos) {
        for (var i = 0; i < _cues.length; i++) {
            if (pos >= _cues[i].start && pos <= _cues[i].end) {
                if (_cue !== _cues[i].text) _cue = _cues[i].text;
                return;
            }
        }
        if (_cue !== "") _cue = "";
    }

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
        // Paused by default (desktop convention: playback starts on the user's click); autoplay
        // is an explicit opt-in.
        onMediaStatusChanged: if (root.autoplay && mediaStatus === MediaPlayer.LoadedMedia) player.play()
        onPositionChanged: root._syncCue(position)
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

        Css.CssText {
            cssPrimitive: ""
            cssClass: ["media-subtitle"]
            visible: root._cue !== ""
            text: root._cue
            anchors.horizontalCenter: video.horizontalCenter
            anchors.bottom: video.bottom
            anchors.bottomMargin: 14
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
