import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SpektraLab.Native 1.0

// The light table page (task 6.4): the A4 (or A5/A6) light pad seen straight
// down, measured in millimetres — the model's world — with the library's
// films lying on it at their true size. The arithmetic is all on the
// lightTable model (the statement-for-statement port of the Mac's
// LightTableModel.swift); this page is the view onto it: the Drag tool (a
// mouse drags, as the Mac's finger does), the quarter-turn buttons and film
// menu where the Mac turns with two fingers, the zoom (fit, +/- x1.5, the
// wheel, 1:1 when the display knows its size), Tidy Up/Random/Scatter, and
// the size and orientation the Mac's bar owns. Every geometry binding reads
// lightTable.revision ahead of its maths, so a drag, a zoom or a pan
// re-positions the pad and every film at once (the Localization.epoch
// convention).
//
// Entry points: the mode row's 印相台 button opens the page; the library
// grid's context menu (放到印相台, "Put on Light Table") carries frames in —
// Main.qml runs that carry through the model and switches the mode here.
Item {
    id: page

    readonly property int toolHeight: 32
    readonly property int stripHeight: 28
    // The one film the toolbar and menu act on ("chosen on the page").
    readonly property string selectedUuid: {
        lightTable.revision
        return lightTable.selectedUuid()
    }
    // A carry's word, set by Main.qml; the strip shows it until the next one.
    property string notice: ""

    // The pad on screen, read through the viewport's maths.
    readonly property rect frameRect: {
        lightTable.revision
        return lightTable.frameScreenRect()
    }
    readonly property rect litRect: {
        lightTable.revision
        return lightTable.litScreenRect()
    }
    readonly property double sPx: {
        lightTable.revision
        return lightTable.scaleFactor()
    }

    // The view laid out in the room the page's own controls leave, and the
    // library's truth: sizes re-read, frames the library dropped taken off.
    // Both run on entry and whenever the page's size changes.
    function remeasure() {
        if (!visible || width < 1 || height < 1) return
        lightTable.attachWindow(window)
        lightTable.measureView(width, height, toolHeight + 4, 16, stripHeight, 16)
    }
    function refresh() {
        if (!visible) return
        lightTable.sync(library.lightTableFrames())
    }
    onWidthChanged: remeasure()
    onHeightChanged: remeasure()
    onVisibleChanged: {
        if (!visible) return
        remeasure()
        refresh()
        canvas.forceActiveFocus()
    }
    Component.onCompleted: remeasure()

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // The toolbar: the table's identity (size, orientation), the pile's
        // arrangements (Tidy Up, Random, Scatter), the view (fit, zoom,
        // 1:1), and the chosen film's turns and take-off.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: page.toolHeight
            color: Theme.card
            Rectangle { width: parent.width; height: 1; color: Theme.line; opacity: 0.4 }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                spacing: 6
                Text {
                    objectName: "lightTableTitle"
                    text: Localization.epoch && Localization.L("Light Table")
                    color: Theme.text
                    font.pixelSize: 12
                    font.bold: true
                }
                Text {
                    objectName: "lightTableFilmsText"
                    text: lightTable.filmCount === 1
                          ? Localization.epoch && Localization.L("1 film")
                          : Localization.epoch && Localization.L("%@ films", [lightTable.filmCount])
                    color: lightTable.isFull ? "#ffb39f" : Theme.muted
                    font.pixelSize: 11
                }
                Rectangle { width: 1; height: 18; color: Theme.line; opacity: 0.5 }
                Text { text: Localization.epoch && Localization.L("Table Size"); color: Theme.muted; font.pixelSize: 10 }
                ComboBox {
                    objectName: "lightTablePaperCombo"
                    implicitWidth: 74
                    font.pixelSize: 11
                    model: ["A4", "A5", "A6"]
                    currentIndex: {
                        lightTable.revision
                        const at = model.indexOf(lightTable.paperName)
                        return at < 0 ? 0 : at
                    }
                    onActivated: lightTable.setPaper(model[currentIndex])
                }
                CheckBox {
                    objectName: "lightTablePortraitCheck"
                    text: lightTable.portrait
                          ? Localization.epoch && Localization.L("Portrait")
                          : Localization.epoch && Localization.L("Landscape")
                    font.pixelSize: 11
                    checked: lightTable.portrait
                    onToggled: lightTable.portrait = checked
                    ToolTip.visible: hovered
                    ToolTip.text: Localization.epoch && Localization.L("Table Orientation")
                }
                Rectangle { width: 1; height: 18; color: Theme.line; opacity: 0.5 }
                ActionButton {
                    objectName: "lightTableTidyButton"
                    text: Localization.epoch && Localization.L("Tidy Up")
                    implicitHeight: 23
                    enabled: lightTable.filmCount > 0
                    onClicked: lightTable.tidyUp()
                }
                ActionButton {
                    objectName: "lightTableRandomButton"
                    text: Localization.epoch && Localization.L("Random")
                    implicitHeight: 23
                    enabled: lightTable.filmCount > 0
                    onClicked: lightTable.randomize(Math.floor(Math.random() * 2147483647))
                }
                ActionButton {
                    objectName: "lightTableScatterButton"
                    text: Localization.epoch && Localization.L("Scatter")
                    implicitHeight: 23
                    enabled: lightTable.filmCount > 0
                    onClicked: lightTable.scatterNow(Math.floor(Math.random() * 2147483647))
                }
                Item { Layout.fillWidth: true }
                Text {
                    text: Localization.epoch &&
                          Localization.L("Zoomed %@ percent of fit", [Math.round(lightTable.zoom * 100)])
                    color: Theme.muted
                    font.pixelSize: 10
                    horizontalAlignment: Text.AlignRight
                }
                ActionButton {
                    objectName: "lightTableFitButton"
                    text: Localization.epoch && Localization.L("Fit")
                    accented: !lightTable.isZoomed
                    implicitHeight: 23
                    onClicked: lightTable.fitView()
                    ToolTip.visible: hovered
                    ToolTip.text: Localization.epoch && Localization.L("Fit the table to the window")
                }
                ActionButton {
                    objectName: "lightTableZoomOutButton"
                    text: "−"
                    implicitHeight: 23
                    implicitWidth: 27
                    font.pixelSize: 13
                    onClicked: lightTable.zoomOut()
                    ToolTip.visible: hovered
                    ToolTip.text: Localization.epoch && Localization.L("Zoom Out")
                }
                ActionButton {
                    objectName: "lightTableZoomInButton"
                    text: "+"
                    implicitHeight: 23
                    implicitWidth: 27
                    font.pixelSize: 13
                    onClicked: lightTable.zoomIn()
                    ToolTip.visible: hovered
                    ToolTip.text: Localization.epoch && Localization.L("Zoom In")
                }
                ActionButton {
                    objectName: "lightTableActualSizeButton"
                    text: Localization.epoch && Localization.L("Actual Size")
                    accented: lightTable.actualSizeActive
                    implicitHeight: 23
                    // A display that does not know its size: no 1:1 button.
                    enabled: lightTable.canActualSize
                    onClicked: lightTable.actualSize()
                }
                Rectangle { width: 1; height: 18; color: Theme.line; opacity: 0.5 }
                ActionButton {
                    objectName: "lightTableRotateLeftButton"
                    text: Localization.epoch && Localization.L("Rotate Left")
                    implicitHeight: 23
                    enabled: page.selectedUuid.length > 0 && lightTable.itemOn(page.selectedUuid)
                    onClicked: lightTable.turnQuarter(page.selectedUuid, 1)
                }
                ActionButton {
                    objectName: "lightTableRotateRightButton"
                    text: Localization.epoch && Localization.L("Rotate Right")
                    implicitHeight: 23
                    enabled: page.selectedUuid.length > 0 && lightTable.itemOn(page.selectedUuid)
                    onClicked: lightTable.turnQuarter(page.selectedUuid, -1)
                }
                ActionButton {
                    objectName: "lightTableTakeOffButton"
                    text: Localization.epoch && Localization.L("Take Off")
                    implicitHeight: 23
                    enabled: page.selectedUuid.length > 0 && lightTable.itemOn(page.selectedUuid)
                    onClicked: lightTable.takeOff(page.selectedUuid)
                }
            }
        }

        // The canvas: the desk, the aluminium frame, the lit area, and the
        // films. The ground MouseArea takes the view's pans and zooms; the
        // films' own areas grab their drags first.
        Item {
            id: canvas
            objectName: "lightTableCanvas"
            Layout.fillWidth: true
            Layout.fillHeight: true
            focus: true
            clip: true

            // the desk round everything
            Rectangle { anchors.fill: parent; color: "#4b4b49" }

            // the aluminium frame round the lit area
            Rectangle {
                color: "#b9b6b0"
                radius: 6
                x: page.frameRect.x
                y: page.frameRect.y
                width: page.frameRect.width
                height: page.frameRect.height
            }

            // the lit area: the light itself
            Rectangle {
                id: lit
                color: "#f5f2ec"
                radius: 3.0 * page.sPx // the lit area's corner radius, mm
                x: page.litRect.x
                y: page.litRect.y
                width: page.litRect.width
                height: page.litRect.height

                // The scale's band along the top and left edges, which
                // placing keeps clear of — a hint at the pad's own edge.
                Rectangle {
                    width: parent.width
                    height: 10 * page.sPx
                    color: "#000000"
                    opacity: 0.05
                }
                Rectangle {
                    width: 10 * page.sPx
                    height: parent.height
                    color: "#000000"
                    opacity: 0.05
                }

                // The films, bottom of the pile first. Rows are the whole
                // table (on it and taken off); only the on-table ones with a
                // true size draw. Each lies at its centre, unturned box,
                // turned about that centre by the film's own angle — the
                // screen's y runs down, so the radians' counter-clockwise
                // turn reads as a minus here.
                Repeater {
                    model: lightTable

                    delegate: Item {
                        id: film

                        required property string frame
                        required property bool on
                        // x/y are QQuickItem's own final properties — the
                        // model's mm coordinates come through the implicit
                        // `model` object instead (model.x / model.y).
                        required property double rotDeg
                        required property double w
                        required property double h
                        required property var image
                        required property bool hasImage
                        required property int index

                        readonly property bool drawn: on && w > 0 && h > 0
                        readonly property point centre: {
                            lightTable.revision
                            return lightTable.toScreen(Qt.point(x, y))
                        }

                        x: centre.x - width / 2
                        y: centre.y - height / 2
                        width: w * page.sPx
                        height: h * page.sPx
                        rotation: -rotDeg
                        z: index
                        visible: drawn

                        // the film's silhouette: unexposed film, dark, with a
                        // hair of margin round the picture
                        Rectangle {
                            anchors.fill: parent
                            color: "#181614"
                            radius: 1
                            border.color: page.selectedUuid === film.frame ? Theme.accent : "#3a3733"
                            border.width: page.selectedUuid === film.frame ? 2 : 1
                        }
                        // the picture, fitted — the library's cached decode;
                        // until it lands the silhouette IS the honest view of
                        // the film's true size
                        FrameCanvas {
                            anchors.fill: parent
                            anchors.margins: 1
                            image: film.image
                            visible: film.hasImage
                            enabled: false
                        }

                        MouseArea {
                            anchors.fill: parent
                            acceptedButtons: Qt.LeftButton | Qt.RightButton
                            cursorShape: Qt.OpenHandCursor
                            property point pressScreen
                            property point pressCentreMM
                            onPressed: function(mouse) {
                                lightTable.setSelected(film.frame)
                                if (mouse.button === Qt.RightButton) {
                                    filmMenu.uuid = film.frame
                                    filmMenu.popup()
                                    return
                                }
                                lightTable.beginGesture()
                                pressScreen = mapToItem(canvas, mouse.x, mouse.y)
                                pressCentreMM = Qt.point(model.x, model.y)
                            }
                            onPositionChanged: function(mouse) {
                                if (!pressed || pressScreen.x < 0) return
                                const now = mapToItem(canvas, mouse.x, mouse.y)
                                const mmNow = lightTable.toWorld(now)
                                const mmFrom = lightTable.toWorld(pressScreen)
                                lightTable.moveTo(film.frame,
                                                  pressCentreMM.x + (mmNow.x - mmFrom.x),
                                                  pressCentreMM.y + (mmNow.y - mmFrom.y))
                            }
                            onReleased: function(mouse) {
                                lightTable.endGesture()
                                pressScreen = Qt.point(-1, -1)
                            }
                            onCanceled: {
                                lightTable.endGesture()
                                pressScreen = Qt.point(-1, -1)
                            }
                        }
                    }
                }

                Text {
                    anchors.centerIn: parent
                    visible: lightTable.filmCount === 0
                    text: Localization.epoch && Localization.L("No films on the table.")
                    color: "#9a978f"
                    font.pixelSize: 13
                }
            }

            MouseArea {
                id: ground
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton
                property point pressScreen
                property point centreAtPress
                onPressed: function(mouse) {
                    lightTable.setSelected("")
                    pressScreen = Qt.point(mouse.x, mouse.y)
                    centreAtPress = lightTable.viewCentre()
                }
                onPositionChanged: function(mouse) {
                    if (!pressed) return
                    lightTable.panView(centreAtPress, mouse.x - pressScreen.x, mouse.y - pressScreen.y)
                }
                onDoubleClicked: lightTable.fitView()
                onWheel: function(wheel) {
                    const factor = wheel.angleDelta.y > 0 ? 1.5 : 1 / 1.5
                    lightTable.zoomAt(lightTable.zoom * factor,
                                      lightTable.toWorld(Qt.point(wheel.x, wheel.y)),
                                      Qt.point(wheel.x, wheel.y))
                    wheel.accepted = true
                }
            }

            // The film menu, where the Mac's two-finger corner-turn lands on
            // a mouse: quarter turns either way, or off the table.
            Menu {
                id: filmMenu
                property string uuid
                MenuItem {
                    text: Localization.epoch && Localization.L("Rotate Left")
                    onClicked: lightTable.turnQuarter(filmMenu.uuid, 1)
                }
                MenuItem {
                    text: Localization.epoch && Localization.L("Rotate Right")
                    onClicked: lightTable.turnQuarter(filmMenu.uuid, -1)
                }
                MenuItem {
                    text: Localization.epoch && Localization.L("Take Off")
                    onClicked: lightTable.takeOff(filmMenu.uuid)
                }
            }
        }

        // The info strip (every mode carries one): the chosen film's true
        // size and angle in the model's own reading, the pile's count, and a
        // carry's word.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: page.stripHeight
            color: Theme.card
            Text {
                objectName: "lightTableInfoStrip"
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                verticalAlignment: Text.AlignVCenter
                text: {
                    lightTable.revision
                    const bits = []
                    if (page.notice.length > 0) bits.push(page.notice)
                    const sel = page.selectedUuid
                    if (sel.length > 0 && lightTable.itemOn(sel) && lightTable.itemW(sel) > 0)
                        bits.push(lightTable.mmLabel(lightTable.itemW(sel)) + " × " +
                                  lightTable.mmLabel(lightTable.itemH(sel)) + " mm · " +
                                  lightTable.degreesLabel(lightTable.itemRot(sel)))
                    bits.push(lightTable.isFull
                                  ? Localization.epoch && Localization.L("The table is full: 99 films.")
                                  : Localization.epoch && Localization.L("Films on the Table"))
                    return bits.join("  ·  ")
                }
                color: lightTable.isFull ? "#ffb39f" : Theme.muted
                font.pixelSize: 10
                elide: Text.ElideRight
            }
        }
    }
}
