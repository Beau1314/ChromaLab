import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import SpektraLab.Native 1.0

ApplicationWindow {
    id: window
    objectName: "editorWindow"
    width: 1540; height: 980
    minimumWidth: 1100; minimumHeight: 740
    visible: true
    color: Theme.card
    title: (preview.hasFrame ? preview.fileName + " — " : "") + "SpektraLab"
    font.family: "Segoe UI"
    font.pixelSize: 12
    palette.window: Theme.card
    palette.windowText: Theme.text
    palette.text: Theme.text
    palette.buttonText: Theme.text
    palette.base: Theme.well
    palette.button: Theme.well
    palette.highlight: Theme.accent
    property bool closePending: false
    property bool cropMode: false
    property rect draftCrop: Qt.rect(0, 0, 1, 1)
    property int exportFormat: 0
    property int exportQuality: 95
    property int exportLongEdge: 0
    readonly property bool canEditCrop: preview.hasFrame && !preview.busy && !preview.batchExporting
    readonly property bool canBrowse: preview.ready && !preview.busy && !preview.batchExporting && !cropMode
    // Text controls keep their own undo stack while they own keyboard focus.
    readonly property bool editingText: activeFocusItem !== null && typeof activeFocusItem.text === "string" && typeof activeFocusItem.undo === "function"
    onCropModeChanged: updateDialogState()

    function updateDialogState() {
        preview.setDialogOpen(cropMode || openDialog.visible || saveDialog.visible || batchDialog.visible || exportPopup.visible)
    }
    function startCrop() {
        if (!canEditCrop) return
        draftCrop = preview.cropRect
        cropMode = true
        Qt.callLater(function() { canvas.resetView(true) })
    }
    function finishCrop() {
        var next = draftCrop
        var commit = cropMode && canEditCrop
        cropMode = false
        if (commit) preview.setCropRect(next)
        Qt.callLater(function() { canvas.resetView(true) })
    }
    function cancelCrop() { cropMode = false; canvas.resetView(true) }
    function filmGroup(positive) {
        var rows = []
        for (var i = 0; i < preview.films.length; ++i)
            if (preview.films[i].positive === positive)
                rows.push({ "name": preview.films[i].name, "catalogIndex": i })
        return rows
    }
    function setLayer2Zero(names) {
        // Unchanged fields no-op inside setLayer2, so a reset only records
        // undo steps for the fields that actually moved.
        for (var i = 0; i < names.length; ++i) preview.setLayer2(names[i], 0)
    }
    function resetColorBalance() {
        for (var zone = 0; zone < 4; ++zone)
            for (var axis = 0; axis < 3; ++axis)
                preview.setZone(zone, axis, 0)
    }
    function openExport() { if (preview.hasFrame && !preview.busy && !cropMode) exportPopup.open() }
    function showExportOptions() { openExport() }
    function formatFilter() {
        if (exportFormat === 3) return ["JPEG 图像 (*.jpg *.jpeg)"]
        if (exportFormat === 1 || exportFormat === 2) return ["PNG 图像 (*.png)"]
        return ["16 位 TIFF (*.tif *.tiff)"]
    }
    onClosing: function(event) {
        if (preview.batchExporting) preview.cancelExport()
        if (preview.busy) { closePending = true; event.accepted = false }
    }
    Connections {
        target: preview
        function onStateChanged() { if (window.closePending && !preview.busy) window.close() }
        function onSourceOpened() { window.cropMode = false; canvas.resetView(true) }
    }
    FileDialog {
        id: openDialog
        title: "导入一张或多张 RAW 照片"
        fileMode: FileDialog.OpenFiles
        nameFilters: ["相机 RAW (*.arw *.nef *.nrw *.dng *.cr2 *.cr3 *.raf *.rw2 *.orf *.pef *.srw)", "所有文件 (*)"]
        onVisibleChanged: window.updateDialogState()
        onAccepted: preview.openFiles(selectedFiles)
    }
    FileDialog {
        id: saveDialog
        title: "导出当前画面 · 不覆盖现有文件"
        fileMode: FileDialog.SaveFile
        nameFilters: window.formatFilter()
        defaultSuffix: window.exportFormat === 3 ? "jpg" : window.exportFormat === 0 ? "tif" : "png"
        currentFolder: preview.sourceFolder
        onVisibleChanged: window.updateDialogState()
        onAccepted: preview.exportImage(selectedFile, window.exportFormat, window.exportQuality, window.exportLongEdge)
    }
    FolderDialog {
        id: batchDialog
        title: "选择批量导出文件夹 · 跳过已有文件"
        currentFolder: preview.sourceFolder
        onVisibleChanged: window.updateDialogState()
        onAccepted: preview.exportBatch(selectedFolder, window.exportFormat, window.exportQuality, window.exportLongEdge)
    }
    Shortcut { sequences: [StandardKey.Open]; enabled: window.canBrowse; onActivated: openDialog.open() }
    Shortcut { sequence: "Ctrl+E"; enabled: preview.hasFrame && !preview.busy && !window.cropMode; onActivated: window.openExport() }
    Shortcut { sequence: "Ctrl+0"; enabled: preview.hasFrame; onActivated: canvas.resetView(true) }
    Shortcut { sequence: "Ctrl+1"; enabled: preview.hasFrame && !window.cropMode; onActivated: canvas.resetView(false) }
    // Split compare toggles like a view, not an edit; text fields keep V.
    Shortcut { sequence: "V"; enabled: preview.hasFrame && !window.cropMode && !window.editingText; onActivated: preview.compareEnabled = !preview.compareEnabled }
    Shortcut { sequence: "Ctrl+Z"; enabled: preview.settingsEditable && preview.canUndo && !window.cropMode && !window.editingText; onActivated: preview.undo() }
    Shortcut { sequences: ["Ctrl+Shift+Z", "Ctrl+Y"]; enabled: preview.settingsEditable && preview.canRedo && !window.cropMode && !window.editingText; onActivated: preview.redo() }
    Shortcut { sequence: "Escape"; enabled: window.cropMode; onActivated: window.cancelCrop() }
    Shortcut { sequence: "Return"; enabled: window.cropMode; onActivated: window.finishCrop() }

    component Note: Text {
        Layout.fillWidth: true
        color: Theme.muted
        font.pixelSize: 10
        wrapMode: Text.WordWrap
        lineHeight: 1.35
    }
    component Adjustment: ColumnLayout {
        id: adjustment
        property string label
        property string setting
        property real from: 0
        property real to: 2
        property real step: 0.05
        property int decimals: 2
        property string suffix: ""
        property real fallback: 0
        property bool available: true
        readonly property real current: preview.adjustments[setting] === undefined ? fallback : Number(preview.adjustments[setting])
        Layout.fillWidth: true; spacing: 2
        RowLayout {
            Layout.fillWidth: true
            Text { text: adjustment.label; color: adjustment.available ? Theme.text : Theme.muted; font.pixelSize: 11 }
            Item { Layout.fillWidth: true }
            Text { text: adjustment.current.toFixed(adjustment.decimals) + adjustment.suffix; color: Theme.muted; font.pixelSize: 10 }
        }
        Slider {
            Layout.fillWidth: true; implicitHeight: 24
            from: adjustment.from; to: adjustment.to; stepSize: adjustment.step
            value: adjustment.current
            enabled: preview.settingsEditable && adjustment.available
            onPressedChanged: {
                if (pressed) preview.beginEditGesture(adjustment.setting)
                else preview.endEditGesture()
            }
            onMoved: preview.setAdjustment(adjustment.setting, value)
        }
    }
    component Layer2Adjustment: ColumnLayout {
        id: layer2Adjustment
        property string label
        property string setting
        // Dotted read path into preview.layer2. The scalars are flat, but the
        // vignette is published nested ({vignette: {amount, midpoint}}) while
        // setLayer2 keeps the flat "vignetteAmount"/"vignetteMidpoint" names.
        property string readKey: setting
        property real from: 0
        property real to: 2
        property real step: 0.05
        property int decimals: 2
        property string suffix: ""
        property real fallback: 0
        property bool available: preview.hasFrame
        readonly property real current: {
            var node = preview.layer2
            var parts = layer2Adjustment.readKey.split(".")
            for (var i = 0; i < parts.length; ++i) {
                if (node === undefined || node === null) return layer2Adjustment.fallback
                node = node[parts[i]]
            }
            return node === undefined ? layer2Adjustment.fallback : Number(node)
        }
        Layout.fillWidth: true; spacing: 2
        RowLayout {
            Layout.fillWidth: true
            Text { text: layer2Adjustment.label; color: layer2Adjustment.available ? Theme.text : Theme.muted; font.pixelSize: 11 }
            Item { Layout.fillWidth: true }
            Text { text: layer2Adjustment.current.toFixed(layer2Adjustment.decimals) + layer2Adjustment.suffix; color: Theme.muted; font.pixelSize: 10 }
        }
        Slider {
            Layout.fillWidth: true; implicitHeight: 24
            from: layer2Adjustment.from; to: layer2Adjustment.to; stepSize: layer2Adjustment.step
            value: layer2Adjustment.current
            enabled: preview.settingsEditable && layer2Adjustment.available
            onPressedChanged: {
                // The gesture key is the setLayer2 name, matching the edit key
                // changedLayer2 uses, so a whole drag is one undo step.
                if (pressed) preview.beginEditGesture(layer2Adjustment.setting)
                else preview.endEditGesture()
            }
            onMoved: preview.setLayer2(layer2Adjustment.setting, value)
        }
    }
    component Effect: ColumnLayout {
        id: effect
        property string label
        property string activeKey
        property string amountKey
        property real maximum: 2
        Layout.fillWidth: true; spacing: 1
        CheckBox {
            text: effect.label; font.pixelSize: 11
            palette.windowText: Theme.text
            checked: preview.adjustments[effect.activeKey] === undefined ? true : preview.adjustments[effect.activeKey]
            enabled: preview.settingsEditable
            onToggled: preview.setAdjustment(effect.activeKey, checked)
        }
        Adjustment { label: "强度"; setting: effect.amountKey; fallback: 1; from: 0; to: effect.maximum; available: preview.adjustments[effect.activeKey] !== false }
    }
    component FilmGroup: ColumnLayout {
        id: group
        property bool positive: false
        Layout.fillWidth: true; spacing: 3
        Text { text: group.positive ? "反转片 · Positive" : "负片 · Negative"; color: Theme.muted; font.pixelSize: 10; font.bold: true }
        ListView {
            id: list
            objectName: group.positive ? "positiveFilmList" : "filmList"
            Layout.fillWidth: true; Layout.preferredHeight: group.positive ? 112 : 174
            model: window.filmGroup(group.positive); clip: true
            currentIndex: {
                for (var i = 0; i < model.length; ++i)
                    if (model[i].catalogIndex === preview.filmIndex) return i
                return -1
            }
            onCurrentIndexChanged: { if (currentIndex >= 0) positionViewAtIndex(currentIndex, ListView.Contain) }
            onCountChanged: Qt.callLater(function() { if (list.currentIndex >= 0) list.positionViewAtIndex(list.currentIndex, ListView.Contain) })
            ScrollBar.vertical: ScrollBar {}
            delegate: Rectangle {
                required property var modelData
                width: list.width; height: 27; radius: 4
                color: modelData.catalogIndex === preview.filmIndex ? Theme.selection : filmMouse.containsMouse ? "#41413e" : "transparent"
                Text {
                    anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 6
                    verticalAlignment: Text.AlignVCenter; text: modelData.name
                    color: modelData.catalogIndex === preview.filmIndex ? Theme.selectionText : preview.settingsEditable ? Theme.text : Theme.muted
                    font.pixelSize: 10; elide: Text.ElideRight
                }
                MouseArea { id: filmMouse; anchors.fill: parent; hoverEnabled: true; enabled: preview.settingsEditable; onClicked: preview.filmIndex = modelData.catalogIndex }
            }
        }
    }

    RowLayout {
        anchors.fill: parent; spacing: 1
        Rectangle {
            Layout.preferredWidth: Theme.leftWidth; Layout.fillHeight: true; color: Theme.card
            ColumnLayout {
                anchors.fill: parent; spacing: 0
                Text { text: "胶片与相纸"; color: Theme.text; font.pixelSize: 14; font.bold: true; Layout.leftMargin: 14; Layout.preferredHeight: Theme.topHeight; verticalAlignment: Text.AlignVCenter }
                ScrollView {
                    Layout.fillWidth: true; Layout.fillHeight: true; clip: true; contentWidth: availableWidth
                    ColumnLayout {
                        width: parent.width; spacing: 0
                        Section {
                            title: "导航图"
                            Rectangle {
                                Layout.fillWidth: true; Layout.preferredHeight: 132; color: Theme.wellDark
                                FrameCanvas { anchors.fill: parent; anchors.margins: 6; image: preview.previewImage; enabled: false }
                                Text { anchors.centerIn: parent; visible: !preview.hasFrame; text: "导入照片开始"; color: Theme.muted; font.pixelSize: 11 }
                            }
                            RowLayout {
                                ActionButton { text: "适合"; implicitHeight: 23; enabled: preview.hasFrame; onClicked: canvas.resetView(true) }
                                Item { Layout.fillWidth: true }
                                Text { text: preview.hasFrame ? canvas.scalePercent.toFixed(1) + "%" : ""; color: Theme.muted; font.pixelSize: 10 }
                            }
                        }
                        Section {
                            title: "胶片"
                            FilmGroup { positive: true }
                            FilmGroup { positive: false }
                        }
                        Section {
                            title: "相纸"
                            Note { visible: preview.filmIsPositive; text: "反转片直接扫描，不使用相纸。"; color: Theme.accent }
                            ListView {
                                id: papers; objectName: "paperList"
                                Layout.fillWidth: true; Layout.preferredHeight: 174; clip: true
                                model: preview.papers; currentIndex: preview.paperIndex
                                onCurrentIndexChanged: positionViewAtIndex(currentIndex, ListView.Contain)
                                onCountChanged: Qt.callLater(function() { papers.positionViewAtIndex(preview.paperIndex, ListView.Contain) })
                                ScrollBar.vertical: ScrollBar {}
                                delegate: Rectangle {
                                    required property var modelData
                                    required property int index
                                    width: papers.width; height: 27; radius: 4
                                    color: !preview.filmIsPositive && index === preview.paperIndex ? Theme.selection : paperMouse.containsMouse && paperMouse.enabled ? "#41413e" : "transparent"
                                    Text {
                                        anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 6; verticalAlignment: Text.AlignVCenter; text: modelData.name
                                        color: !preview.filmIsPositive && index === preview.paperIndex ? Theme.selectionText : preview.settingsEditable && !preview.filmIsPositive ? Theme.text : Theme.muted
                                        font.pixelSize: 10; elide: Text.ElideRight
                                    }
                                    MouseArea { id: paperMouse; anchors.fill: parent; hoverEnabled: true; enabled: preview.settingsEditable && !preview.filmIsPositive; onClicked: preview.paperIndex = index }
                                }
                            }
                        }
                        Section {
                            title: "裁切"
                            RowLayout {
                                Layout.fillWidth: true
                                ComboBox {
                                    id: cropAspect; Layout.fillWidth: true
                                    model: ["自由比例", "1:1", "3:2", "4:3", "16:9"]; enabled: window.canEditCrop
                                    currentIndex: {
                                        var c = preview.cropRect
                                        if (!preview.hasFrame || (c.x === 0 && c.y === 0 && c.width === 1 && c.height === 1)) return 0
                                        var ratio = c.width * preview.metadata.sourceWidth / (c.height * preview.metadata.sourceHeight)
                                        if (preview.quarterTurns % 2 !== 0) ratio = 1 / ratio
                                        var values = [0, 1, 1.5, 4 / 3, 16 / 9]
                                        for (var i = 1; i < values.length; ++i)
                                            if (Math.abs(ratio - values[i]) < 0.002) return i
                                        return 0
                                    }
                                    onActivated: { window.cropMode = false; preview.setCropAspect([0, 1, 1.5, 4 / 3, 16 / 9][currentIndex]); canvas.resetView(true) }
                                }
                                ActionButton { text: "重置"; enabled: window.canEditCrop; onClicked: { window.cropMode = false; preview.resetCrop(); canvas.resetView(true) } }
                            }
                            ActionButton { Layout.fillWidth: true; text: window.cropMode ? "完成裁切" : "在画面中拖选裁切"; accented: window.cropMode; enabled: window.canEditCrop; onClicked: window.cropMode ? window.finishCrop() : window.startCrop() }
                            Note { text: window.cropMode ? "在未旋转原图上拖出矩形；完成后恢复方向与拉直。Enter 确认，Esc 取消。" : "预览与导出使用同一裁切与方向；原始 RAW 不变。" }
                            RowLayout {
                                Layout.fillWidth: true
                                ActionButton { objectName: "rotateLeftButton"; Layout.fillWidth: true; text: "左转 90°"; enabled: window.canEditCrop && !window.cropMode; onClicked: { preview.rotateCounterClockwise(); canvas.resetView(true) } }
                                ActionButton { objectName: "rotateRightButton"; Layout.fillWidth: true; text: "右转 90°"; enabled: window.canEditCrop && !window.cropMode; onClicked: { preview.rotateClockwise(); canvas.resetView(true) } }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                ActionButton { objectName: "flipHorizontalButton"; Layout.fillWidth: true; text: "水平翻转"; accented: preview.flipHorizontal; enabled: window.canEditCrop && !window.cropMode; onClicked: preview.setFlipHorizontal(!preview.flipHorizontal) }
                                ActionButton { objectName: "flipVerticalButton"; Layout.fillWidth: true; text: "垂直翻转"; accented: preview.flipVertical; enabled: window.canEditCrop && !window.cropMode; onClicked: preview.setFlipVertical(!preview.flipVertical) }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: "拉直"; color: Theme.text; font.pixelSize: 11 }
                                Item { Layout.fillWidth: true }
                                Text { text: (straightenSlider.pressed ? straightenSlider.from + straightenSlider.position * (straightenSlider.to - straightenSlider.from) : preview.straightenDegrees).toFixed(1) + "°"; color: Theme.muted; font.pixelSize: 10 }
                            }
                            Slider {
                                id: straightenSlider; objectName: "straightenSlider"
                                Layout.fillWidth: true; implicitHeight: 24
                                from: -45; to: 45; stepSize: 0.1; live: false
                                value: preview.straightenDegrees
                                enabled: window.canEditCrop && !window.cropMode
                                onPressedChanged: {
                                    if (pressed) preview.beginEditGesture("straightenDegrees")
                                    else {
                                        preview.setStraightenDegrees(Math.round((from + position * (to - from)) * 10) / 10)
                                        preview.endEditGesture()
                                    }
                                }
                                onMoved: { if (!pressed) preview.setStraightenDegrees(value) }
                            }
                            Note { text: "拉直在松开滑块后应用。边缘自动收紧以避免空白。" }
                            ActionButton { objectName: "resetGeometryButton"; Layout.fillWidth: true; text: "重置裁切与方向"; enabled: window.canEditCrop && !window.cropMode; onClicked: { preview.resetGeometry(); canvas.resetView(true) } }
                        }
                        Section {
                            title: "放大机 · 相纸亮度"
                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: "亮度"; color: Theme.text; font.pixelSize: 11 }
                                Item { Layout.fillWidth: true }
                                Text { text: preview.filmIsPositive ? "不适用" : (preview.exposureEv >= 0 ? "+" : "") + preview.exposureEv.toFixed(1) + " EV"; color: Theme.muted; font.pixelSize: 10 }
                            }
                            Slider {
                                objectName: "exposureSlider"; Layout.fillWidth: true; from: -2; to: 2; stepSize: 0.1; implicitHeight: 24
                                value: preview.exposureEv; enabled: preview.settingsEditable && !preview.filmIsPositive
                                onPressedChanged: { if (pressed) preview.beginEditGesture("exposureEv"); else preview.endEditGesture() }
                                onMoved: preview.exposureEv = value
                            }
                            Adjustment { label: "黄色滤镜"; setting: "yFilterShift"; from: -1; to: 1; step: 0.01; decimals: 2; available: !preview.filmIsPositive }
                            Adjustment { label: "洋红滤镜"; setting: "mFilterShift"; from: -1; to: 1; step: 0.01; decimals: 2; available: !preview.filmIsPositive }
                        }
                    }
                }
            }
        }
        Rectangle {
            Layout.fillWidth: true; Layout.fillHeight: true; color: Theme.ground
            ColumnLayout {
                anchors.fill: parent; spacing: 0
                Rectangle {
                    Layout.fillWidth: true; implicitHeight: Theme.topHeight; color: Theme.card
                    RowLayout {
                        anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 8; spacing: 6
                        ActionButton { text: "导入 RAW"; enabled: window.canBrowse; onClicked: openDialog.open(); implicitHeight: 27 }
                        ActionButton { text: "导出"; enabled: preview.hasFrame && !preview.busy && !window.cropMode; onClicked: window.openExport(); implicitHeight: 27 }
                        ActionButton { text: "裁切"; accented: window.cropMode; enabled: window.canEditCrop; onClicked: window.cropMode ? window.finishCrop() : window.startCrop(); implicitHeight: 27 }
                        ActionButton { objectName: "compareButton"; text: "对比"; accented: preview.compareEnabled; enabled: preview.hasFrame && !window.cropMode; onClicked: preview.compareEnabled = !preview.compareEnabled; implicitHeight: 27; ToolTip.visible: hovered; ToolTip.text: "左侧显示未评分的冲印结果，右侧显示当前编辑 · V" }
                        ActionButton { objectName: "undoButton"; text: "撤销"; enabled: preview.settingsEditable && preview.canUndo && !window.cropMode; onClicked: preview.undo(); implicitHeight: 27; ToolTip.visible: hovered; ToolTip.text: "撤销当前照片的上一步调整 · Ctrl+Z" }
                        ActionButton { objectName: "redoButton"; text: "重做"; enabled: preview.settingsEditable && preview.canRedo && !window.cropMode; onClicked: preview.redo(); implicitHeight: 27; ToolTip.visible: hovered; ToolTip.text: "重做当前照片的调整 · Ctrl+Shift+Z / Ctrl+Y" }
                        Item { Layout.fillWidth: true }
                        Text { text: preview.hasFrame ? canvas.scalePercent.toFixed(1) + "%" : ""; color: Theme.muted; font.pixelSize: 10; horizontalAlignment: Text.AlignRight }
                        ActionButton { text: "适合"; accented: canvas.fit; enabled: preview.hasFrame; onClicked: canvas.resetView(true); implicitHeight: 27 }
                        ActionButton { text: "100%"; accented: !canvas.fit; enabled: preview.hasFrame && !window.cropMode; onClicked: canvas.resetView(false); implicitHeight: 27 }
                    }
                }
                Item {
                    Layout.fillWidth: true; Layout.fillHeight: true
                    FrameCanvas {
                        id: canvas; objectName: "frameCanvas"; anchors.fill: parent; anchors.margins: 18
                        image: window.cropMode ? preview.uncroppedImage : preview.previewImage
                        beforeImage: preview.beforeImage
                        compareEnabled: preview.compareEnabled && !window.cropMode
                        enabled: !window.cropMode
                    }
                    Item {
                        id: cropOverlay
                        objectName: "cropOverlay"
                        anchors.fill: canvas
                        visible: window.cropMode
                        readonly property rect imageBox: canvas.imageRect
                        readonly property rect selectionBox: Qt.rect(imageBox.x + window.draftCrop.x * imageBox.width, imageBox.y + window.draftCrop.y * imageBox.height, window.draftCrop.width * imageBox.width, window.draftCrop.height * imageBox.height)
                        Rectangle { x: cropOverlay.imageBox.x; y: cropOverlay.imageBox.y; width: cropOverlay.imageBox.width; height: Math.max(0, cropOverlay.selectionBox.y - y); color: "#99000000" }
                        Rectangle { x: cropOverlay.imageBox.x; y: cropOverlay.selectionBox.y + cropOverlay.selectionBox.height; width: cropOverlay.imageBox.width; height: Math.max(0, cropOverlay.imageBox.y + cropOverlay.imageBox.height - y); color: "#99000000" }
                        Rectangle { x: cropOverlay.imageBox.x; y: cropOverlay.selectionBox.y; width: Math.max(0, cropOverlay.selectionBox.x - x); height: cropOverlay.selectionBox.height; color: "#99000000" }
                        Rectangle { x: cropOverlay.selectionBox.x + cropOverlay.selectionBox.width; y: cropOverlay.selectionBox.y; width: Math.max(0, cropOverlay.imageBox.x + cropOverlay.imageBox.width - x); height: cropOverlay.selectionBox.height; color: "#99000000" }
                        Rectangle {
                            x: cropOverlay.selectionBox.x; y: cropOverlay.selectionBox.y; width: cropOverlay.selectionBox.width; height: cropOverlay.selectionBox.height
                            color: "transparent"; border.color: Theme.accent; border.width: 1
                            Rectangle { x: parent.width / 3; width: 1; height: parent.height; color: "#70ffffff" }
                            Rectangle { x: parent.width * 2 / 3; width: 1; height: parent.height; color: "#70ffffff" }
                            Rectangle { y: parent.height / 3; height: 1; width: parent.width; color: "#70ffffff" }
                            Rectangle { y: parent.height * 2 / 3; height: 1; width: parent.width; color: "#70ffffff" }
                        }
                        MouseArea {
                            anchors.fill: parent; enabled: window.canEditCrop; cursorShape: Qt.CrossCursor
                            property point origin
                            property bool drawing: false
                            function normalized(x, y) {
                                return Qt.point(Math.max(0, Math.min(1, (x - cropOverlay.imageBox.x) / cropOverlay.imageBox.width)), Math.max(0, Math.min(1, (y - cropOverlay.imageBox.y) / cropOverlay.imageBox.height)))
                            }
                            function updateCrop(x, y) {
                                var p = normalized(x, y)
                                window.draftCrop = Qt.rect(Math.min(origin.x, p.x), Math.min(origin.y, p.y), Math.abs(p.x - origin.x), Math.abs(p.y - origin.y))
                            }
                            onPressed: function(mouse) {
                                var b = cropOverlay.imageBox
                                if (mouse.x < b.x || mouse.y < b.y || mouse.x > b.x + b.width || mouse.y > b.y + b.height) { mouse.accepted = false; return }
                                origin = normalized(mouse.x, mouse.y); drawing = true
                            }
                            onPositionChanged: function(mouse) { if (drawing) updateCrop(mouse.x, mouse.y) }
                            onReleased: function(mouse) {
                                if (!drawing) return
                                updateCrop(mouse.x, mouse.y); drawing = false
                                if (window.draftCrop.width > 0.003 && window.draftCrop.height > 0.003) window.finishCrop()
                                else window.draftCrop = preview.cropRect
                            }
                            onCanceled: { drawing = false; window.draftCrop = preview.cropRect }
                        }
                    }
                    // Split compare overlay: a line at comparePosition over the
                    // image box, with a narrow grab strip that retargets the
                    // split on press and drags it. The composite itself is the
                    // canvas's; this only mirrors where the split sits.
                    Item {
                        id: compareOverlay
                        objectName: "compareOverlay"
                        anchors.fill: canvas
                        visible: preview.compareEnabled && !window.cropMode && preview.hasFrame
                        readonly property rect box: canvas.imageRect
                        Rectangle {
                            id: splitLine; objectName: "compareSplitLine"
                            width: 2; radius: 1; color: "#e8e5df"
                            x: Math.round(compareOverlay.box.x + canvas.comparePosition * compareOverlay.box.width - width / 2)
                            y: compareOverlay.box.y; height: compareOverlay.box.height
                        }
                        MouseArea {
                            objectName: "compareDrag"
                            width: 16
                            x: splitLine.x + splitLine.width / 2 - width / 2
                            anchors.top: parent.top; anchors.bottom: parent.bottom
                            cursorShape: Qt.SplitHCursor
                            function retarget(mouseX) {
                                var b = compareOverlay.box
                                canvas.comparePosition = Math.max(0, Math.min(1, (mouseX - b.x) / b.width))
                            }
                            onPressed: function(mouse) { retarget(mouse.x) }
                            onPositionChanged: function(mouse) { if (pressed) retarget(mouse.x) }
                        }
                    }
                    Column {
                        anchors.centerIn: parent; spacing: 16; visible: !preview.hasFrame
                        Text { anchors.horizontalCenter: parent.horizontalCenter; text: "SpektraLab"; color: "#e5e2dc"; font.pixelSize: 32; font.letterSpacing: 1 }
                        Text { anchors.horizontalCenter: parent.horizontalCenter; text: "导入 RAW，逐张选择胶片与相纸"; color: "#d0cdc6"; font.pixelSize: 13 }
                        Text { anchors.horizontalCenter: parent.horizontalCenter; text: "支持一次拖入多张照片"; color: "#c0bdb7"; font.pixelSize: 11 }
                    }
                    DropArea { anchors.fill: parent; enabled: window.canBrowse; onDropped: function(drop) { if (drop.hasUrls) preview.openFiles(drop.urls) } }
                    Rectangle {
                        anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 26
                        width: window.cropMode ? 78 : 48; height: 20; radius: 2; color: "#b32d2d2c"; visible: preview.hasFrame
                        Text { anchors.centerIn: parent; text: window.cropMode ? "裁切原图" : "FULL"; color: Theme.text; font.pixelSize: 9; font.letterSpacing: 1 }
                    }
                    BusyIndicator { anchors.centerIn: parent; running: preview.busy; visible: running; width: 44; height: 44 }
                }
                Rectangle {
                    Layout.fillWidth: true; implicitHeight: 28; color: Theme.card
                    Text { anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 12; verticalAlignment: Text.AlignVCenter; text: preview.hasFrame ? preview.metadata.width + " × " + preview.metadata.height + "   ·   SDR sRGB / 8 位预览   ·   " + (preview.dirty ? "更新预览中…" : "选择与调整自动应用") : "完整分辨率预览"; color: preview.dirty ? Theme.accent : Theme.muted; font.pixelSize: 10; elide: Text.ElideRight }
                }
                Rectangle {
                    Layout.fillWidth: true; implicitHeight: Theme.stripHeight; color: Theme.card
                    Rectangle { width: parent.width; height: 1; color: Theme.line; opacity: 0.5 }
                    ColumnLayout {
                        anchors.fill: parent; anchors.margins: 8; spacing: 6
                        RowLayout {
                            Layout.fillWidth: true; spacing: 6
                            Text { text: preview.libraryItems.length + " 张照片 · 已选 " + preview.selectedCount; color: Theme.muted; font.pixelSize: 10 }
                            Item { Layout.fillWidth: true }
                            ActionButton { text: "全选"; implicitHeight: 23; font.pixelSize: 10; enabled: window.canBrowse && preview.libraryItems.length > 0; onClicked: preview.selectAllPhotos(true) }
                            ActionButton { text: "清除选择"; implicitHeight: 23; font.pixelSize: 10; enabled: window.canBrowse && preview.selectedCount > 0; onClicked: preview.selectAllPhotos(false) }
                            ActionButton { text: "同步设置"; implicitHeight: 23; font.pixelSize: 10; enabled: window.canBrowse && preview.hasFrame && preview.selectedCount > 0; onClicked: preview.syncSelectedSettings(false); ToolTip.visible: hovered; ToolTip.text: "将当前照片的胶片、相纸、解码和效果同步到所选照片，不同步裁切。" }
                        }
                        ListView {
                            id: filmstrip; objectName: "photoStrip"
                            Layout.fillWidth: true; Layout.fillHeight: true
                            orientation: ListView.Horizontal; spacing: 8; clip: true
                            model: preview.libraryItems; currentIndex: preview.activeIndex
                            onCurrentIndexChanged: positionViewAtIndex(currentIndex, ListView.Contain)
                            ScrollBar.horizontal: ScrollBar {}
                            delegate: Rectangle {
                                required property var modelData
                                required property int index
                                width: 132; height: filmstrip.height - 3
                                color: Theme.wellDark; border.color: modelData.active ? Theme.accent : Theme.line; border.width: modelData.active ? 2 : 0
                                Text { anchors.centerIn: parent; anchors.verticalCenterOffset: -8; text: modelData.error ? "无法读取" : "点击预览"; color: modelData.error ? "#ffb39f" : Theme.muted; font.pixelSize: 10 }
                                FrameCanvas { anchors.fill: parent; anchors.margins: 5; anchors.bottomMargin: 22; image: modelData.thumbnail; enabled: false }
                                Text { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: 6; text: modelData.name; color: Theme.text; font.pixelSize: 10; elide: Text.ElideMiddle }
                                MouseArea {
                                    anchors.fill: parent; enabled: window.canBrowse; hoverEnabled: true
                                    onClicked: { window.cropMode = false; preview.selectImage(index) }
                                    ToolTip.visible: containsMouse && modelData.error.length > 0; ToolTip.text: modelData.error
                                }
                                CheckBox { anchors.top: parent.top; anchors.right: parent.right; padding: 0; scale: 0.8; checked: modelData.selected; enabled: window.canBrowse; onToggled: preview.setItemSelected(index, checked) }
                            }
                        }
                    }
                }
            }
        }
        Rectangle {
            Layout.preferredWidth: Theme.rightWidth; Layout.fillHeight: true; color: Theme.card
            ColumnLayout {
                anchors.fill: parent; spacing: 0
                Text { text: "参数 · 显影前"; color: Theme.text; font.pixelSize: 14; font.bold: true; Layout.leftMargin: 14; Layout.preferredHeight: Theme.topHeight; verticalAlignment: Text.AlignVCenter }
                ScrollView {
                    Layout.fillWidth: true; Layout.fillHeight: true; clip: true; contentWidth: availableWidth
                    ColumnLayout {
                        width: parent.width; spacing: 0
                        // Output histogram: 4×256 bins over the published
                        // preview pixels (geometry + grade applied), the same
                        // surface the canvas draws. Four normalized polylines —
                        // luma behind, R/G/B over it — against the max bin.
                        Section {
                            id: histogramSection
                            title: "直方图"
                            visible: preview.hasFrame
                            readonly property var bins: preview.histogram
                            // Indexed by channel (bins[0..3] = R/G/B/luma):
                            // R red, G green, B blue, luma gray. The paint
                            // loop's pass→channel mapping keeps the draw order
                            // luma behind, R/G/B over it.
                            readonly property var channelColors: ["#e8524e", "#62c462", "#5d7de8", "#bdbdbd"]
                            readonly property int peak: {
                                var top = 1
                                for (var c = 0; c < bins.length; ++c)
                                    for (var i = 0; i < bins[c].length; ++i)
                                        if (bins[c][i] > top) top = bins[c][i]
                                return top
                            }
                            onBinsChanged: plot.requestPaint()
                            onPeakChanged: plot.requestPaint()
                            Component.onCompleted: plot.requestPaint()
                            component HistogramKey: Row {
                                property string label
                                property color swatch
                                spacing: 3
                                Rectangle { width: 8; height: 8; radius: 2; color: parent.swatch; anchors.verticalCenter: parent.verticalCenter }
                                Text { text: parent.label; color: Theme.muted; font.pixelSize: 10; anchors.verticalCenter: parent.verticalCenter }
                            }
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 104
                                radius: 4
                                color: Theme.wellDark
                                clip: true
                                Canvas {
                                    id: plot
                                    objectName: "histogramCanvas"
                                    anchors.fill: parent
                                    anchors.margins: 6
                                    antialiasing: true
                                    onWidthChanged: requestPaint()
                                    onHeightChanged: requestPaint()
                                    onPaint: {
                                        var ctx = getContext("2d")
                                        ctx.clearRect(0, 0, width, height)
                                        ctx.lineWidth = 1
                                        // Desktop order: the luma channel behind, R/G/B over it.
                                        for (var pass = 0; pass < 4; ++pass) {
                                            var channel = pass === 0 ? 3 : pass - 1
                                            var row = histogramSection.bins[channel]
                                            if (row === undefined || row.length !== 256) continue
                                            ctx.strokeStyle = histogramSection.channelColors[channel]
                                            ctx.beginPath()
                                            for (var i = 0; i < 256; ++i) {
                                                var y = height - row[i] / histogramSection.peak * height
                                                if (i === 0) ctx.moveTo(0, y); else ctx.lineTo(i / 255 * width, y)
                                            }
                                            ctx.stroke()
                                        }
                                    }
                                }
                            }
                            Row {
                                spacing: 8
                                HistogramKey { label: "红"; swatch: "#e8524e" }
                                HistogramKey { label: "绿"; swatch: "#62c462" }
                                HistogramKey { label: "蓝"; swatch: "#5d7de8" }
                                HistogramKey { label: "亮度"; swatch: "#bdbdbd" }
                            }
                        }
                        // Scene latitude (RFC-023): an on-demand read-only
                        // measurement of the open session -- the medium's
                        // characteristic ramp, the scene's stop histogram
                        // behind it, and the three zones the medium's ISO
                        // boundaries cut the scene into. Windows supports the
                        // analysis; applying the solved mapping is refused by
                        // the engine, so this card only informs. Every reply
                        // key beyond medium/scene is read defensively: the
                        // canvas draws whichever layers arrived.
                        Section {
                            id: latitudeSection
                            title: "宽容度"
                            visible: preview.hasFrame
                            readonly property var reply: preview.latitude && preview.latitude.medium !== undefined ? preview.latitude : null
                            readonly property var medium: reply ? reply.medium : null
                            readonly property var hist: reply && reply.scene !== undefined && reply.scene.histogram !== undefined ? reply.scene.histogram : null
                            // The scene histogram's own axis (the engine's
                            // stop bins, [-16, 16] by default); the ramp and
                            // the zone boundaries share it.
                            readonly property real evLo: hist && hist.lo_ev !== undefined ? Number(hist.lo_ev) : -16
                            readonly property real evHi: hist && hist.hi_ev !== undefined ? Number(hist.hi_ev) : 16
                            readonly property string summary: {
                                var parts = []
                                var m = latitudeSection.medium
                                if (m && m.latitude_stops !== undefined)
                                    parts.push("宽容度 " + Number(m.latitude_stops).toFixed(1) + " stops")
                                var s = latitudeSection.reply && latitudeSection.reply.suggested !== undefined ? latitudeSection.reply.suggested : null
                                if (s && s.valid !== undefined)
                                    parts.push("建议收高光 " + Number(s.highlight_pull_back === undefined ? 0 : s.highlight_pull_back).toFixed(1) + " / 收阴影 " + Number(s.shadow_pull_back === undefined ? 0 : s.shadow_pull_back).toFixed(1) + " EV")
                                var fit = latitudeSection.reply && latitudeSection.reply.fit !== undefined ? latitudeSection.reply.fit : null
                                if (fit && fit.valid !== undefined && !fit.valid) parts.push("拟合未通过")
                                return parts.join(" · ")
                            }
                            onReplyChanged: latitudePlot.requestPaint()
                            onEvLoChanged: latitudePlot.requestPaint()
                            onEvHiChanged: latitudePlot.requestPaint()
                            Component.onCompleted: latitudePlot.requestPaint()
                            RowLayout {
                                Layout.fillWidth: true
                                ActionButton {
                                    id: latitudeButton
                                    objectName: "latitudeButton"
                                    text: preview.latitudeBusy ? "分析中…" : "分析"
                                    enabled: preview.hasFrame && !preview.busy
                                    onClicked: preview.requestLatitude()
                                }
                                Text { Layout.fillWidth: true; text: latitudeSection.summary; color: Theme.muted; font.pixelSize: 10; elide: Text.ElideRight }
                            }
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 96
                                radius: 4
                                color: Theme.wellDark
                                clip: true
                                Canvas {
                                    id: latitudePlot
                                    objectName: "latitudePlot"
                                    anchors.fill: parent
                                    anchors.margins: 6
                                    antialiasing: true
                                    onWidthChanged: requestPaint()
                                    onHeightChanged: requestPaint()
                                    onPaint: {
                                        var ctx = getContext("2d")
                                        ctx.clearRect(0, 0, width, height)
                                        var m = latitudeSection.medium
                                        var h = latitudeSection.hist
                                        var lo = latitudeSection.evLo, span = latitudeSection.evHi - lo
                                        if (span <= 0) return
                                        function evX(ev) { return (ev - lo) / span * width }
                                        // The three zones the medium's ISO
                                        // boundaries cut the scene axis into:
                                        // below (crushed shadows), within, and
                                        // above (blocked highlights).
                                        if (m && m.shadow_ev !== undefined && m.highlight_ev !== undefined) {
                                            var xs = Math.max(0, Math.min(width, evX(Number(m.shadow_ev))))
                                            var xh = Math.max(0, Math.min(width, evX(Number(m.highlight_ev))))
                                            ctx.fillStyle = "#2a5d7de8"
                                            ctx.fillRect(0, 0, xs, height)
                                            ctx.fillStyle = "#2ae8524e"
                                            ctx.fillRect(xh, 0, width - xh, height)
                                        }
                                        // The scene's stop histogram as the
                                        // backing silhouette, normalized to
                                        // its own peak.
                                        if (h && h.fractions !== undefined && h.fractions.length > 0) {
                                            var fr = h.fractions, peak = 0
                                            for (var i = 0; i < fr.length; ++i) peak = Math.max(peak, Number(fr[i]))
                                            if (peak > 0) {
                                                ctx.beginPath()
                                                ctx.moveTo(0, height)
                                                for (var i = 0; i < fr.length; ++i) {
                                                    var ev = lo + (i + 0.5) / fr.length * span
                                                    ctx.lineTo(evX(ev), height - Number(fr[i]) / peak * (height - 3))
                                                }
                                                ctx.lineTo(width, height)
                                                ctx.closePath()
                                                ctx.fillStyle = "#4abdbdbd"
                                                ctx.fill()
                                            }
                                        }
                                        // The medium's 128-point ramp: print
                                        // luminance against the scene's stop
                                        // axis, normalized to its own white.
                                        if (m && m.ramp_ev !== undefined && m.ramp_y !== undefined &&
                                            m.ramp_ev.length === m.ramp_y.length && m.ramp_ev.length > 1) {
                                            var yMax = 0
                                            for (var i = 0; i < m.ramp_y.length; ++i) yMax = Math.max(yMax, Number(m.ramp_y[i]))
                                            if (yMax > 0) {
                                                ctx.strokeStyle = "#e8e5df"
                                                ctx.lineWidth = 1.5
                                                ctx.beginPath()
                                                for (var i = 0; i < m.ramp_ev.length; ++i) {
                                                    var px = evX(Number(m.ramp_ev[i]))
                                                    var py = height - 2 - Number(m.ramp_y[i]) / yMax * (height - 6)
                                                    if (i === 0) ctx.moveTo(px, py); else ctx.lineTo(px, py)
                                                }
                                                ctx.stroke()
                                                // The measured boundaries the
                                                // zone shading above splits on.
                                                if (m.shadow_ev !== undefined && m.highlight_ev !== undefined) {
                                                    ctx.lineWidth = 1
                                                    ctx.strokeStyle = "#70e8e5df"
                                                    for (var i = 0; i < 2; ++i) {
                                                        var bx = evX(Number(i === 0 ? m.shadow_ev : m.highlight_ev))
                                                        if (bx >= 0 && bx <= width) {
                                                            ctx.beginPath()
                                                            ctx.moveTo(bx, 0); ctx.lineTo(bx, height)
                                                            ctx.stroke()
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                                Text {
                                    anchors.centerIn: parent
                                    visible: !latitudeSection.reply && !preview.latitudeBusy
                                    text: "点击「分析」测量当前胶片与场景"
                                    color: Theme.muted; font.pixelSize: 10
                                }
                            }
                            Note { visible: latitudeSection.summary.length > 0; text: latitudeSection.summary; color: Theme.text }
                            Note { text: "分析只读取当前显影结果，不改变画面；Windows 暂不支持应用求解的场景放置。" }
                        }
                        Section {
                            title: "编辑 · 光线"
                            Layer2Adjustment { label: "曝光"; setting: "exposure"; from: -5; to: 5; step: 0.05; decimals: 2; suffix: " EV" }
                            Layer2Adjustment { label: "亮度"; setting: "brightness"; from: -100; to: 100; step: 1; decimals: 0 }
                            Layer2Adjustment { label: "对比度"; setting: "contrast"; from: -100; to: 100; step: 1; decimals: 0 }
                            Layer2Adjustment { label: "高光"; setting: "highlights"; from: -100; to: 100; step: 1; decimals: 0 }
                            Layer2Adjustment { label: "阴影"; setting: "shadows"; from: -100; to: 100; step: 1; decimals: 0 }
                            Layer2Adjustment { label: "黑色阶"; setting: "blackPoint"; from: 0; to: 100; step: 1; decimals: 0 }
                            Layer2Adjustment { label: "白色阶"; setting: "whitePoint"; from: 0; to: 100; step: 1; decimals: 0 }
                            ActionButton { Layout.fillWidth: true; text: "重置光线"; enabled: preview.settingsEditable && preview.hasFrame; onClicked: window.setLayer2Zero(["exposure", "brightness", "contrast", "highlights", "shadows", "blackPoint", "whitePoint"]) }
                            Note { text: "分级调整作用于显影后的画面：实时生效，不触发重新渲染，与导出像素一致。" }
                        }
                        Section {
                            title: "编辑 · 颜色"
                            Layer2Adjustment { label: "色温"; setting: "temperature"; from: -100; to: 100; step: 1; decimals: 0 }
                            Layer2Adjustment { label: "色调"; setting: "tint"; from: -100; to: 100; step: 1; decimals: 0 }
                            Layer2Adjustment { label: "饱和度"; setting: "saturation"; from: -100; to: 100; step: 1; decimals: 0 }
                            ActionButton { Layout.fillWidth: true; text: "重置颜色"; enabled: preview.settingsEditable && preview.hasFrame; onClicked: window.setLayer2Zero(["temperature", "tint", "saturation"]) }
                        }
                        Section {
                            title: "曲线"
                            CurveEditor {}
                        }
                        Section {
                            title: "编辑 · 色彩平衡"
                            ColorBalance {}
                            ActionButton { Layout.fillWidth: true; text: "重置色彩平衡"; enabled: preview.settingsEditable && preview.hasFrame; onClicked: window.resetColorBalance() }
                        }
                        Section {
                            title: "编辑 · 暗角"
                            Layer2Adjustment { label: "数量"; setting: "vignetteAmount"; readKey: "vignette.amount"; from: -100; to: 100; step: 1; decimals: 0 }
                            Layer2Adjustment { label: "中点"; setting: "vignetteMidpoint"; readKey: "vignette.midpoint"; fallback: 50; from: 0; to: 100; step: 1; decimals: 0 }
                            ActionButton { Layout.fillWidth: true; text: "重置暗角"; enabled: preview.settingsEditable && preview.hasFrame; onClicked: { preview.setLayer2("vignetteAmount", 0); preview.setLayer2("vignetteMidpoint", 50) } }
                            Note { text: "负值压暗画面边缘，正值加亮；中点决定暗角从画面何处开始。" }
                        }
                        Section {
                            id: cameraSection
                            title: "输入 / 相机"
                            Adjustment { label: "胶片曝光"; setting: "filmExposureEv"; from: -4; to: 4; step: 0.1; decimals: 1; suffix: " EV" }
                            Note { text: "在胶片曝光前调整场景亮度。每张照片的效果与裁切自动保存，下次导入时恢复。" }
                            // 白平衡（RAW 解码侧的 Temperature/Tint，Adobe DNG 口径）：
                            // 0 = 自动（As Shot）。改动是解码参数，会重新解码当前照片
                            // 而不是增量重渲染；高光扩展解码不接受覆盖，控件随之禁用。
                            readonly property bool wbEditable: preview.hasFrame && preview.decodeMode === 0
                            readonly property bool wbAsShot: preview.wbTemperature <= 0
                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: "温度"; color: cameraSection.wbEditable ? Theme.text : Theme.muted; font.pixelSize: 11 }
                                Item { Layout.fillWidth: true }
                                Text { text: cameraSection.wbAsShot ? "自动（As Shot）" : Math.round(preview.wbTemperature) + " K"; color: Theme.muted; font.pixelSize: 10 }
                                ActionButton { text: "As Shot"; implicitHeight: 23; font.pixelSize: 10; enabled: preview.settingsEditable && cameraSection.wbEditable && preview.wbAsShotAvailable; onClicked: preview.applyWbAsShot(); ToolTip.visible: hovered; ToolTip.text: "回填照片解码回报的拍摄白平衡数值" }
                            }
                            Slider {
                                objectName: "wbTemperatureSlider"
                                Layout.fillWidth: true; implicitHeight: 24
                                from: 2000; to: 50000; stepSize: 50
                                value: cameraSection.wbAsShot ? 5500 : preview.wbTemperature
                                enabled: preview.settingsEditable && cameraSection.wbEditable
                                onPressedChanged: { if (pressed) preview.beginEditGesture("wb"); else preview.endEditGesture() }
                                onMoved: preview.setWb(value, preview.wbTint)
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: "色调"; color: cameraSection.wbEditable ? Theme.text : Theme.muted; font.pixelSize: 11 }
                                Item { Layout.fillWidth: true }
                                Text { text: cameraSection.wbAsShot ? "自动（As Shot）" : (preview.wbTint > 0 ? "+" : "") + Math.round(preview.wbTint); color: Theme.muted; font.pixelSize: 10 }
                                ActionButton { text: "As Shot"; implicitHeight: 23; font.pixelSize: 10; enabled: preview.settingsEditable && cameraSection.wbEditable && preview.wbAsShotAvailable; onClicked: preview.applyWbAsShot(); ToolTip.visible: hovered; ToolTip.text: "回填照片解码回报的拍摄白平衡数值" }
                            }
                            Slider {
                                objectName: "wbTintSlider"
                                Layout.fillWidth: true; implicitHeight: 24
                                from: -150; to: 150; stepSize: 1
                                value: preview.wbTint
                                enabled: preview.settingsEditable && cameraSection.wbEditable
                                onPressedChanged: { if (pressed) preview.beginEditGesture("wb"); else preview.endEditGesture() }
                                onMoved: preview.setWb(cameraSection.wbAsShot ? 5500 : preview.wbTemperature, value)
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: "白平衡"; color: cameraSection.wbEditable ? Theme.text : Theme.muted; font.pixelSize: 11 }
                                ComboBox {
                                    objectName: "wbPresetCombo"
                                    Layout.fillWidth: true
                                    model: ["自动（As Shot）", "日光 5500K", "阴天 6500K", "阴影 8000K", "钨丝灯 3200K", "荧光灯 4000K", "闪光灯 5500K"]
                                    readonly property var presets: ["", "daylight", "cloudy", "shade", "tungsten", "fluorescent", "flash"]
                                    enabled: preview.settingsEditable && cameraSection.wbEditable
                                    currentIndex: {
                                        var k = preview.wbTemperature, t = preview.wbTint
                                        if (k <= 0) return 0
                                        if (k === 5500 && t === 0) return 1
                                        if (k === 6500 && t === 0) return 2
                                        if (k === 8000 && t === 0) return 3
                                        if (k === 3200 && t === 0) return 4
                                        if (k === 4000 && t === 20) return 5
                                        if (k === 5500 && t === 10) return 6
                                        return -1
                                    }
                                    displayText: currentIndex < 0 ? "自定" : currentText
                                    onActivated: presets[currentIndex] === "" ? preview.resetWb() : preview.applyWbPreset(presets[currentIndex])
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Switch {
                                    id: meterSwitch
                                    text: "测光"; font.pixelSize: 11
                                    palette.windowText: Theme.text
                                    checked: preview.autoExposure
                                    enabled: preview.settingsEditable
                                    onToggled: preview.autoExposure = checked
                                }
                                Item { Layout.fillWidth: true }
                                Text { text: meterSwitch.checked ? "中心加权平均" : "关闭（不经测光放置场景）"; color: Theme.muted; font.pixelSize: 10 }
                            }
                            Note { text: "白平衡作用于 RAW 解码，改动会重新解码当前照片；As Shot 回填解码回报的拍摄白平衡数值。测光关闭时，胶片曝光直接决定场景放置。" }
                        }
                        Section {
                            title: "胶片画幅与效果"
                            RowLayout {
                                Text { text: "画幅长边"; color: Theme.text; font.pixelSize: 11 }
                                ComboBox {
                                    Layout.fillWidth: true
                                    model: ["18 mm", "24 mm", "35 mm（默认）", "36 mm（135）", "56 mm", "70 mm", "90 mm"]
                                    readonly property var lengths: [18, 24, 35, 36, 56, 70, 90]
                                    readonly property real actualLength: Number(preview.adjustments.filmFormatMm === undefined ? 35 : preview.adjustments.filmFormatMm)
                                    currentIndex: lengths.indexOf(actualLength)
                                    displayText: currentIndex < 0 ? actualLength.toFixed(1) + " mm（自定）" : currentText
                                    enabled: preview.settingsEditable
                                    onActivated: preview.setAdjustment("filmFormatMm", lengths[currentIndex])
                                }
                            }
                            Note { text: "物理画幅决定颗粒与光晕尺度；裁切不改变画幅。" }
                            Effect { label: "颗粒"; activeKey: "grainActive"; amountKey: "grainAmount" }
                            Effect { label: "光晕"; activeKey: "halationActive"; amountKey: "halationAmount"; maximum: 4 }
                            Effect { label: "眩光"; activeKey: "glareActive"; amountKey: "glareAmount"; maximum: 30 }
                        }
                        Section {
                            title: "RAW 解码"
                            ComboBox { id: decode; Layout.fillWidth: true; model: ["兼容模式（默认）", "高光扩展（实验）"]; currentIndex: preview.decodeMode; enabled: preview.settingsEditable; onActivated: preview.decodeMode = currentIndex }
                            Note { text: preview.decodeMode ? "保留去马赛克后的部分高光及负值；仍使用整数去马赛克。" : "与已验证的原生 RAW 流程保持一致。" }
                        }
                        Section {
                            title: "照片信息"
                            Note { text: preview.hasFrame ? preview.metadata.camera : "尚未打开照片"; color: Theme.text }
                            Note { text: preview.hasFrame ? preview.metadata.width + " × " + preview.metadata.height + " 像素\n" + (preview.metadata.mode === "headroom" ? "高光扩展解码" : "兼容解码") : "" }
                            Note { text: preview.hasFrame ? "本次渲染 " + (preview.metadata.renderMs / 1000).toFixed(2) + " 秒 · " + (preview.metadata.cached ? "复用负片" : "完整渲染") : "" }
                            ActionButton { Layout.fillWidth: true; text: "重置当前照片设置"; enabled: preview.settingsEditable; onClicked: preview.resetSettings() }
                        }
                        Section {
                            title: preview.batchExporting ? "批量导出" : preview.failed ? "无法完成操作" : "状态"
                            Note { text: window.closePending ? "等待当前操作完成后关闭…" : preview.status; color: preview.failed ? "#ffb39f" : Theme.muted }
                            Note { objectName: "savedStatusNote"; text: preview.savedStatus; visible: text.length > 0 }
                            Note { objectName: "persistenceWarningNote"; text: preview.persistenceWarning; visible: text.length > 0; color: "#ffb39f" }
                            ProgressBar { Layout.fillWidth: true; visible: preview.batchExporting; from: 0; to: Math.max(1, preview.batchTotal); value: preview.batchProgress }
                            ActionButton { Layout.fillWidth: true; text: "当前照片完成后停止"; visible: preview.batchExporting; onClicked: preview.cancelExport() }
                            ActionButton { Layout.fillWidth: true; text: "重新尝试预览"; visible: preview.failed && preview.hasFrame && !preview.batchExporting; enabled: !preview.busy; onClicked: preview.apply() }
                        }
                    }
                }
            }
        }
    }
    Popup {
        id: exportPopup
        objectName: "exportPopup"
        anchors.centerIn: parent
        width: 470; padding: 22
        modal: true; focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        onVisibleChanged: window.updateDialogState()
        background: Rectangle { color: Theme.card; border.color: Theme.line; radius: 7 }
        contentItem: ColumnLayout {
            spacing: 14
            Text { text: "导出照片"; color: Theme.text; font.pixelSize: 20; font.bold: true }
            Note { text: "当前画面或所选照片 · sRGB\n裁切与效果一起写入文件。" }
            Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.line; opacity: 0.5 }
            RowLayout {
                Text { text: "格式"; color: Theme.text; Layout.preferredWidth: 76 }
                ComboBox { Layout.fillWidth: true; model: ["TIFF · 16 位", "PNG · 8 位", "PNG · 16 位", "JPEG · 8 位"]; currentIndex: window.exportFormat; onActivated: window.exportFormat = currentIndex }
            }
            RowLayout {
                visible: window.exportFormat === 3
                Text { text: "JPEG 质量"; color: Theme.text; Layout.preferredWidth: 76 }
                Slider { Layout.fillWidth: true; from: 1; to: 100; stepSize: 1; value: window.exportQuality; onMoved: window.exportQuality = value }
                Text { text: window.exportQuality; color: Theme.muted; Layout.preferredWidth: 28 }
            }
            RowLayout {
                Text { text: "输出尺寸"; color: Theme.text; Layout.preferredWidth: 76 }
                ComboBox {
                    Layout.fillWidth: true; model: ["原始裁切尺寸", "长边 3840 像素", "长边 2560 像素", "长边 1920 像素", "长边 1080 像素"]
                    readonly property var sizes: [0, 3840, 2560, 1920, 1080]
                    currentIndex: Math.max(0, sizes.indexOf(window.exportLongEdge))
                    onActivated: window.exportLongEdge = sizes[currentIndex]
                }
            }
            Note { text: "保持长宽比例，不放大图像。PNG 无损保存，JPEG 为有损压缩。" }
            Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.line; opacity: 0.5 }
            ActionButton { Layout.fillWidth: true; accented: true; text: "导出当前已显示画面…"; enabled: preview.hasFrame && !preview.busy; onClicked: { saveDialog.open(); exportPopup.close() } }
            ActionButton { Layout.fillWidth: true; text: "导出所选 " + preview.selectedCount + " 张到文件夹…"; enabled: preview.selectedCount > 0 && !preview.busy; onClicked: { batchDialog.open(); exportPopup.close() } }
            Note { text: "当前画面导出不覆盖已有文件。批量导出逐张处理，自动跳过同名文件；未访问的照片也按各自设置渲染。" }
            ActionButton { Layout.alignment: Qt.AlignRight; text: "取消"; onClicked: exportPopup.close() }
        }
    }
}
