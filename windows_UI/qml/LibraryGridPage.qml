import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SpektraLab.Native 1.0

// The library grid page (task 4.4; MAC-LIBRARY-UI §4/§5): a lazy GridView
// over the library's rows model (cacheBuffer + reuseItems), each cell the
// thumbnail (via the model's worker-pool decode) with its file name and the
// badge row — 11-point stars, the colour dot, the develop ring. Selection is
// the library model's own (click, Ctrl-click toggles, Shift-click extends);
// double-click or Return hands the frame to the develop page; Ctrl+= / Ctrl+-
// step the cell between 64 and 256 at ×1.25 (the header's buttons do the
// same). §4's metrics: a square cell (default 112), a 2-point gutter, the
// picture fitted inside a 6-point inset, the chosen tile wearing the white
// frame, and type never below 11.
Item {
    id: gridPage

    readonly property int gutter: 2
    readonly property int inset: 6
    property real cellSize: 112
    signal openRequested(string path)

    function zoomIn() { cellSize = Math.min(256, Math.max(64, Math.round(cellSize * 1.25))) }
    function zoomOut() { cellSize = Math.max(64, Math.min(256, Math.round(cellSize / 1.25))) }
    // The grid owns the page's keys (Main hands it focus on entering 图库).
    function takeFocus() { grid.forceActiveFocus() }
    function moveSelection(delta) {
        var next = grid.currentIndex + delta
        if (next < 0 || next >= library.rowCount()) return
        grid.currentIndex = next
        grid.positionViewAtIndex(next, GridView.Contain)
        library.selectOnly(next)
    }

    // The colour-label dots, MacColorLabel's seven.
    readonly property var labelColors: [,
        "#e1574e", "#e6954f", "#e3c14f", "#6bbd5f", "#5588d8", "#d967ad", "#9067c9"]

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // The grid header: count, sort and its direction, the zoom steps.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 32
            color: Theme.card
            Rectangle { width: parent.width; height: 1; color: Theme.line; opacity: 0.4 }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                spacing: 6
                Text {
                    objectName: "libraryCountText"
                    text: library.total + " 张照片 · 已选 " + library.selectedCount
                    color: Theme.muted
                    font.pixelSize: 11
                }
                Item { Layout.fillWidth: true }
                ComboBox {
                    objectName: "librarySortCombo"
                    implicitWidth: 108
                    font.pixelSize: 11
                    model: ["拍摄时间", "导入时间", "文件名", "评分"]
                    readonly property var sorts: ["captured", "imported", "name", "rating"]
                    onActivated: library.sort = sorts[currentIndex]
                }
                CheckBox {
                    objectName: "libraryDescendingCheck"
                    text: "降序"
                    font.pixelSize: 11
                    checked: library.descending
                    onToggled: library.descending = checked
                }
                ActionButton {
                    objectName: "libraryZoomOutButton"
                    text: "−"
                    implicitHeight: 23
                    implicitWidth: 27
                    font.pixelSize: 13
                    onClicked: gridPage.zoomOut()
                    ToolTip.visible: hovered
                    ToolTip.text: "更小的缩略图 · Ctrl+-"
                }
                Text {
                    text: Math.round(gridPage.cellSize) + " px"
                    color: Theme.muted
                    font.pixelSize: 10
                }
                ActionButton {
                    objectName: "libraryZoomInButton"
                    text: "+"
                    implicitHeight: 23
                    implicitWidth: 27
                    font.pixelSize: 13
                    onClicked: gridPage.zoomIn()
                    ToolTip.visible: hovered
                    ToolTip.text: "更大的缩略图 · Ctrl+="
                }
            }
        }

        // The grid itself. Keys live here so a typing field never steals
        // them: 0-5 rate, − * + colour the labels (Capture One's red,
        // yellow, green), Return or double-click opens in 冲洗, the arrows
        // move the selection, Ctrl+A takes the page.
        GridView {
            id: grid
            objectName: "libraryGrid"
            Layout.fillWidth: true
            Layout.fillHeight: true
            focus: true
            clip: true
            model: library
            cacheBuffer: 600
            reuseItems: true
            currentIndex: 0
            cellWidth: gridPage.cellSize + gridPage.gutter * 2
            cellHeight: gridPage.cellSize + 40
            ScrollBar.vertical: ScrollBar {}

            Keys.onPressed: function(event) {
                if (event.key === Qt.Key_Left) { gridPage.moveSelection(-1); event.accepted = true }
                else if (event.key === Qt.Key_Right) { gridPage.moveSelection(1); event.accepted = true }
                else if (event.key === Qt.Key_Up) { gridPage.moveSelection(-Math.max(1, Math.floor(grid.width / grid.cellWidth))); event.accepted = true }
                else if (event.key === Qt.Key_Down) { gridPage.moveSelection(Math.max(1, Math.floor(grid.width / grid.cellWidth))); event.accepted = true }
                else if (event.key >= Qt.Key_0 && event.key <= Qt.Key_5) {
                    var rating = event.key - Qt.Key_0
                    var uuids = library.selectedUuids()
                    for (var i = 0; i < uuids.length; ++i) library.setRating(uuids[i], rating)
                    event.accepted = true
                }
                else if (event.key === Qt.Key_Minus || event.key === Qt.Key_Underscore) {
                    var reds = library.selectedUuids()
                    for (var r = 0; r < reds.length; ++r) library.setColorTag(reds[r], 1)
                    event.accepted = true
                }
                else if (event.text === "*" || event.key === Qt.Key_Asterisk) {
                    var yellows = library.selectedUuids()
                    for (var y = 0; y < yellows.length; ++y) library.setColorTag(yellows[y], 3)
                    event.accepted = true
                }
                else if (event.text === "+" || event.key === Qt.Key_Plus) {
                    var greens = library.selectedUuids()
                    for (var g = 0; g < greens.length; ++g) library.setColorTag(greens[g], 4)
                    event.accepted = true
                }
                else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                    if (grid.currentIndex >= 0 && grid.currentIndex < library.rowCount()) {
                        var info = library.rowInfo(library.uuidAt(grid.currentIndex))
                        if (info.path !== undefined && String(info.path).length > 0)
                            gridPage.openRequested(String(info.path))
                    }
                    event.accepted = true
                }
                else if (event.key === Qt.Key_A && (event.modifiers & Qt.ControlModifier)) {
                    library.selectAllRows()
                    event.accepted = true
                }
            }

            delegate: Rectangle {
                id: tile
                required property int index
                required property string uuid
                required property string path
                required property string fileName
                required property int rating
                required property int colorTag
                required property var develop
                required property bool selected
                required property bool hasThumbnail

                width: grid.cellWidth
                height: grid.cellHeight
                color: tile.selected ? Theme.card : "transparent"
                radius: 4
                border.color: tile.selected ? Theme.selection : "transparent"
                border.width: tile.selected ? 2 : 0

                // The thumbnail decode rides the worker pool: the ask
                // returns at once when the row is cached, otherwise the
                // picture lands as dataChanged(hasThumbnail/thumbnail).
                onPathChanged: library.getThumbnail(tile.path, 256)
                Component.onCompleted: library.getThumbnail(tile.path, 256)

                // The picture, fitted (not cropped) inside the 6-point inset.
                FrameCanvas {
                    id: thumb
                    anchors.fill: parent
                    anchors.margins: gridPage.inset + 2
                    anchors.bottomMargin: gridPage.inset + 2 + 34
                    image: thumbnail
                    enabled: false
                }
                Rectangle {
                    visible: !tile.hasThumbnail
                    anchors.fill: thumb
                    color: Theme.wellDark
                    radius: 2
                    Text {
                        anchors.centerIn: parent
                        text: tile.develop !== undefined && tile.develop.state === "developed"
                              ? "显影过" : "无预览"
                        color: Theme.muted
                        font.pixelSize: 10
                    }
                }

                // The badge line: stars, the colour dot, the develop ring.
                Row {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.margins: gridPage.inset
                    spacing: 4
                    height: 14
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: tile.rating > 0
                        text: "★★★★★".slice(0, Math.max(0, tile.rating))
                        color: Theme.accent
                        font.pixelSize: 11
                    }
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: tile.colorTag > 0
                        width: 9; height: 9; radius: 4.5
                        color: tile.colorTag <= 7 ? gridPage.labelColors[tile.colorTag] : "transparent"
                        border.color: Theme.line
                        border.width: tile.colorTag > 7 ? 1 : 0
                    }
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: tile.develop !== undefined && tile.develop.state === "developed"
                        width: 10; height: 10; radius: 5
                        color: "transparent"
                        border.color: Theme.accent
                        border.width: 2
                    }
                }
                Text {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 16
                    anchors.leftMargin: gridPage.inset
                    anchors.rightMargin: gridPage.inset
                    text: tile.fileName
                    color: Theme.text
                    font.pixelSize: 11
                    elide: Text.ElideMiddle
                    horizontalAlignment: Text.AlignHCenter
                }

                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton
                    onClicked: function(mouse) {
                        grid.forceActiveFocus()
                        grid.currentIndex = tile.index
                        if (mouse.modifiers & Qt.ShiftModifier) library.selectRange(tile.index)
                        else if (mouse.modifiers & Qt.ControlModifier) library.toggleSelected(tile.index)
                        else library.selectOnly(tile.index)
                    }
                    onDoubleClicked: gridPage.openRequested(tile.path)
                }
            }

            // The empty state: no source, or a source with no rows.
            Column {
                anchors.centerIn: parent
                visible: library.rowCount() === 0
                spacing: 10
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: library.sources.length === 0 ? "库是空的" : "此来源还没有照片"
                    color: "#e5e2dc"
                    font.pixelSize: 20
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: library.sources.length === 0
                          ? "新建目录或会话后导入照片"
                          : "从侧栏选择收藏集，或将照片导入该来源"
                    color: "#c0bdb7"
                    font.pixelSize: 12
                }
            }
        }

        // The info strip (every mode carries one, §8): the same facts the
        // develop page's strip carries, for the grid.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 28
            color: Theme.card
            Text {
                objectName: "libraryInfoStrip"
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                verticalAlignment: Text.AlignVCenter
                text: (library.hasClipboard ? "剪贴板：" + library.clipboardSourceName + "  ·  " : "") +
                      (library.lastError.length > 0 ? library.lastError : "网格 · 点击选择，双击进入冲洗")
                color: library.lastError.length > 0 ? "#ffb39f" : Theme.muted
                font.pixelSize: 10
                elide: Text.ElideRight
            }
        }
    }
}
