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
    // The one window's mode (§8): 图库 swaps the left column to the library
    // sidebar and the canvas to the grid; 冲洗/编辑 are the develop page this
    // window has always been (the filmstrip stays their browser; the full
    // shared-browser unification is future work).
    property string appMode: "冲洗"
    readonly property bool libraryMode: appMode === "图库"
    onAppModeChanged: if (window.libraryMode && libraryPage.item !== null) libraryPage.item.takeFocus()
    // The active library item is a half-frame pair (its `pair` map is filled).
    // The pair's own look sections stay live; the shared photo sections that
    // pairWireOf overrides are guarded against it below.
    readonly property bool pairActive: preview.pair.leftUrl !== undefined
    // On-strip = the pair item's own film edge (`pairWireOf` reads
    // overscan_active): the wire then forces the strip's 1 mm gap.
    readonly property bool pairOnStrip: pairActive && preview.overscan.on === true
    readonly property bool pairAnyDated: pairActive && preview.pair.holes !== undefined &&
        (preview.pair.holes[0].dated === true || preview.pair.holes[1].dated === true)
    readonly property real pairSpacingMM: pairActive && preview.pair.spacingMM !== undefined
        ? Number(preview.pair.spacingMM) : 1.0
    // 建对 takes the first two selected photographs in library order (a pair
    // item is never a candidate -- a pair is never a hole of another pair).
    readonly property var pairCandidates: {
        var rows = []
        var items = preview.libraryItems
        for (var i = 0; i < items.length && rows.length < 2; ++i)
            if (items[i].selected && !items[i].pair) rows.push(items[i])
        return rows
    }
    // The 建对 refusal message (createPair returns false without changing
    // anything); cleared automatically so the strip row stays quiet.
    property string pairNotice: ""
    Timer { id: pairNoticeTimer; interval: 8000; onTriggered: window.pairNotice = "" }
    readonly property bool canEditCrop: preview.hasFrame && !preview.busy && !preview.batchExporting && !pairActive
    readonly property bool canBrowse: preview.ready && !preview.busy && !preview.batchExporting && !cropMode
    // Text controls keep their own undo stack while they own keyboard focus.
    readonly property bool editingText: activeFocusItem !== null && typeof activeFocusItem.text === "string" && typeof activeFocusItem.undo === "function"
    onCropModeChanged: updateDialogState()

    function updateDialogState() {
        preview.setDialogOpen(cropMode || openDialog.visible || saveDialog.visible || batchDialog.visible || exportPopup.visible || copySheet.visible)
    }
    function groupName(id) {
        var names = {
            "filmAndPaper": "胶片与相纸", "filmExposure": "胶片曝光", "format": "画幅",
            "decode": "解码", "enlarger": "放大机", "couplers": "偶合剂",
            "preflash": "预闪", "light": "光线", "curve": "曲线", "colour": "颜色"
        }
        return names[id] === undefined ? id : names[id]
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
    // 图库's keys (MAC-LIBRARY-UI §5): zoom steps at ×1.25 across 64..256,
    // copy opens the group sheet, paste writes the copied groups' settings.
    // The page's own keys (0-5, − * +, arrows, Return, Ctrl+A) live on the
    // grid itself, so a typing field never loses them.
    Shortcut {
        sequences: ["Ctrl+="]
        enabled: window.libraryMode && !window.editingText
        onActivated: if (libraryPage.item !== null) libraryPage.item.zoomIn()
    }
    Shortcut {
        sequence: "Ctrl+-"
        enabled: window.libraryMode && !window.editingText
        onActivated: if (libraryPage.item !== null) libraryPage.item.zoomOut()
    }
    Shortcut {
        sequence: "Ctrl+C"
        enabled: window.libraryMode && !window.editingText && library.selectedCount > 0
        onActivated: copySheet.open()
    }
    Shortcut {
        sequence: "Ctrl+V"
        enabled: window.libraryMode && !window.editingText && library.hasClipboard && library.selectedCount > 0
        onActivated: library.pasteSettings()
    }
    // Filter and sort setters store state and only emit queryChanged; the
    // QML layer wires it to refresh (the model's recorded seam).
    Connections {
        target: library
        function onQueryChanged() { library.refresh() }
    }

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
    component MaskSlider: ColumnLayout {
        id: maskSlider
        // RFC-024's contrast mask: reads preview.mask, writes
        // preview.setMask(name, value) -- the engine-parameter path (a moved
        // slider re-renders the print, never re-decodes). The ranges are the
        // engine schema's own rows.
        property string label
        property string name
        property real from: 0
        property real to: 3
        property real step: 0.05
        property int decimals: 2
        property string suffix: ""
        readonly property real current: preview.mask[name] === undefined ? 0 : Number(preview.mask[name])
        readonly property bool available: preview.settingsEditable && !preview.filmIsPositive
        Layout.fillWidth: true; spacing: 2
        RowLayout {
            Layout.fillWidth: true
            Text { text: maskSlider.label; color: maskSlider.available ? Theme.text : Theme.muted; font.pixelSize: 11 }
            Item { Layout.fillWidth: true }
            Text { text: maskSlider.current.toFixed(maskSlider.decimals) + maskSlider.suffix; color: Theme.muted; font.pixelSize: 10 }
        }
        Slider {
            Layout.fillWidth: true; implicitHeight: 24
            from: maskSlider.from; to: maskSlider.to; stepSize: maskSlider.step
            value: maskSlider.current
            enabled: maskSlider.available
            onPressedChanged: {
                if (pressed) preview.beginEditGesture("mask:" + maskSlider.name)
                else preview.endEditGesture()
            }
            onMoved: preview.setMask(maskSlider.name, value)
        }
    }
    component OverscanSlider: ColumnLayout {
        id: overscanSlider
        // The film edge's scalars: reads preview.overscan, writes
        // preview.setOverscan(name, value) -- the engine-parameter path (a
        // moved slider re-renders the print, never re-decodes). The ranges
        // are the engine schema's own rows.
        property string label
        property string name
        property real from: 0
        property real to: 4
        property real step: 0.05
        property int decimals: 2
        property string suffix: ""
        readonly property real current: preview.overscan[name] === undefined ? 0 : Number(preview.overscan[name])
        readonly property bool available: preview.settingsEditable && preview.overscan.on === true
        Layout.fillWidth: true; spacing: 2
        RowLayout {
            Layout.fillWidth: true
            Text { text: overscanSlider.label; color: overscanSlider.available ? Theme.text : Theme.muted; font.pixelSize: 11 }
            Item { Layout.fillWidth: true }
            Text { text: overscanSlider.current.toFixed(overscanSlider.decimals) + overscanSlider.suffix; color: Theme.muted; font.pixelSize: 10 }
        }
        Slider {
            Layout.fillWidth: true; implicitHeight: 24
            from: overscanSlider.from; to: overscanSlider.to; stepSize: overscanSlider.step
            value: overscanSlider.current
            enabled: overscanSlider.available
            onPressedChanged: {
                if (pressed) preview.beginEditGesture("overscan:" + overscanSlider.name)
                else preview.endEditGesture()
            }
            onMoved: preview.setOverscan(overscanSlider.name, value)
        }
    }
    component DateSlider: ColumnLayout {
        id: dateSlider
        // The date back's scalars: reads preview.dateImprint, writes
        // preview.setDateImprint(name, value) -- the same engine-parameter
        // path, active with or without the film edge.
        property string label
        property string name
        property real from: 0
        property real to: 30
        property real step: 0.5
        property int decimals: 1
        property string suffix: ""
        readonly property real current: preview.dateImprint[name] === undefined ? 0 : Number(preview.dateImprint[name])
        // A pair's date rows live off the holes (any dated hole activates the
        // face), so the scalars follow the same rule as the dateSection rows.
        readonly property bool available: preview.settingsEditable &&
            (preview.dateImprint.on === true || (window.pairActive && window.pairAnyDated))
        Layout.fillWidth: true; spacing: 2
        RowLayout {
            Layout.fillWidth: true
            Text { text: dateSlider.label; color: dateSlider.available ? Theme.text : Theme.muted; font.pixelSize: 11 }
            Item { Layout.fillWidth: true }
            Text { text: dateSlider.current.toFixed(dateSlider.decimals) + dateSlider.suffix; color: Theme.muted; font.pixelSize: 10 }
        }
        Slider {
            Layout.fillWidth: true; implicitHeight: 24
            from: dateSlider.from; to: dateSlider.to; stepSize: dateSlider.step
            value: dateSlider.current
            enabled: dateSlider.available
            onPressedChanged: {
                if (pressed) preview.beginEditGesture("dateImprint:" + dateSlider.name)
                else preview.endEditGesture()
            }
            onMoved: preview.setDateImprint(dateSlider.name, value)
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
    // One hole of the active half-frame pair (PAIR-WIRING §5): reads
    // preview.pair.holes[side], writes preview.setPairHole(side, field,
    // value). Exposure and placement re-compose the piece; the date rows are
    // wire rows and reprint the kept negative. The print trim rows are
    // persisted by the controller but not yet wired to pixels (§12's
    // per-hole print-layer cut is a later port), so they read back
    // read-only with that note.
    component PairHoleSection: ColumnLayout {
        id: holePanel
        property int side: 0
        readonly property var hole: window.pairActive && preview.pair.holes !== undefined ? preview.pair.holes[side] : null
        readonly property bool ok: hole !== null
        readonly property string sideName: window.pairActive && preview.pair.turned === true
            ? (side === 0 ? "上" : "下") : (side === 0 ? "左" : "右")
        Layout.fillWidth: true; spacing: 6
        Text { text: holePanel.sideName + "孔" + (holePanel.ok && holePanel.hole.sourceName !== undefined && holePanel.hole.sourceName.length > 0 ? " · " + holePanel.hole.sourceName : ""); color: Theme.text; font.pixelSize: 11; font.bold: true }
        RowLayout {
            Layout.fillWidth: true
            Text { text: "曝光"; color: Theme.text; font.pixelSize: 11 }
            Item { Layout.fillWidth: true }
            Text {
                text: holePanel.ok ? (holePanel.hole.exposureEv >= 0 ? "+" : "") + Number(holePanel.hole.exposureEv).toFixed(2) + " EV" : ""
                color: Theme.muted; font.pixelSize: 10
            }
        }
        Slider {
            Layout.fillWidth: true; implicitHeight: 24
            from: -5; to: 5; stepSize: 0.05
            value: holePanel.ok ? Number(holePanel.hole.exposureEv) : 0
            enabled: preview.settingsEditable
            onPressedChanged: {
                if (pressed) preview.beginEditGesture("pairHole" + holePanel.side + "ExposureEv")
                else preview.endEditGesture()
            }
            onMoved: preview.setPairHole(holePanel.side, "exposureEv", value)
        }
        RowLayout {
            Layout.fillWidth: true
            Text { text: "测光读数"; color: Theme.text; font.pixelSize: 11 }
            Item { Layout.fillWidth: true }
            Text {
                // The reading arrives with the compose reply (the meter cache
                // holds while the hole is dragged, PAIR-WIRING §4.2); while a
                // job runs with no reading yet it reads as metering.
                text: !holePanel.ok ? ""
                    : holePanel.hole.meteredEv === undefined
                        ? (preview.busy ? "测光中…" : "尚无读数")
                        : Number(holePanel.hole.meteredEv).toFixed(2) + " EV"
                color: Theme.muted; font.pixelSize: 10
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Switch {
                text: "日期"; font.pixelSize: 11
                palette.windowText: Theme.text
                checked: holePanel.ok && holePanel.hole.dated === true
                enabled: preview.settingsEditable
                onToggled: preview.setPairHole(holePanel.side, "dated", checked)
            }
            TextField {
                objectName: "pairHole" + holePanel.side + "DateText"
                Layout.fillWidth: true
                placeholderText: "如 '26 11 15"
                text: holePanel.ok && holePanel.hole.dateText !== undefined ? holePanel.hole.dateText : ""
                enabled: preview.settingsEditable && holePanel.ok && holePanel.hole.dated === true
                font.pixelSize: 11
                onEditingFinished: {
                    var current = holePanel.hole.dateText === undefined ? "" : holePanel.hole.dateText
                    if (text !== current) preview.setPairHole(holePanel.side, "dateText", text)
                }
            }
        }
        // Print trim (PAIR-WIRING §12): the four enlarger rows stored and
        // persisted with the pair, shown read-only until the print-layer cut
        // lands -- a live edit here would silently change nothing.
        ColumnLayout {
            Layout.fillWidth: true; spacing: 2
            Text { text: "印相 trim（暂存）"; color: Theme.muted; font.pixelSize: 10; font.bold: true }
            Text { text: "亮度 " + (holePanel.ok ? (holePanel.hole.trim.printExposure >= 0 ? "+" : "") + Number(holePanel.hole.trim.printExposure).toFixed(2) : ""); color: Theme.muted; font.pixelSize: 10 }
            Text { text: "黄滤镜 " + (holePanel.ok ? (holePanel.hole.trim.yFilterShift >= 0 ? "+" : "") + Number(holePanel.hole.trim.yFilterShift).toFixed(2) : ""); color: Theme.muted; font.pixelSize: 10 }
            Text { text: "洋红滤镜 " + (holePanel.ok ? (holePanel.hole.trim.mFilterShift >= 0 ? "+" : "") + Number(holePanel.hole.trim.mFilterShift).toFixed(2) : ""); color: Theme.muted; font.pixelSize: 10 }
            Text { text: "预闪 " + (holePanel.ok ? Number(holePanel.hole.trim.preflashExposure).toFixed(2) : ""); color: Theme.muted; font.pixelSize: 10 }
        }
        // Scene Placement per hole is deferred: requestLatitude() carries no
        // region parameter yet and refuses on a pair (PAIR-WIRING §6.2/§6.3
        // want the per-hole region request), so there is deliberately no
        // 应用映射/Fit row here to build a dead path against.
        Note { text: "印相 trim 四行随对记录保存；逐孔印相切割在后续任务接线，当前不改变像素。" }
    }

    RowLayout {
        anchors.fill: parent; spacing: 1
        Rectangle {
            Layout.preferredWidth: Theme.leftWidth; Layout.fillHeight: true; color: Theme.card
            ColumnLayout {
                anchors.fill: parent; spacing: 0
                // The mode row (§8): 图库 · 冲洗 · 编辑 leads the left column.
                // 冲洗/编辑 show this page's rail; 图库 swaps the column to
                // the library sidebar below.
                Rectangle {
                    objectName: "modeSwitch"
                    Layout.fillWidth: true; Layout.preferredHeight: Theme.topHeight; color: Theme.card
                    RowLayout {
                        anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 8; spacing: 2
                        component ModeButton: ActionButton {
                            required property string label
                            required property string mode
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            implicitHeight: 25
                            font.pixelSize: 12
                            text: label
                            accented: window.appMode === mode
                            onClicked: window.appMode = mode
                        }
                        ModeButton { objectName: "modeLibraryButton"; label: "图库"; mode: "图库" }
                        ModeButton { objectName: "modeDevelopButton"; label: "冲洗"; mode: "冲洗" }
                        ModeButton { objectName: "modeEditButton"; label: "编辑"; mode: "编辑" }
                    }
                }
                // The develop rail: 冲洗/编辑's column content, unchanged.
                ScrollView {
                    visible: !window.libraryMode
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
                            Note {
                                visible: window.pairActive
                                text: "半幅对 · 画面级裁切与方向不适用于对件：两孔的画面在孔内逐孔放置（见「半幅对」面板）。"
                                color: Theme.accent
                            }
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
                            // 遮罩 (RFC-024, phase 2): the contrast mask is an
                            // engine parameter on Windows now. The switch turns
                            // it on and the four sliders set the curve's
                            // amounts, core and scale; the scheme stays the
                            // product default (gaussian) and is not exposed.
                            // Slide film never reaches the print, so the group
                            // is dead there like the DI's rows.
                            RowLayout {
                                Layout.fillWidth: true
                                Switch {
                                    objectName: "contrastMaskSwitch"
                                    text: "遮罩"; font.pixelSize: 11
                                    palette.windowText: Theme.text
                                    checked: preview.mask.active
                                    enabled: preview.settingsEditable && !preview.filmIsPositive
                                    onToggled: preview.setMask("active", checked)
                                }
                                Item { Layout.fillWidth: true }
                                Text {
                                    text: !preview.mask.active ? "关闭" : "已应用"
                                    color: Theme.muted; font.pixelSize: 10
                                }
                            }
                            MaskSlider { label: "高光提升"; name: "highlights"; from: 0; to: 3; step: 0.05; decimals: 2; suffix: " stops" }
                            MaskSlider { label: "阴影压低"; name: "shadows"; from: 0; to: 3; step: 0.05; decimals: 2; suffix: " stops" }
                            MaskSlider { label: "核心宽度"; name: "core"; from: 0.1; to: 3; step: 0.05; decimals: 2; suffix: " stops" }
                            MaskSlider { label: "作用范围"; name: "scale"; from: 0.002; to: 0.12; step: 0.002; decimals: 3 }
                            ActionButton {
                                objectName: "resetMaskButton"
                                Layout.fillWidth: true
                                text: "重置遮罩"
                                enabled: preview.settingsEditable && !preview.filmIsPositive && preview.hasFrame
                                onClicked: {
                                    preview.setMask("active", false)
                                    preview.setMask("highlights", 0)
                                    preview.setMask("shadows", 0)
                                    preview.setMask("core", 1.0)
                                    preview.setMask("scale", 0.03)
                                }
                            }
                            Note { text: "对比遮罩按画面局部提升相纸高光、压低阴影：开关与滑块走引擎参数，重新渲染而不重解码。" }
                        }
                    }
                }
                // The library sidebar (图库's column): the three tabs over
                // the library model — source picker, collections tree,
                // facet checks, info — instantiated only while in 图库.
                Loader {
                    id: libraryPage
                    active: window.libraryMode
                    visible: window.libraryMode
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    sourceComponent: LibrarySidebar {}
                }
            }
        }
        Rectangle {
            Layout.fillWidth: true; Layout.fillHeight: true; color: Theme.ground
            ColumnLayout {
                // 冲洗/编辑's page (the develop surface this window has
                // always been); 图库 replaces the column with the grid page
                // beside it.
                visible: !window.libraryMode
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
                            Text {
                                Layout.fillWidth: true
                                visible: window.pairNotice.length > 0
                                text: window.pairNotice
                                color: "#ffb39f"; font.pixelSize: 10
                                elide: Text.ElideRight
                            }
                            Item { Layout.fillWidth: true; visible: window.pairNotice.length === 0 }
                            ActionButton { text: "全选"; implicitHeight: 23; font.pixelSize: 10; enabled: window.canBrowse && preview.libraryItems.length > 0; onClicked: preview.selectAllPhotos(true) }
                            ActionButton { text: "清除选择"; implicitHeight: 23; font.pixelSize: 10; enabled: window.canBrowse && preview.selectedCount > 0; onClicked: preview.selectAllPhotos(false) }
                            ActionButton { text: "同步设置"; implicitHeight: 23; font.pixelSize: 10; enabled: window.canBrowse && preview.hasFrame && preview.selectedCount > 0; onClicked: preview.syncSelectedSettings(false); ToolTip.visible: hovered; ToolTip.text: "将当前照片的胶片、相纸、解码和效果同步到所选照片，不同步裁切。" }
                            // 建对 (PAIR-WIRING §5): the two selected photographs
                            // become one pair library item. The controller
                            // refuses (and changes nothing) while the engine is
                            // busy, when a source is itself a pair, or when an
                            // identical pair already sits in the library (that
                            // last one selects the existing pair and returns
                            // true), so the button only surfaces the refusal.
                            ActionButton {
                                objectName: "createPairButton"
                                text: "建对"; implicitHeight: 23; font.pixelSize: 10
                                enabled: window.canBrowse && window.pairCandidates.length === 2
                                onClicked: {
                                    if (preview.createPair(window.pairCandidates[0].url, window.pairCandidates[1].url)) {
                                        window.pairNotice = ""
                                    } else {
                                        window.pairNotice = "无法建对：请等待当前操作完成，或改选两张不同的照片（半幅对不能作为对孔）"
                                        pairNoticeTimer.restart()
                                    }
                                }
                                ToolTip.visible: hovered
                                ToolTip.text: "把所选的前两张照片（按库序）合为一件半幅对；对以独立库项出现，逐孔编辑见右侧「半幅对」面板。"
                            }
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
            // The grid page (图库): the lazy GridView over the library model
            // with its own header and info strip. The filmstrip block above
            // stays 冲洗/编辑's browser — the full shared-browser
            // unification is future work.
            LibraryGridPage {
                objectName: "libraryPageItem"
                anchors.fill: parent
                visible: window.libraryMode
                onOpenRequested: function(path) {
                    library.openInDevelop(path)
                    window.appMode = "冲洗"
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
                        // 半幅对 (PAIR-WIRING §5/§6): the pair library item's
                        // own panel. The piece's spacing/turn and each hole's
                        // exposure/date edit through setPairPiece/setPairHole;
                        // the pair's look (film/paper, film edge, print rows)
                        // stays in the shared sections below. The canvas needs
                        // no change -- the pair item's previewImage IS the
                        // piece, and the filmstrip already carries the item.
                        Section {
                            id: pairSection
                            title: "半幅对"
                            visible: window.pairActive
                            Note {
                                visible: window.pairActive
                                text: "对不拥有自己的照片：两孔随各自源照片的解码与裁切；件的胶片、相纸与胶片边缘在下方共用面板调整。"
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: "记录"; color: Theme.text; font.pixelSize: 11 }
                                Item { Layout.fillWidth: true }
                                Text {
                                    text: window.pairActive && preview.pair.saved === true ? "已保存" : "保存中…"
                                    color: Theme.muted; font.pixelSize: 10
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: "间距"; color: window.pairOnStrip ? Theme.muted : Theme.text; font.pixelSize: 11 }
                                Item { Layout.fillWidth: true }
                                Text {
                                    text: window.pairActive
                                        ? (window.pairOnStrip ? "1.0 mm（片条固定）" : Number(preview.pair.spacingMM).toFixed(2) + " mm")
                                        : ""
                                    color: Theme.muted; font.pixelSize: 10
                                }
                            }
                            Slider {
                                objectName: "pairSpacingSlider"
                                Layout.fillWidth: true; implicitHeight: 24
                                from: 0.5; to: 2.0; stepSize: 0.05
                                value: window.pairOnStrip ? 1.0 : (window.pairActive ? Number(preview.pair.spacingMM) : 1.0)
                                enabled: preview.settingsEditable && !window.pairOnStrip
                                onPressedChanged: {
                                    if (pressed) preview.beginEditGesture("pairSpacing")
                                    else preview.endEditGesture()
                                }
                                onMoved: preview.setPairPiece("spacingMM", value)
                            }
                            Note { visible: window.pairOnStrip; text: "胶片边缘开启：对走在片条上，间隙固定为片条的 1 mm；关闭胶片边缘后可调。" }
                            // Wide-gap dated bare pair (PAIR-WIRING §2's gate
                            // shape): with a date on and no film edge, the
                            // engine's 37:24 shape gate refuses spacing above
                            // ~2.85 mm, so a record-loaded wide gap warns here
                            // (the slider itself caps at 2.0).
                            Note {
                                visible: window.pairActive && !window.pairOnStrip && window.pairAnyDated &&
                                         window.pairSpacingMM > 2.85
                                text: "有日期且无胶片边缘时间距上限约 2.85 mm：更宽的间隙超出引擎的画幅比例约束，组装会被拒绝。"
                                color: Theme.accent
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Switch {
                                    objectName: "pairTurnedSwitch"
                                    text: "转向（竖排）"; font.pixelSize: 11
                                    palette.windowText: Theme.text
                                    checked: window.pairActive && preview.pair.turned === true
                                    enabled: preview.settingsEditable
                                    onToggled: preview.setPairPiece("turned", checked)
                                }
                                Item { Layout.fillWidth: true }
                                Text {
                                    text: window.pairActive && preview.pair.turned === true ? "上下竖排" : "左右并排"
                                    color: Theme.muted; font.pixelSize: 10
                                }
                            }
                            Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.line; opacity: 0.35 }
                            PairHoleSection { side: 0 }
                            Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.line; opacity: 0.35 }
                            PairHoleSection { side: 1 }
                            Note { text: "曝光与放置改动重新组装整件；日期改动按同底片重印。撤销/重做按对件自己的历史行走。" }
                        }
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
                        // Scene latitude (RFC-023): an on-demand measurement
                        // of the open session -- the medium's characteristic
                        // ramp, the scene's stop histogram behind it, and the
                        // three zones the medium's ISO boundaries cut the
                        // scene into. Since the phase-2 port the card also
                        // applies: the mapping is an engine parameter, so the
                        // switch places the scene with the committed curve and
                        // 提交拟合 writes the solved fit. Every reply key
                        // beyond medium/scene is read defensively: the canvas
                        // draws whichever layers arrived.
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
                                    // A pair's reading is the piece's per-hole
                                    // region (PAIR-WIRING §6.2); until that
                                    // request exists the controller refuses the
                                    // analysis on a pair, so the button stands
                                    // down rather than no-op silently.
                                    enabled: preview.hasFrame && !preview.busy && !window.pairActive
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
                            // 应用映射 / 提交拟合 (phase 2): the mapping is an
                            // engine parameter on Windows now. The switch
                            // places the scene with the committed curve; the
                            // button writes the analysis's solved fit through
                            // the ordinary engine path. The reply is cleared
                            // whenever a new print publishes, so the button
                            // only offers a fit for the session it measured.
                            readonly property var fit: reply && reply.fit !== undefined ? reply.fit : null
                            readonly property bool fitValid: fit !== null && fit.valid !== undefined && fit.valid
                            RowLayout {
                                Layout.fillWidth: true
                                Switch {
                                    objectName: "sceneLatitudeSwitch"
                                    text: "应用映射"; font.pixelSize: 11
                                    palette.windowText: Theme.text
                                    checked: preview.sceneLatitudeActive
                                    enabled: preview.settingsEditable
                                    onToggled: preview.setSceneLatitude(checked)
                                }
                                Item { Layout.fillWidth: true }
                                Text {
                                    text: !preview.sceneLatitudeActive ? "关闭" : "已应用"
                                    color: Theme.muted; font.pixelSize: 10
                                }
                            }
                            ActionButton {
                                objectName: "commitFitButton"
                                Layout.fillWidth: true
                                text: "提交拟合"
                                // The fit lands on the first curve; a pair's
                                // commit waits for the per-hole region (§6.3),
                                // and the controller refuses it on a pair.
                                enabled: latitudeSection.fitValid && preview.settingsEditable && preview.hasFrame && !window.pairActive
                                onClicked: preview.commitLatitudeFit()
                            }
                            Note { visible: latitudeSection.summary.length > 0; text: latitudeSection.summary; color: Theme.text }
                            Note {
                                visible: window.pairActive
                                text: "半幅对 · 分析与提交拟合暂不适用于对件（逐孔区域测量待后续任务）；「应用映射」仍按整件生效。"
                                color: Theme.accent
                            }
                            Note { text: "分析只读取当前显影结果，不改变画面；「应用映射」按已提交的曲线放置场景，「提交拟合」写入求解的高光/阴影收放。" }
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
                            Note {
                                visible: window.pairActive
                                text: "半幅对 · 胶片曝光、白平衡与测光在件级控制：对的曝光恒为 0（每孔的曝光在「半幅对」面板），解码随各孔源照片。"
                                color: Theme.accent
                            }
                            Adjustment { label: "胶片曝光"; setting: "filmExposureEv"; from: -4; to: 4; step: 0.1; decimals: 1; suffix: " EV"; available: !window.pairActive }
                            Note { text: "在胶片曝光前调整场景亮度。每张照片的效果与裁切自动保存，下次导入时恢复。" }
                            // 白平衡（RAW 解码侧的 Temperature/Tint，Adobe DNG 口径）：
                            // 0 = 自动（As Shot）。改动是解码参数，会重新解码当前照片
                            // 而不是增量重渲染；高光扩展解码不接受覆盖，控件随之禁用。
                            // 对件没有自己的解码（两孔各随源照片），行随对禁用。
                            readonly property bool wbEditable: preview.hasFrame && preview.decodeMode === 0 && !window.pairActive
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
                                    // pairWireOf forces the meter off on a pair
                                    // (each hole's exposure rides its own
                                    // pixels), so the shared switch stands down.
                                    checked: preview.autoExposure
                                    enabled: preview.settingsEditable && !window.pairActive
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
                                    // pairWireOf derives a pair's format from
                                    // the piece itself (36 mm + the gap), so
                                    // the shared row stands down rather than
                                    // edit a value the wire overrides.
                                    enabled: preview.settingsEditable && !window.pairActive
                                    onActivated: preview.setAdjustment("filmFormatMm", lengths[currentIndex])
                                }
                            }
                            Note { visible: window.pairActive; text: "半幅对 · 画幅长边由整件决定（36 mm + 间距），不单独设置。"; color: Theme.accent }
                            Note { text: "物理画幅决定颗粒与光晕尺度；裁切不改变画幅。" }
                            Effect { label: "颗粒"; activeKey: "grainActive"; amountKey: "grainAmount" }
                            Effect { label: "光晕"; activeKey: "halationActive"; amountKey: "halationAmount"; maximum: 4 }
                            Effect { label: "眩光"; activeKey: "glareActive"; amountKey: "glareAmount"; maximum: 30 }
                        }
                        // 数字中间片 (RFC-028): replaces the paper with a Cineon
                        // log rendering of the film's own negative. Engine-side
                        // settings like the meter — setDi goes through changed()
                        // and re-renders the session; negative film only.
                        Section {
                            id: diSection
                            title: "数字中间片"
                            readonly property bool diActive: preview.di.active === undefined ? false : preview.di.active
                            readonly property bool diAvailable: preview.settingsEditable && !preview.filmIsPositive
                            RowLayout {
                                Layout.fillWidth: true
                                Switch {
                                    objectName: "diActiveSwitch"
                                    text: "数字中间片"; font.pixelSize: 11
                                    palette.windowText: Theme.text
                                    checked: diSection.diActive
                                    enabled: diSection.diAvailable
                                    onToggled: preview.setDi("active", checked)
                                }
                                Item { Layout.fillWidth: true }
                                Text { text: diSection.diActive ? "Cineon 对数" : "相纸冲印"; color: Theme.muted; font.pixelSize: 10 }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Switch {
                                    objectName: "diBlueCompensationSwitch"
                                    text: "蓝补偿"; font.pixelSize: 11
                                    palette.windowText: Theme.text
                                    checked: preview.di.blueCompensation === undefined ? false : preview.di.blueCompensation
                                    enabled: diSection.diAvailable && diSection.diActive
                                    onToggled: preview.setDi("blueCompensation", checked)
                                }
                                Item { Layout.fillWidth: true }
                                Text { text: "校正蓝区的偏色"; color: Theme.muted; font.pixelSize: 10 }
                            }
                            Note { visible: preview.filmIsPositive; text: "反转片直接扫描，没有负片，不适用数字中间片。"; color: Theme.accent }
                            Note { text: "数字中间片以 Cineon 对数编码输出负片的正像，替代相纸冲印；蓝补偿按胶片拟合的蓝区窗口旋转色相。" }
                        }
                        Section {
                            id: rawSection
                            title: "RAW 解码"
                            // A pair opens no RAW of its own: each hole reads
                            // its source photo's own decode, so the shared
                            // choice stands down on a pair item.
                            ComboBox { id: decode; Layout.fillWidth: true; model: ["兼容模式（默认）", "高光扩展（实验）"]; currentIndex: preview.decodeMode; enabled: preview.settingsEditable && !window.pairActive; onActivated: preview.decodeMode = currentIndex }
                            Note { visible: window.pairActive; text: "半幅对 · 解码随各孔源照片，不在件上设置。"; color: Theme.accent }
                            Note { text: preview.decodeMode ? "保留去马赛克后的部分高光及负值；仍使用整数去马赛克。" : "与已验证的原生 RAW 流程保持一致。" }
                            // HDR 扫描 (2026-11-06, phase 2 port): the slide's
                            // HDR scan is an engine parameter on Windows now —
                            // the switch turns it on and the slider sets the
                            // clear base's headroom (the engine schema's own
                            // [0, 16] row; anything up to 1 is today's scan,
                            // byte for byte). Only a positive film scanned has
                            // it: slide film is scanned directly, so the group
                            // is dead for negatives like the DI's rows.
                            readonly property bool hdrAvailable: preview.settingsEditable && preview.filmIsPositive
                            readonly property bool hdrOn: preview.scanHdr.on === undefined ? false : preview.scanHdr.on
                            RowLayout {
                                Layout.fillWidth: true
                                Switch {
                                    objectName: "scanHdrSwitch"
                                    text: "HDR 扫描"; font.pixelSize: 11
                                    palette.windowText: Theme.text
                                    checked: rawSection.hdrOn
                                    enabled: rawSection.hdrAvailable
                                    onToggled: preview.setScanHdr("on", checked)
                                }
                                Item { Layout.fillWidth: true }
                                Text {
                                    text: !rawSection.hdrOn ? "关闭"
                                        : Number(preview.scanHdr.headroom).toFixed(1) + "×"
                                    color: Theme.muted; font.pixelSize: 10
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: "余量"; color: rawSection.hdrOn && rawSection.hdrAvailable ? Theme.text : Theme.muted; font.pixelSize: 11 }
                                Item { Layout.fillWidth: true }
                                Text { text: Number(preview.scanHdr.headroom === undefined ? 0 : preview.scanHdr.headroom).toFixed(1) + "×"; color: Theme.muted; font.pixelSize: 10 }
                            }
                            Slider {
                                objectName: "scanHdrSlider"; Layout.fillWidth: true; implicitHeight: 24
                                from: 1; to: 16; stepSize: 0.1
                                value: Number(preview.scanHdr.headroom === undefined ? 0 : preview.scanHdr.headroom)
                                enabled: rawSection.hdrAvailable && rawSection.hdrOn
                                onPressedChanged: {
                                    if (pressed) preview.beginEditGesture("scanHdr:headroom")
                                    else preview.endEditGesture()
                                }
                                onMoved: preview.setScanHdr("headroom", value)
                            }
                            Note { visible: preview.hasFrame && !preview.filmIsPositive; text: "HDR 扫描仅适用于直接扫描的反转片；负片走相纸冲印，不适用。"; color: Theme.accent }
                            Note { text: "HDR 扫描把反转片每层的肩部移过各自的膝点、在光谱扫描内提升，使片基落在今日扫描的余量倍数上；走引擎参数，重新渲染而不重解码。" }
                        }
                        // 胶片边缘 (RFC-032, phase 2 port): the film outside
                        // the frame as real exposure — the gate, the rebate,
                        // the perforations, the maker's edge print. Engine-side
                        // settings like the HDR scan: setOverscan goes through
                        // changed() and re-renders the session (开 is a
                        // shoot-layer field, so its edits re-develop the film
                        // and the picture's size becomes the film canvas's).
                        Section {
                            id: overscanSection
                            title: "胶片边缘"
                            readonly property bool osOn: preview.overscan.on === undefined ? false : preview.overscan.on
                            readonly property bool osEditable: preview.settingsEditable
                            readonly property var formats: [
                                {"id": "135", "label": "135 全幅（36×24）"},
                                {"id": "135_half", "label": "135 半幅（18×24）"},
                                {"id": "120_645", "label": "120 6×4.5"},
                                {"id": "120_6x6", "label": "120 6×6"},
                                {"id": "120_6x7", "label": "120 6×7"},
                                {"id": "120_6x8", "label": "120 6×8"},
                                {"id": "120_6x9", "label": "120 6×9"},
                                {"id": "135_xpan", "label": "135 宽幅 XPan（65×24）"},
                                {"id": "120_6x12", "label": "120 6×12"},
                                {"id": "120_6x17", "label": "120 6×17"}
                            ]
                            readonly property var gates: ["auto", "square", "rounded", "eared", "shouldered", "kicked"]
                            readonly property var modes: ["strip", "filed"]
                            RowLayout {
                                Layout.fillWidth: true
                                Switch {
                                    objectName: "overscanSwitch"
                                    text: "胶片边缘"; font.pixelSize: 11
                                    palette.windowText: Theme.text
                                    checked: overscanSection.osOn
                                    enabled: overscanSection.osEditable
                                    onToggled: preview.setOverscan("on", checked)
                                }
                                Item { Layout.fillWidth: true }
                                Text {
                                    text: !overscanSection.osOn ? "关闭" : (preview.overscan.format === undefined ? "" : preview.overscan.format)
                                    color: Theme.muted; font.pixelSize: 10
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: "模式"; color: overscanSection.osEditable && overscanSection.osOn ? Theme.text : Theme.muted; font.pixelSize: 11 }
                                ComboBox {
                                    objectName: "overscanModeCombo"
                                    Layout.fillWidth: true
                                    model: ["条片（整个片宽）", "单帧（少量片边）"]
                                    readonly property var values: overscanSection.modes
                                    enabled: overscanSection.osEditable && overscanSection.osOn
                                    currentIndex: values.indexOf(preview.overscan.mode === undefined ? "strip" : preview.overscan.mode)
                                    onActivated: preview.setOverscan("mode", values[currentIndex])
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: "画幅"; color: overscanSection.osEditable && overscanSection.osOn ? Theme.text : Theme.muted; font.pixelSize: 11 }
                                ComboBox {
                                    objectName: "overscanFormatCombo"
                                    Layout.fillWidth: true
                                    model: overscanSection.formats.map(function(f) { return f.label })
                                    // pairWireOf forces 135_half on a pair (the
                                    // strip's own format); the row stands down.
                                    enabled: overscanSection.osEditable && overscanSection.osOn && !window.pairActive
                                    currentIndex: {
                                        var f = preview.overscan.format === undefined ? "135" : preview.overscan.format
                                        for (var i = 0; i < overscanSection.formats.length; ++i)
                                            if (overscanSection.formats[i].id === f) return i
                                        return 0
                                    }
                                    onActivated: preview.setOverscan("format", overscanSection.formats[currentIndex].id)
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: "门栅"; color: overscanSection.osEditable && overscanSection.osOn ? Theme.text : Theme.muted; font.pixelSize: 11 }
                                ComboBox {
                                    objectName: "overscanGateCombo"
                                    Layout.fillWidth: true
                                    model: ["自动（真实机型）", "方形", "圆角", "带耳", "带肩", "外凸角"]
                                    readonly property var values: overscanSection.gates
                                    enabled: overscanSection.osEditable && overscanSection.osOn
                                    currentIndex: values.indexOf(preview.overscan.gate === undefined ? "auto" : preview.overscan.gate)
                                    onActivated: preview.setOverscan("gate", values[currentIndex])
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: "片孔"; color: overscanSection.osEditable && overscanSection.osOn ? Theme.text : Theme.muted; font.pixelSize: 11 }
                                ComboBox {
                                    objectName: "overscanHolesCombo"
                                    Layout.fillWidth: true
                                    model: ["白孔（透过扫描光）", "黑孔（黑色背衬）"]
                                    enabled: overscanSection.osEditable && overscanSection.osOn
                                    currentIndex: preview.overscan.holes === "black" ? 1 : 0
                                    onActivated: preview.setOverscan("holes", currentIndex === 1 ? "black" : "white")
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: "载体"; color: overscanSection.osEditable && overscanSection.osOn ? Theme.text : Theme.muted; font.pixelSize: 11 }
                                ComboBox {
                                    objectName: "overscanCarrierCombo"
                                    Layout.fillWidth: true
                                    model: ["无（画布止于片边）", "黑色载体", "开放（扫描亮）"]
                                    enabled: overscanSection.osEditable && overscanSection.osOn
                                    currentIndex: preview.overscan.carrier === "black" ? 1 : preview.overscan.carrier === "open" ? 2 : 0
                                    onActivated: preview.setOverscan("carrier", ["none", "black", "open"][currentIndex])
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: "印字"; color: overscanSection.osEditable && overscanSection.osOn ? Theme.text : Theme.muted; font.pixelSize: 11 }
                                TextField {
                                    objectName: "overscanEdgeTextInput"
                                    Layout.fillWidth: true
                                    placeholderText: "边缘印字（如 KODAK PORTRA 400）"
                                    text: preview.overscan.edgeText === undefined ? "" : preview.overscan.edgeText
                                    enabled: overscanSection.osEditable && overscanSection.osOn
                                    font.pixelSize: 11
                                    onEditingFinished: if (text !== (preview.overscan.edgeText === undefined ? "" : preview.overscan.edgeText)) preview.setOverscan("edgeText", text)
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: "帧号"; color: overscanSection.osEditable && overscanSection.osOn ? Theme.text : Theme.muted; font.pixelSize: 11 }
                                Item { Layout.fillWidth: true }
                                Text {
                                    text: Number(preview.overscan.frameNumber === undefined ? 0 : preview.overscan.frameNumber) === 0
                                          ? "种子抽定" : String(preview.overscan.frameNumber)
                                    color: Theme.muted; font.pixelSize: 10
                                }
                            }
                            Slider {
                                objectName: "overscanFrameNumberSlider"; Layout.fillWidth: true; implicitHeight: 24
                                from: 0; to: 99; stepSize: 1
                                value: Number(preview.overscan.frameNumber === undefined ? 0 : preview.overscan.frameNumber)
                                enabled: overscanSection.osEditable && overscanSection.osOn
                                onPressedChanged: {
                                    if (pressed) preview.beginEditGesture("overscan:frameNumber")
                                    else preview.endEditGesture()
                                }
                                onMoved: preview.setOverscan("frameNumber", value)
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: "相机种子"; color: overscanSection.osEditable && overscanSection.osOn ? Theme.text : Theme.muted; font.pixelSize: 11 }
                                TextField {
                                    objectName: "overscanCameraSeedInput"
                                    Layout.preferredWidth: 96
                                    validator: IntValidator { bottom: 0; top: 2147483647 }
                                    text: String(preview.overscan.cameraSeed === undefined ? 1 : preview.overscan.cameraSeed)
                                    enabled: overscanSection.osEditable && overscanSection.osOn
                                    font.pixelSize: 11
                                    onEditingFinished: {
                                        var v = parseInt(text, 10)
                                        if (!isNaN(v) && v !== preview.overscan.cameraSeed) preview.setOverscan("cameraSeed", v)
                                    }
                                }
                                Text { text: "帧种子"; color: overscanSection.osEditable && overscanSection.osOn ? Theme.text : Theme.muted; font.pixelSize: 11 }
                                TextField {
                                    objectName: "overscanFrameSeedInput"
                                    Layout.preferredWidth: 96
                                    validator: IntValidator { bottom: 0; top: 2147483647 }
                                    text: String(preview.overscan.frameSeed === undefined ? 1 : preview.overscan.frameSeed)
                                    enabled: overscanSection.osEditable && overscanSection.osOn
                                    font.pixelSize: 11
                                    onEditingFinished: {
                                        var v = parseInt(text, 10)
                                        if (!isNaN(v) && v !== preview.overscan.frameSeed) preview.setOverscan("frameSeed", v)
                                    }
                                }
                            }
                            OverscanSlider { label: "边缘雾"; name: "fog"; from: 0; to: 4; step: 0.05 }
                            OverscanSlider { label: "漏光"; name: "leaks"; from: 0; to: 4; step: 0.05 }
                            Note {
                                visible: window.pairActive
                                text: "半幅对 · 开关即对件上/下片条；画幅固定为 135 半幅条（间隙随「半幅对」面板的间距），门栅、片孔、印字与雾光仍作用于整件。"
                                color: Theme.accent
                            }
                            Note { text: "胶片边缘把画面外的胶片画出为真实曝光：门栅阴影、片基、片孔、片边印字、边缘雾与漏光一起显影；开关与选择走引擎参数，重新渲染而不重解码。画幅需与照片比例一致（先按门栅裁切）。" }
                        }
                        // 日期背印 (RFC-031, phase 2 port): the date exposed
                        // onto the negative from behind — with or without the
                        // film edge (画面内 placement works on the bare
                        // frame). lcd/dots are the engine's own faces; the
                        // data face prints the shooting data beside the date.
                        Section {
                            id: dateSection
                            title: "日期背印"
                            readonly property bool dtOn: preview.dateImprint.on === undefined ? false : preview.dateImprint.on
                            readonly property bool dtEditable: preview.settingsEditable
                            // On a pair the switch and the per-frame date text
                            // are the holes' rows (pairWireOf recomputes
                            // date_imprint_active/text/text_b from the per-hole
                            // switches), so those stand down; the face,
                            // placement and size rows still ride the pair's
                            // look and live whenever any hole is dated.
                            readonly property bool dtItemEditable: preview.settingsEditable && !window.pairActive
                            readonly property bool dtLive: dtOn || (window.pairActive && window.pairAnyDated)
                            Note {
                                visible: window.pairActive
                                text: "半幅对 · 日期开关与文本由逐孔设置（见「半幅对」面板）；字面、位置与大小仍作用于整件。"
                                color: Theme.accent
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Switch {
                                    objectName: "dateImprintSwitch"
                                    text: "日期背印"; font.pixelSize: 11
                                    palette.windowText: Theme.text
                                    checked: dateSection.dtOn
                                    enabled: dateSection.dtItemEditable
                                    onToggled: preview.setDateImprint("on", checked)
                                }
                                Item { Layout.fillWidth: true }
                                Text {
                                    text: !dateSection.dtLive ? "关闭" : (preview.dateImprint.style === undefined ? "" : preview.dateImprint.style)
                                    color: Theme.muted; font.pixelSize: 10
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: "日期"; color: dateSection.dtItemEditable && dateSection.dtLive ? Theme.text : Theme.muted; font.pixelSize: 11 }
                                TextField {
                                    objectName: "dateImprintTextInput"
                                    Layout.fillWidth: true
                                    placeholderText: "如 '26 11 15"
                                    text: preview.dateImprint.text === undefined ? "" : preview.dateImprint.text
                                    enabled: dateSection.dtItemEditable && dateSection.dtLive
                                    font.pixelSize: 11
                                    onEditingFinished: if (text !== (preview.dateImprint.text === undefined ? "" : preview.dateImprint.text)) preview.setDateImprint("text", text)
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: "数据"; color: dateSection.dtEditable && dateSection.dtLive ? Theme.text : Theme.muted; font.pixelSize: 11 }
                                TextField {
                                    objectName: "dateImprintDataTextInput"
                                    Layout.fillWidth: true
                                    placeholderText: "拍摄数据（如 1/125 F2.8 A 50mm）"
                                    text: preview.dateImprint.dataText === undefined ? "" : preview.dateImprint.dataText
                                    enabled: dateSection.dtEditable && dateSection.dtLive
                                    font.pixelSize: 11
                                    onEditingFinished: if (text !== (preview.dateImprint.dataText === undefined ? "" : preview.dateImprint.dataText)) preview.setDateImprint("dataText", text)
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: "字面"; color: dateSection.dtEditable && dateSection.dtLive ? Theme.text : Theme.muted; font.pixelSize: 11 }
                                ActionButton {
                                    objectName: "dateStyleLcd"
                                    text: "LCD"; accented: preview.dateImprint.style === "lcd"
                                    enabled: dateSection.dtEditable && dateSection.dtLive
                                    onClicked: preview.setDateImprint("style", "lcd")
                                }
                                ActionButton {
                                    objectName: "dateStyleDots"
                                    text: "点阵"; accented: preview.dateImprint.style === "dots"
                                    enabled: dateSection.dtEditable && dateSection.dtLive
                                    onClicked: preview.setDateImprint("style", "dots")
                                }
                                ActionButton {
                                    objectName: "dateStyleData"
                                    text: "数据"; accented: preview.dateImprint.style === "data"
                                    enabled: dateSection.dtEditable && dateSection.dtLive
                                    onClicked: preview.setDateImprint("style", "data")
                                }
                                Item { Layout.fillWidth: true }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: "位置"; color: dateSection.dtEditable && dateSection.dtLive ? Theme.text : Theme.muted; font.pixelSize: 11 }
                                ComboBox {
                                    objectName: "datePlacementCombo"
                                    Layout.fillWidth: true
                                    model: ["画面内（角上）", "片框（帧间）"]
                                    enabled: dateSection.dtEditable && dateSection.dtLive
                                    currentIndex: preview.dateImprint.placement === "rebate" ? 1 : 0
                                    onActivated: preview.setDateImprint("placement", currentIndex === 1 ? "rebate" : "frame")
                                }
                                Text { text: "角"; color: dateSection.dtEditable && dateSection.dtLive ? Theme.text : Theme.muted; font.pixelSize: 11 }
                                ComboBox {
                                    objectName: "dateCornerCombo"
                                    Layout.preferredWidth: 88
                                    model: ["右下", "左下", "右上", "左上"]
                                    readonly property var values: ["br", "bl", "tr", "tl"]
                                    enabled: dateSection.dtEditable && dateSection.dtLive
                                    currentIndex: values.indexOf(preview.dateImprint.corner === undefined ? "br" : preview.dateImprint.corner)
                                    onActivated: preview.setDateImprint("corner", values[currentIndex])
                                }
                            }
                            DateSlider { label: "曝光"; name: "ev"; from: -2; to: 8; step: 0.1; suffix: " EV" }
                            DateSlider { label: "大小"; name: "size"; from: 0.4; to: 3; step: 0.05; decimals: 2; suffix: "×" }
                            DateSlider { label: "横内缩"; name: "insetX"; from: 0; to: 30; step: 0.5; suffix: " mm" }
                            DateSlider { label: "纵内缩"; name: "insetY"; from: 0; to: 30; step: 0.5; suffix: " mm" }
                            Note { text: "日期背印从胶片背面曝光日期与拍摄数据：与胶片边缘同用，也可单独印在画面角上；「片框」位置把日期印在帧间，需要胶片边缘开启。" }
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
    // The Copy Settings group sheet (the Mac's C7): the groups tick at copy
    // time and the ticks are remembered; Paste writes only them — settings,
    // never solved numbers (SettingsClipboard's contract).
    Popup {
        id: copySheet
        objectName: "copySheet"
        anchors.centerIn: parent
        width: 320
        padding: 18
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        onVisibleChanged: {
            if (visible) ticks = library.clipboardGroups.slice()
            window.updateDialogState()
        }
        property var ticks: []
        background: Rectangle { color: Theme.card; border.color: Theme.line; radius: 7 }
        contentItem: ColumnLayout {
            spacing: 6
            Text { text: "复制设置"; color: Theme.text; font.pixelSize: 16; font.bold: true }
            Note { text: "勾选随复制保留的组；粘贴只写这些设置——目标照片按自己的测光重新放置，保留自己的 As Shot 白平衡、裁切与解码模式。" }
            Repeater {
                model: library.offeredClipboardGroups()
                delegate: CheckBox {
                    required property string modelData
                    Layout.fillWidth: true
                    text: window.groupName(modelData)
                    font.pixelSize: 11
                    checked: copySheet.ticks.indexOf(modelData) >= 0
                    onToggled: {
                        var next = copySheet.ticks.filter(function(v) { return v !== modelData })
                        if (checked) next.push(modelData)
                        copySheet.ticks = next
                    }
                }
            }
            Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.line; opacity: 0.5 }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                ActionButton {
                    objectName: "copySettingsButton"
                    Layout.fillWidth: true
                    accented: true
                    text: "复制 " + library.selectedCount + " 张所选"
                    enabled: copySheet.ticks.length > 0 && library.selectedCount > 0
                    onClicked: {
                        library.setClipboardGroups(copySheet.ticks)
                        library.copySettings()
                        copySheet.close()
                    }
                }
                ActionButton { text: "取消"; onClicked: copySheet.close() }
            }
        }
    }
}
