import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Layer 2 tone curve. The chips pick one of the five channels; the Canvas
// paints the quarter grid, the identity diagonal, a 64-segment polyline
// sampled through preview.curveEval and the control points from
// preview.curvePoints. One MouseArea inserts a point on an empty click and
// drags it, moves existing points, and removes a point on double-click —
// the desktop CurveEditor's interaction. The drag gesture key matches the
// controller's "curve:<channel>" edit key, so a drag is one undo step.
ColumnLayout {
    id: root
    property int channel: 0
    readonly property var channelKeys: ["rgb", "luma", "red", "green", "blue"]
    readonly property string channelKey: channelKeys[channel]
    readonly property var channelLabels: ["RGB", "明度", "红", "绿", "蓝"]
    readonly property var channelColors: ["#faf8f4", "#c9b06a", "#e8524e", "#62c462", "#4ec0e8"]
    // Reading preview.layer2 subscribes this binding to settingsChanged; the
    // curve* invokables alone would never make it re-evaluate.
    readonly property var points: {
        var curves = preview.layer2.curves
        var list = curves === undefined ? undefined : curves[channelKey]
        return list === undefined ? [] : list
    }
    readonly property bool editable: preview.settingsEditable && preview.hasFrame
    Layout.fillWidth: true
    spacing: 4
    onPointsChanged: curve.requestPaint()
    onChannelChanged: curve.requestPaint()
    Component.onCompleted: curve.requestPaint()

    Row {
        spacing: 4
        Repeater {
            model: channelLabels
            ActionButton {
                required property int index
                required property string modelData
                text: modelData
                font.pixelSize: 10
                implicitHeight: 20
                accented: root.channel === index
                enabled: root.editable
                onClicked: root.channel = index
            }
        }
    }
    Rectangle {
        id: plot
        Layout.fillWidth: true
        Layout.preferredHeight: 176
        radius: 4
        color: Theme.wellDark
        clip: true
        Canvas {
            id: curve
            anchors.fill: parent
            anchors.margins: 9
            antialiasing: true
            onPaint: {
                var ctx = getContext("2d")
                ctx.clearRect(0, 0, width, height)
                ctx.lineWidth = 1
                ctx.strokeStyle = "#33322f"
                ctx.beginPath()
                for (var g = 1; g < 4; ++g) {
                    ctx.moveTo(g * width / 4, 0); ctx.lineTo(g * width / 4, height)
                    ctx.moveTo(0, g * height / 4); ctx.lineTo(width, g * height / 4)
                }
                ctx.stroke()
                ctx.strokeStyle = "#4a4844"
                ctx.beginPath()
                ctx.moveTo(0, height); ctx.lineTo(width, 0)
                ctx.stroke()
                ctx.strokeStyle = root.channelColors[root.channel]
                ctx.lineWidth = 1.4
                ctx.beginPath()
                for (var i = 0; i <= 64; ++i) {
                    var y = (1 - preview.curveEval(root.channelKey, i / 64)) * height
                    if (i === 0) ctx.moveTo(0, y); else ctx.lineTo(i / 64 * width, y)
                }
                ctx.stroke()
                ctx.fillStyle = Theme.card
                ctx.strokeStyle = root.channelColors[root.channel]
                ctx.lineWidth = 1.2
                var pts = root.points
                for (var p = 0; p < pts.length; ++p) {
                    ctx.beginPath()
                    ctx.arc(pts[p].x * width, (1 - pts[p].y) * height, 3.5, 0, 2 * Math.PI)
                    ctx.fill(); ctx.stroke()
                }
            }
        }
        MouseArea {
            id: curveMouse
            anchors.fill: parent
            enabled: root.editable
            cursorShape: Qt.CrossCursor
            property int dragIndex: -1
            function toCurve(mouse) {
                return Qt.point(
                    Math.max(0, Math.min(1, (mouse.x - curve.x) / curve.width)),
                    Math.max(0, Math.min(1, 1 - (mouse.y - curve.y) / curve.height)))
            }
            function nearestIndex(pts, p) {
                var best = -1
                var bestDistance = 14 / Math.min(curve.width, curve.height)
                for (var i = 0; i < pts.length; ++i) {
                    var dx = pts[i].x - p.x, dy = pts[i].y - p.y
                    var d = Math.sqrt(dx * dx + dy * dy)
                    if (d <= bestDistance) { bestDistance = d; best = i }
                }
                return best
            }
            function hitIndex(mouse) {
                return nearestIndex(root.points, toCurve(mouse))
            }
            onPressed: function(mouse) {
                preview.beginEditGesture("curve:" + root.channelKey)
                var hit = hitIndex(mouse)
                if (hit < 0) {
                    // Empty click: insert, then grab the fresh point to drag —
                    // read the controller directly, the points binding may not
                    // have re-evaluated yet.
                    var p = toCurve(mouse)
                    preview.curveInsertPoint(root.channelKey, p.x, p.y)
                    hit = nearestIndex(preview.curvePoints(root.channelKey), p)
                }
                dragIndex = hit
            }
            onPositionChanged: function(mouse) {
                if (dragIndex < 0) return
                var p = toCurve(mouse)
                preview.curveMovePoint(root.channelKey, dragIndex, p.x, p.y)
            }
            onReleased: { dragIndex = -1; preview.endEditGesture() }
            onDoubleClicked: function(mouse) {
                var hit = hitIndex(mouse)
                if (hit >= 0) preview.curveRemovePoint(root.channelKey, hit)
            }
        }
    }
}
