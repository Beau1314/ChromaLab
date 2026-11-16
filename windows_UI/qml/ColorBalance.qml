import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Layer 2 colour balance: the four zones (master / shadows / midtones /
// highlights), each with hue, saturation and luminance sliders plus its own
// reset. Ranges are the controller's setZone contract — hue in degrees,
// saturation and luminance as fractions — matching the desktop's ColorWheel
// panel. Slider gesture keys match the controller's edit keys
// ("colorBalance:<zone>"), so one drag is one undo step.
ColumnLayout {
    id: root
    readonly property var zoneLabels: Localization.epoch &&
        [Localization.L("Master"), Localization.L("Shadows"), Localization.L("Midtones"), Localization.L("Highlights")]
    readonly property bool editable: preview.settingsEditable && preview.hasFrame
    Layout.fillWidth: true
    spacing: 8

    function resetZone(zone) {
        for (var axis = 0; axis < 3; ++axis) preview.setZone(zone, axis, 0)
    }

    component ZoneSlider: ColumnLayout {
        id: zoneSlider
        property string label
        property int zone
        property int axis
        property real from
        property real to
        property real step
        property int decimals
        property string suffix: ""
        readonly property string zoneName: zone === 0 ? "master" : zone === 1 ? "shadows" : zone === 2 ? "midtones" : "highlights"
        readonly property string axisName: axis === 0 ? "hue" : axis === 1 ? "saturation" : "luminance"
        readonly property real current: {
            var zones = preview.layer2.colorBalance
            var z = zones === undefined ? undefined : zones[zoneName]
            var v = z === undefined ? undefined : z[axisName]
            return v === undefined ? 0 : Number(v)
        }
        Layout.fillWidth: true
        spacing: 2
        RowLayout {
            Layout.fillWidth: true
            Text { text: zoneSlider.label; color: Theme.text; font.pixelSize: 11 }
            Item { Layout.fillWidth: true }
            Text { text: zoneSlider.current.toFixed(zoneSlider.decimals) + zoneSlider.suffix; color: Theme.muted; font.pixelSize: 10 }
        }
        Slider {
            Layout.fillWidth: true; implicitHeight: 24
            from: zoneSlider.from; to: zoneSlider.to; stepSize: zoneSlider.step
            value: zoneSlider.current
            enabled: preview.settingsEditable && preview.hasFrame
            onPressedChanged: {
                if (pressed) preview.beginEditGesture("colorBalance:" + zoneSlider.zoneName)
                else preview.endEditGesture()
            }
            onMoved: preview.setZone(zoneSlider.zone, zoneSlider.axis, value)
        }
    }

    Repeater {
        model: zoneLabels
        ColumnLayout {
            required property int index
            Layout.fillWidth: true
            spacing: 2
            RowLayout {
                Layout.fillWidth: true
                Text { text: root.zoneLabels[index]; color: Theme.text; font.pixelSize: 11; font.bold: true }
                Item { Layout.fillWidth: true }
                ActionButton { text: Localization.epoch && Localization.L("Reset"); font.pixelSize: 10; implicitHeight: 20; enabled: root.editable; onClicked: root.resetZone(index) }
            }
            ZoneSlider { label: Localization.epoch && Localization.L("Hue"); zone: index; axis: 0; from: 0; to: 360; step: 1; decimals: 0; suffix: "°" }
            ZoneSlider { label: Localization.epoch && Localization.L("Saturation"); zone: index; axis: 1; from: 0; to: 1; step: 0.05; decimals: 2 }
            ZoneSlider { label: Localization.epoch && Localization.L("Brightness"); zone: index; axis: 2; from: -1; to: 1; step: 0.05; decimals: 2 }
        }
    }
}
