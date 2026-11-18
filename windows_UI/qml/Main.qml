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
    // The export's colour space, indexed into the engine's four destinations
    // (0 sRGB, 1 Display P3, 2 Adobe RGB (1998), 3 ProPhoto RGB). The names
    // are the engine's own, looked up through L() like the Mac's sheet.
    property int exportColour: 0
    readonly property var exportColourNames: ["sRGB", "Display P3", "Adobe RGB (1998)", "ProPhoto RGB"]
    // The one window's mode (§8): 图库 swaps the left column to the library
    // sidebar and the canvas to the grid; 冲洗/编辑 are the develop page this
    // window has always been (the filmstrip stays their browser; the full
    // shared-browser unification is future work).
    property string appMode: "develop"
    readonly property bool libraryMode: appMode === "library"
    readonly property bool editMode: appMode === "edit"
    // The light table (task 6.4): the third page, full-window except the
    // mode row this column keeps.
    readonly property bool lighttableMode: appMode === "lighttable"
    onAppModeChanged: if (window.libraryMode) gridPageRoot.takeFocus()
    // The grid's 放到印相台: carry the selection's frames through the
    // model's admission filter and true-mm sizes, then open the page. A
    // carry that lays nothing says so on the page's strip (the Mac's Add
    // sheet explains the one rule: developed, film edge or pair; a frame
    // the join cannot size yet — pixels still undecoded — waits for the
    // page's next sync).
    function carryToLightTable(uuids) {
        const frames = library.lightTableFrames(uuids)
        const placed = lightTable.carry(frames)
        window.appMode = "lighttable"
        // The page is a direct child (no Loader): setting `.item.notice`
        // wrote to undefined and threw, so the refusal never reached the
        // user — an empty table read as "no reaction".
        lightTablePage.notice = placed === 0
            ? Localization.L("Only pictures developed in SpektraLab go on the Light Table. Open a photo in Develop first, or give it a film edge or a half-frame pair — the table shows true millimetres, so it needs the develop's size.")
            : ""
    }
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
            "filmAndPaper": Localization.L("Film & Paper"), "filmExposure": Localization.L("Film Exposure"),
            "format": Localization.L("Frame"), "decode": Localization.L("Decode"),
            "enlarger": Localization.L("Enlarger"), "couplers": Localization.L("Colour couplers"),
            "preflash": Localization.L("Pre-flash"), "light": Localization.L("Light"),
            "curve": Localization.L("Curve"), "colour": Localization.L("Colour")
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
        if (exportFormat === 3) return [Localization.L("JPEG image (*.jpg *.jpeg)")]
        if (exportFormat === 1 || exportFormat === 2) return [Localization.L("PNG image (*.png)")]
        return [Localization.L("16-bit TIFF (*.tif *.tiff)")]
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
        title: Localization.epoch && Localization.L("Import one or more RAW photos")
        fileMode: FileDialog.OpenFiles
        nameFilters: Localization.epoch && [Localization.L("Camera RAW (*.arw *.nef *.nrw *.dng *.cr2 *.cr3 *.raf *.rw2 *.orf *.pef *.srw)"), Localization.L("All files (*)")]
        onVisibleChanged: window.updateDialogState()
        onAccepted: preview.openFiles(selectedFiles)
    }
    FileDialog {
        id: saveDialog
        title: Localization.epoch && Localization.L("Export the current frame · will not overwrite existing files")
        fileMode: FileDialog.SaveFile
        nameFilters: window.formatFilter()
        defaultSuffix: window.exportFormat === 3 ? "jpg" : window.exportFormat === 0 ? "tif" : "png"
        currentFolder: preview.sourceFolder
        onVisibleChanged: window.updateDialogState()
        onAccepted: preview.exportImage(selectedFile, window.exportFormat, window.exportQuality, window.exportLongEdge, window.exportColour)
    }
    FolderDialog {
        id: batchDialog
        title: Localization.epoch && Localization.L("Choose the batch-export folder · existing files are skipped")
        currentFolder: preview.sourceFolder
        onVisibleChanged: window.updateDialogState()
        onAccepted: preview.exportBatch(selectedFolder, window.exportFormat, window.exportQuality, window.exportLongEdge, window.exportColour)
    }
    Shortcut { sequences: [StandardKey.Open]; enabled: window.canBrowse; onActivated: openDialog.open() }
    // The develop keys stand down in 图库 (they would operate the previously
    // opened frame invisibly behind the grid).
    Shortcut { sequence: "Ctrl+E"; enabled: preview.hasFrame && !preview.busy && !window.cropMode && !window.libraryMode; onActivated: window.openExport() }
    Shortcut { sequence: "Ctrl+0"; enabled: preview.hasFrame && !window.libraryMode; onActivated: canvas.resetView(true) }
    Shortcut { sequence: "Ctrl+1"; enabled: preview.hasFrame && !window.cropMode && !window.libraryMode; onActivated: canvas.resetView(false) }
    // Split compare toggles like a view, not an edit; text fields keep V.
    Shortcut { sequence: "V"; enabled: preview.hasFrame && !window.cropMode && !window.editingText && !window.libraryMode; onActivated: preview.compareEnabled = !preview.compareEnabled }
    Shortcut { sequence: "Ctrl+Z"; enabled: preview.settingsEditable && preview.canUndo && !window.cropMode && !window.editingText && !window.libraryMode; onActivated: preview.undo() }
    Shortcut { sequences: ["Ctrl+Shift+Z", "Ctrl+Y"]; enabled: preview.settingsEditable && preview.canRedo && !window.cropMode && !window.editingText && !window.libraryMode; onActivated: preview.redo() }
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
        // The two constraints make the note wrap inside the rail instead of
        // widening it: a Text's implicit (single-line) width is the layout's
        // preferred AND minimum contribution by default, so a long note
        // would stretch the whole right rail past the window and every
        // fillWidth row in it with it. Preferred 0 keeps the layout's
        // implicit width honest; minimum 0 lets the layout squeeze the note
        // to the rail and the wrap takes over.
        Layout.fillWidth: true
        Layout.preferredWidth: 0
        Layout.minimumWidth: 0
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
        Adjustment { label: Localization.epoch && Localization.L("Strength"); setting: effect.amountKey; fallback: 1; from: 0; to: effect.maximum; available: preview.adjustments[effect.activeKey] !== false }
    }
    component FilmGroup: ColumnLayout {
        id: group
        property bool positive: false
        Layout.fillWidth: true; spacing: 3
        Text { text: group.positive ? Localization.epoch && Localization.L("Slide · Positive") : Localization.epoch && Localization.L("Negative film · Negative"); color: Theme.muted; font.pixelSize: 10; font.bold: true }
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
        readonly property string sideName: Localization.epoch && Localization.L(window.pairActive && preview.pair.turned === true
            ? (side === 0 ? "Top" : "Bottom") : (side === 0 ? "Left" : "Right"))
        Layout.fillWidth: true; spacing: 6
        Text { text: Localization.epoch && Localization.L("%@ hole", [holePanel.sideName]) + (holePanel.ok && holePanel.hole.sourceName !== undefined && holePanel.hole.sourceName.length > 0 ? " · " + holePanel.hole.sourceName : ""); color: Theme.text; font.pixelSize: 11; font.bold: true }
        RowLayout {
            Layout.fillWidth: true
            Text { text: Localization.epoch && Localization.L("Exposure"); color: Theme.text; font.pixelSize: 11 }
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
            Text { text: Localization.epoch && Localization.L("Meter reading"); color: Theme.text; font.pixelSize: 11 }
            Item { Layout.fillWidth: true }
            Text {
                // The reading arrives with the compose reply (the meter cache
                // holds while the hole is dragged, PAIR-WIRING §4.2); while a
                // job runs with no reading yet it reads as metering.
                text: !holePanel.ok ? ""
                    : holePanel.hole.meteredEv === undefined
                        ? (preview.busy ? Localization.epoch && Localization.L("Metering…") : Localization.epoch && Localization.L("No reading yet"))
                        : Number(holePanel.hole.meteredEv).toFixed(2) + " EV"
                color: Theme.muted; font.pixelSize: 10
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Switch {
                text: Localization.epoch && Localization.L("Date"); font.pixelSize: 11
                palette.windowText: Theme.text
                checked: holePanel.ok && holePanel.hole.dated === true
                enabled: preview.settingsEditable
                onToggled: preview.setPairHole(holePanel.side, "dated", checked)
            }
            TextField {
                objectName: "pairHole" + holePanel.side + "DateText"
                Layout.fillWidth: true
                placeholderText: Localization.epoch && Localization.L("e.g. '26 11 15")
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
            Text { text: Localization.epoch && Localization.L("Print trim (stored)"); color: Theme.muted; font.pixelSize: 10; font.bold: true }
            Text { text: Localization.epoch && Localization.L("Brightness ") + (holePanel.ok ? (holePanel.hole.trim.printExposure >= 0 ? "+" : "") + Number(holePanel.hole.trim.printExposure).toFixed(2) : ""); color: Theme.muted; font.pixelSize: 10 }
            Text { text: Localization.epoch && Localization.L("Yellow filter %@", [holePanel.ok ? (holePanel.hole.trim.yFilterShift >= 0 ? "+" : "") + Number(holePanel.hole.trim.yFilterShift).toFixed(2) : ""]); color: Theme.muted; font.pixelSize: 10 }
            Text { text: Localization.epoch && Localization.L("Magenta filter %@", [holePanel.ok ? (holePanel.hole.trim.mFilterShift >= 0 ? "+" : "") + Number(holePanel.hole.trim.mFilterShift).toFixed(2) : ""]); color: Theme.muted; font.pixelSize: 10 }
            Text { text: Localization.epoch && Localization.L("Pre-flash %@", [holePanel.ok ? Number(holePanel.hole.trim.preflashExposure).toFixed(2) : ""]); color: Theme.muted; font.pixelSize: 10 }
        }
        // Scene Placement per hole is deferred: requestLatitude() carries no
        // region parameter yet and refuses on a pair (PAIR-WIRING §6.2/§6.3
        // want the per-hole region request), so there is deliberately no
        // 应用映射/Fit row here to build a dead path against.
        Note { text: Localization.epoch && Localization.L("The four print-trim rows are saved with the pair's record; per-hole print cropping is wired in a later task and changes no pixels today.") }
    }

    // The window shell (§8): one top bar over every page — the mode row
    // centred with the language picker at the far right — and the mode's
    // content below. A mode press swaps the content, never the shell.
    ColumnLayout {
        anchors.fill: parent; spacing: 0
        Rectangle {
            objectName: "modeSwitch"
            Layout.fillWidth: true; Layout.preferredHeight: Theme.topHeight; color: Theme.card
            Rectangle { width: parent.width; height: 1; color: Theme.line; opacity: 0.35; anchors.bottom: parent.bottom }
            RowLayout {
                anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 12; spacing: 6
                Item { Layout.fillWidth: true }
                RowLayout {
                    spacing: 3
                    component ModeButton: ActionButton {
                        required property string label
                        required property string mode
                        implicitHeight: 25
                        implicitWidth: label.length * 11 + 30
                        font.pixelSize: 12
                        text: label
                        accented: window.appMode === mode
                        onClicked: window.appMode = mode
                    }
                    ModeButton { objectName: "modeLibraryButton"; label: Localization.epoch && Localization.L("Library"); mode: "library" }
                    ModeButton { objectName: "modeDevelopButton"; label: Localization.epoch && Localization.L("Develop"); mode: "develop" }
                    ModeButton { objectName: "modeEditButton"; label: Localization.epoch && Localization.L("Edit"); mode: "edit" }
                    ModeButton { objectName: "modeLightTableButton"; label: Localization.epoch && Localization.L("Light Table"); mode: "lighttable" }
                }
                Item { Layout.fillWidth: true }
                // The language picker: each language names itself, so
                // the rows are the same string in all four UI
                // languages and deliberately bypass L(). The char
                // codes keep the CJK out of the source (the gate's
                // unwrapped-literal scan would flag plain literals;
                // these names are not translations).
                ToolButton {
                    id: languageButton
                    objectName: "languageButton"
                    readonly property var rows: [
                        { "code": "en", "name": "English" },
                        { "code": "zh-Hans", "name": String.fromCharCode(0x7B80, 0x4F53, 0x4E2D, 0x6587) },
                        { "code": "zh-Hant", "name": String.fromCharCode(0x7E41, 0x9AD4, 0x4E2D, 0x6587) },
                        { "code": "ja", "name": String.fromCharCode(0x65E5, 0x672C, 0x8A9E) }
                    ]
                    function ownName(code) {
                        for (var i = 0; i < rows.length; ++i)
                            if (rows[i].code === code) return rows[i].name
                        return code
                    }
                    text: ownName(Localization.language)
                    font.pixelSize: 11
                    implicitHeight: 25
                    onClicked: languageMenu.popup()
                    Menu {
                        id: languageMenu
                        Repeater {
                            model: languageButton.rows
                            MenuItem {
                                required property var modelData
                                text: modelData.name
                                checkable: true
                                checked: Localization.language === modelData.code
                                onTriggered: Localization.language = modelData.code
                            }
                        }
                    }
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true; Layout.fillHeight: true; spacing: 1
            Rectangle {
            // 印相台是满宽的整窗页（§8），它的工具行就在顶栏之下——左栏只在
            // 图库（侧栏）与冲洗/编辑（工具栏）有意义。
            visible: !window.lighttableMode
            Layout.preferredWidth: Theme.leftWidth; Layout.fillHeight: true; color: Theme.card
            ColumnLayout {
                anchors.fill: parent; spacing: 0
                // The develop rail: 冲洗/编辑's column content, unchanged.
                ScrollView {
                    visible: !(window.libraryMode || window.lighttableMode)
                    Layout.fillWidth: true; Layout.fillHeight: true; clip: true; contentWidth: availableWidth
                    ColumnLayout {
                        width: parent.width; spacing: 0
                        Section {
                            title: Localization.epoch && Localization.L("Navigator")
                            Rectangle {
                                Layout.fillWidth: true; Layout.preferredHeight: 132; color: Theme.wellDark
                                FrameCanvas { anchors.fill: parent; anchors.margins: 6; image: preview.previewImage; enabled: false }
                                Text { anchors.centerIn: parent; visible: !preview.hasFrame; text: Localization.epoch && Localization.L("Import photos to begin"); color: Theme.muted; font.pixelSize: 11 }
                            }
                            RowLayout {
                                ActionButton { text: Localization.epoch && Localization.L("Fit"); implicitHeight: 23; enabled: preview.hasFrame; onClicked: canvas.resetView(true) }
                                Item { Layout.fillWidth: true }
                                Text { text: preview.hasFrame ? canvas.scalePercent.toFixed(1) + "%" : ""; color: Theme.muted; font.pixelSize: 10 }
                            }
                        }
                        Section {
                            title: Localization.epoch && Localization.L("Film")
                            FilmGroup { positive: true }
                            FilmGroup { positive: false }
                        }
                        Section {
                            title: Localization.epoch && Localization.L("Paper")
                            Note { visible: preview.filmIsPositive; text: Localization.epoch && Localization.L("Slide film is scanned directly; no paper is used."); color: Theme.accent }
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
                            title: Localization.epoch && Localization.L("Crop image")
                            Note {
                                visible: window.pairActive
                                text: Localization.epoch && Localization.L("Half-frame pair · picture-level crop and orientation do not apply to a pair: each hole's picture is placed inside its hole (see the Half-frame pair panel).")
                                color: Theme.accent
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                ComboBox {
                                    id: cropAspect; Layout.fillWidth: true
                                    model: Localization.epoch && [Localization.L("Free ratio"), "1:1", "3:2", "4:3", "16:9"]; enabled: window.canEditCrop
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
                                ActionButton { text: Localization.epoch && Localization.L("Reset"); enabled: window.canEditCrop; onClicked: { window.cropMode = false; preview.resetCrop(); canvas.resetView(true) } }
                            }
                            ActionButton { Layout.fillWidth: true; text: window.cropMode ? Localization.epoch && Localization.L("Finish crop") : Localization.epoch && Localization.L("Drag in the picture to crop"); accented: window.cropMode; enabled: window.canEditCrop; onClicked: window.cropMode ? window.finishCrop() : window.startCrop() }
                            Note { text: window.cropMode ? Localization.epoch && Localization.L("Drag a rectangle on the unrotated original; orientation and straighten return when you finish. Enter confirms, Esc cancels.") : Localization.epoch && Localization.L("Preview and export share the same crop and orientation; the original RAW is untouched.") }
                            RowLayout {
                                Layout.fillWidth: true
                                ActionButton { objectName: "rotateLeftButton"; Layout.fillWidth: true; text: Localization.epoch && Localization.L("Rotate left 90°"); enabled: window.canEditCrop && !window.cropMode; onClicked: { preview.rotateCounterClockwise(); canvas.resetView(true) } }
                                ActionButton { objectName: "rotateRightButton"; Layout.fillWidth: true; text: Localization.epoch && Localization.L("Rotate right 90°"); enabled: window.canEditCrop && !window.cropMode; onClicked: { preview.rotateClockwise(); canvas.resetView(true) } }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                ActionButton { objectName: "flipHorizontalButton"; Layout.fillWidth: true; text: Localization.epoch && Localization.L("Flip horizontally"); accented: preview.flipHorizontal; enabled: window.canEditCrop && !window.cropMode; onClicked: preview.setFlipHorizontal(!preview.flipHorizontal) }
                                ActionButton { objectName: "flipVerticalButton"; Layout.fillWidth: true; text: Localization.epoch && Localization.L("Flip vertically"); accented: preview.flipVertical; enabled: window.canEditCrop && !window.cropMode; onClicked: preview.setFlipVertical(!preview.flipVertical) }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: Localization.epoch && Localization.L("Straighten"); color: Theme.text; font.pixelSize: 11 }
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
                            Note { text: Localization.epoch && Localization.L("Straighten applies when the slider is released; the edges tighten automatically to avoid blank areas.") }
                            ActionButton { objectName: "resetGeometryButton"; Layout.fillWidth: true; text: Localization.epoch && Localization.L("Reset crop & orientation"); enabled: window.canEditCrop && !window.cropMode; onClicked: { preview.resetGeometry(); canvas.resetView(true) } }
                        }
                        Section {
                            title: Localization.epoch && Localization.L("Enlarger · paper brightness")
                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: Localization.epoch && Localization.L("Brightness"); color: Theme.text; font.pixelSize: 11 }
                                Item { Layout.fillWidth: true }
                                Text { text: preview.filmIsPositive ? Localization.epoch && Localization.L("Not applicable") : (preview.exposureEv >= 0 ? "+" : "") + preview.exposureEv.toFixed(1) + " EV"; color: Theme.muted; font.pixelSize: 10 }
                            }
                            Slider {
                                objectName: "exposureSlider"; Layout.fillWidth: true; from: -2; to: 2; stepSize: 0.1; implicitHeight: 24
                                value: preview.exposureEv; enabled: preview.settingsEditable && !preview.filmIsPositive
                                onPressedChanged: { if (pressed) preview.beginEditGesture("exposureEv"); else preview.endEditGesture() }
                                onMoved: preview.exposureEv = value
                            }
                            Adjustment { label: Localization.epoch && Localization.L("Yellow filter"); setting: "yFilterShift"; from: -1; to: 1; step: 0.01; decimals: 2; available: !preview.filmIsPositive }
                            Adjustment { label: Localization.epoch && Localization.L("Magenta filter"); setting: "mFilterShift"; from: -1; to: 1; step: 0.01; decimals: 2; available: !preview.filmIsPositive }
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
                                    text: Localization.epoch && Localization.L("Mask"); font.pixelSize: 11
                                    palette.windowText: Theme.text
                                    checked: preview.mask.active
                                    enabled: preview.settingsEditable && !preview.filmIsPositive
                                    onToggled: preview.setMask("active", checked)
                                }
                                Item { Layout.fillWidth: true }
                                Text {
                                    text: !preview.mask.active ? Localization.epoch && Localization.L("Close") : Localization.epoch && Localization.L("Applied")
                                    color: Theme.muted; font.pixelSize: 10
                                }
                            }
                            MaskSlider { label: Localization.epoch && Localization.L("Lift highlights"); name: "highlights"; from: 0; to: 3; step: 0.05; decimals: 2; suffix: " stops" }
                            MaskSlider { label: Localization.epoch && Localization.L("Pull shadows"); name: "shadows"; from: 0; to: 3; step: 0.05; decimals: 2; suffix: " stops" }
                            MaskSlider { label: Localization.epoch && Localization.L("Core width"); name: "core"; from: 0.1; to: 3; step: 0.05; decimals: 2; suffix: " stops" }
                            MaskSlider { label: Localization.epoch && Localization.L("Scale"); name: "scale"; from: 0.002; to: 0.12; step: 0.002; decimals: 3 }
                            ActionButton {
                                objectName: "resetMaskButton"
                                Layout.fillWidth: true
                                text: Localization.epoch && Localization.L("Reset mask")
                                enabled: preview.settingsEditable && !preview.filmIsPositive && preview.hasFrame
                                onClicked: {
                                    preview.setMask("active", false)
                                    preview.setMask("highlights", 0)
                                    preview.setMask("shadows", 0)
                                    preview.setMask("core", 1.0)
                                    preview.setMask("scale", 0.03)
                                }
                            }
                            Note { text: Localization.epoch && Localization.L("The contrast mask lifts the paper's highlights and pulls its shadows locally: the switch and sliders are engine parameters and re-render without re-decoding.") }
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
                visible: !(window.libraryMode || window.lighttableMode)
                anchors.fill: parent; spacing: 0
                Rectangle {
                    Layout.fillWidth: true; implicitHeight: Theme.topHeight; color: Theme.card
                    RowLayout {
                        anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 8; spacing: 6
                        ActionButton { text: Localization.epoch && Localization.L("Import RAW"); enabled: window.canBrowse; onClicked: openDialog.open(); implicitHeight: 27 }
                        ActionButton { text: Localization.epoch && Localization.L("Export"); enabled: preview.hasFrame && !preview.busy && !window.cropMode; onClicked: window.openExport(); implicitHeight: 27 }
                        ActionButton { text: Localization.epoch && Localization.L("Crop image"); accented: window.cropMode; enabled: window.canEditCrop; onClicked: window.cropMode ? window.finishCrop() : window.startCrop(); implicitHeight: 27 }
                        ActionButton { objectName: "compareButton"; text: Localization.epoch && Localization.L("Compare"); accented: preview.compareEnabled; enabled: preview.hasFrame && !window.cropMode; onClicked: preview.compareEnabled = !preview.compareEnabled; implicitHeight: 27; ToolTip.visible: hovered; ToolTip.text: Localization.epoch && Localization.L("The plain decode on the left, the developed print on the right · V") }
                        ActionButton { objectName: "undoButton"; text: Localization.epoch && Localization.L("Undo"); enabled: preview.settingsEditable && preview.canUndo && !window.cropMode; onClicked: preview.undo(); implicitHeight: 27; ToolTip.visible: hovered; ToolTip.text: Localization.epoch && Localization.L("Undo the photo's last adjustment · Ctrl+Z") }
                        ActionButton { objectName: "redoButton"; text: Localization.epoch && Localization.L("Redo"); enabled: preview.settingsEditable && preview.canRedo && !window.cropMode; onClicked: preview.redo(); implicitHeight: 27; ToolTip.visible: hovered; ToolTip.text: Localization.epoch && Localization.L("Redo the photo's adjustment · Ctrl+Shift+Z / Ctrl+Y") }
                        Item { Layout.fillWidth: true }
                        Text { text: preview.hasFrame ? canvas.scalePercent.toFixed(1) + "%" : ""; color: Theme.muted; font.pixelSize: 10; horizontalAlignment: Text.AlignRight }
                        ActionButton { text: Localization.epoch && Localization.L("Fit"); accented: canvas.fit; enabled: preview.hasFrame; onClicked: canvas.resetView(true); implicitHeight: 27 }
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
                                // mouse.x is local to this 16px strip — map it
                                // into the overlay's space before dividing.
                                var p = mapToItem(compareOverlay, mouseX, 0)
                                canvas.comparePosition = Math.max(0, Math.min(1, (p.x - b.x) / b.width))
                            }
                            onPressed: function(mouse) { retarget(mouse.x) }
                            onPositionChanged: function(mouse) { if (pressed) retarget(mouse.x) }
                        }
                    }
                    Column {
                        anchors.centerIn: parent; spacing: 16; visible: !preview.hasFrame
                        Text { anchors.horizontalCenter: parent.horizontalCenter; text: "SpektraLab"; color: "#e5e2dc"; font.pixelSize: 32; font.letterSpacing: 1 }
                        Text { anchors.horizontalCenter: parent.horizontalCenter; text: Localization.epoch && Localization.L("Import RAW; pick the film and paper per photo"); color: "#d0cdc6"; font.pixelSize: 13 }
                        Text { anchors.horizontalCenter: parent.horizontalCenter; text: Localization.epoch && Localization.L("You can drop several photos at once"); color: "#c0bdb7"; font.pixelSize: 11 }
                    }
                    DropArea { anchors.fill: parent; enabled: window.canBrowse; onDropped: function(drop) { if (drop.hasUrls) preview.openFiles(drop.urls) } }
                    Rectangle {
                        anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 26
                        width: window.cropMode ? 78 : 48; height: 20; radius: 2; color: "#b32d2d2c"; visible: preview.hasFrame
                        Text { anchors.centerIn: parent; text: window.cropMode ? Localization.epoch && Localization.L("Cropping the original") : "FULL"; color: Theme.text; font.pixelSize: 9; font.letterSpacing: 1 }
                    }
                    BusyIndicator { anchors.centerIn: parent; running: preview.busy; visible: running; width: 44; height: 44 }
                }
                Rectangle {
                    Layout.fillWidth: true; implicitHeight: 28; color: Theme.card
                    Text { anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 12; verticalAlignment: Text.AlignVCenter; text: preview.hasFrame ? Localization.epoch && Localization.L("%@ × %@   ·   SDR sRGB / 8-bit preview   ·   %@", [preview.metadata.width, preview.metadata.height, preview.dirty ? Localization.L("Updating the preview…") : Localization.L("Selection and adjustments apply automatically")]) : Localization.epoch && Localization.L("Full-resolution preview"); color: preview.dirty ? Theme.accent : Theme.muted; font.pixelSize: 10; elide: Text.ElideRight }
                }
                Rectangle {
                    Layout.fillWidth: true; implicitHeight: Theme.stripHeight; color: Theme.card
                    Rectangle { width: parent.width; height: 1; color: Theme.line; opacity: 0.5 }
                    ColumnLayout {
                        anchors.fill: parent; anchors.margins: 8; spacing: 6
                        RowLayout {
                            Layout.fillWidth: true; spacing: 6
                            Text { text: Localization.epoch && Localization.L("%@ photos · %@ selected", [preview.libraryItems.length, preview.selectedCount]); color: Theme.muted; font.pixelSize: 10 }
                            Text {
                                Layout.fillWidth: true
                                visible: window.pairNotice.length > 0
                                text: window.pairNotice
                                color: "#ffb39f"; font.pixelSize: 10
                                elide: Text.ElideRight
                            }
                            Item { Layout.fillWidth: true; visible: window.pairNotice.length === 0 }
                            ActionButton { text: Localization.epoch && Localization.L("Select All"); implicitHeight: 23; font.pixelSize: 10; enabled: window.canBrowse && preview.libraryItems.length > 0; onClicked: preview.selectAllPhotos(true) }
                            ActionButton { text: Localization.epoch && Localization.L("Clear selection"); implicitHeight: 23; font.pixelSize: 10; enabled: window.canBrowse && preview.selectedCount > 0; onClicked: preview.selectAllPhotos(false) }
                            ActionButton { text: Localization.epoch && Localization.L("Sync settings"); implicitHeight: 23; font.pixelSize: 10; enabled: window.canBrowse && preview.hasFrame && preview.selectedCount > 0; onClicked: preview.syncSelectedSettings(false); ToolTip.visible: hovered; ToolTip.text: Localization.epoch && Localization.L("Synced the film, paper, decode and effects to the selected photos; the crop is not synced.") }
                            // 建对 (PAIR-WIRING §5): the two selected photographs
                            // become one pair library item. The controller
                            // refuses (and changes nothing) while the engine is
                            // busy, when a source is itself a pair, or when an
                            // identical pair already sits in the library (that
                            // last one selects the existing pair and returns
                            // true), so the button only surfaces the refusal.
                            ActionButton {
                                objectName: "createPairButton"
                                text: Localization.epoch && Localization.L("Make pair"); implicitHeight: 23; font.pixelSize: 10
                                enabled: window.canBrowse && window.pairCandidates.length === 2
                                onClicked: {
                                    if (preview.createPair(window.pairCandidates[0].url, window.pairCandidates[1].url)) {
                                        window.pairNotice = ""
                                    } else {
                                        window.pairNotice = Localization.L("Cannot make the pair: wait for the current operation to finish, or pick two different photos (a half-frame pair cannot be a hole)")
                                        pairNoticeTimer.restart()
                                    }
                                }
                                ToolTip.visible: hovered
                                ToolTip.text: Localization.epoch && Localization.L("Combines the first two selected photos (in library order) into one half-frame pair; the pair appears as its own library item, and each hole is edited in the Half-frame pair panel on the right.")
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
                                Text { anchors.centerIn: parent; anchors.verticalCenterOffset: -8; text: modelData.error ? Localization.epoch && Localization.L("Cannot be read") : Localization.epoch && Localization.L("Click to preview"); color: modelData.error ? "#ffb39f" : Theme.muted; font.pixelSize: 10 }
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
                id: gridPageRoot
                objectName: "libraryPageItem"
                anchors.fill: parent
                visible: window.libraryMode
                onOpenRequested: function(path) {
                    library.openInDevelop(path)
                    window.appMode = "develop"
                }
                // 放到印相台 (task 6.4): the grid's context menu carries the
                // selection into the light table.
                onCarryRequested: function(uuids) { window.carryToLightTable(uuids) }
                onDeleteRequested: function(uuids) { library.trashImages(uuids) }
            }
            // The light table page (task 6.4): the full-window pad. Its
            // tools live in the page's own header; the left column keeps
            // only the mode row in this mode.
            LightTablePage {
                id: lightTablePage
                objectName: "lightTablePageItem"
                anchors.fill: parent
                visible: window.lighttableMode
            }
        }
        Rectangle {
            // 参数·显影前 belongs to 冲洗/编辑: the library's grid uses the
            // full width beside the sidebar (§8 — a mode press swaps the
            // content, and the library page has no print rows to show).
            visible: !window.libraryMode && !window.lighttableMode
            Layout.preferredWidth: Theme.rightWidth; Layout.fillHeight: true; color: Theme.card
            ColumnLayout {
                id: railColumn
                anchors.fill: parent; anchors.rightMargin: 12; spacing: 0
                Text { text: Localization.epoch && (window.editMode ? Localization.L("Edit · the finished print") : Localization.L("Parameters · pre-develop")); color: Theme.text; font.pixelSize: 14; font.bold: true; Layout.leftMargin: 14; Layout.preferredHeight: Theme.topHeight; verticalAlignment: Text.AlignVCenter }
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
                            title: Localization.epoch && Localization.L("Half-frame pair")
                            visible: window.pairActive
                            Note {
                                visible: window.pairActive
                                text: Localization.epoch && Localization.L("A pair owns no photos of its own: each hole follows its source photo's decode and crop; the piece's film, paper and film rebate are adjusted in the shared panels below.")
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: Localization.epoch && Localization.L("Record"); color: Theme.text; font.pixelSize: 11 }
                                Item { Layout.fillWidth: true }
                                Text {
                                    text: window.pairActive && preview.pair.saved === true ? Localization.epoch && Localization.L("Saved") : Localization.epoch && Localization.L("Saving…")
                                    color: Theme.muted; font.pixelSize: 10
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: Localization.epoch && Localization.L("Spacing"); color: window.pairOnStrip ? Theme.muted : Theme.text; font.pixelSize: 11 }
                                Item { Layout.fillWidth: true }
                                Text {
                                    text: window.pairActive
                                        ? (window.pairOnStrip ? Localization.epoch && Localization.L("1.0 mm (strip-fixed)") : Number(preview.pair.spacingMM).toFixed(2) + " mm")
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
                            Note { visible: window.pairOnStrip; text: Localization.epoch && Localization.L("With the film rebate on, the pair rides its strip and the gap is fixed to the strip's 1 mm; turn the film rebate off to adjust it.") }
                            // Wide-gap dated bare pair (PAIR-WIRING §2's gate
                            // shape): with a date on and no film edge, the
                            // engine's 37:24 shape gate refuses spacing above
                            // ~2.85 mm, so a record-loaded wide gap warns here
                            // (the slider itself caps at 2.0).
                            Note {
                                visible: window.pairActive && !window.pairOnStrip && window.pairAnyDated &&
                                         window.pairSpacingMM > 2.85
                                text: Localization.epoch && Localization.L("With a date on and no film rebate, the gap tops out near 2.85 mm: a wider gap exceeds the engine's format-shape constraint and the compose is refused.")
                                color: Theme.accent
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Switch {
                                    objectName: "pairTurnedSwitch"
                                    text: Localization.epoch && Localization.L("Turn (stack)"); font.pixelSize: 11
                                    palette.windowText: Theme.text
                                    checked: window.pairActive && preview.pair.turned === true
                                    enabled: preview.settingsEditable
                                    onToggled: preview.setPairPiece("turned", checked)
                                }
                                Item { Layout.fillWidth: true }
                                Text {
                                    text: window.pairActive && preview.pair.turned === true ? Localization.epoch && Localization.L("Stacked vertically") : Localization.epoch && Localization.L("Side by side")
                                    color: Theme.muted; font.pixelSize: 10
                                }
                            }
                            Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.line; opacity: 0.35 }
                            PairHoleSection { side: 0 }
                            Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.line; opacity: 0.35 }
                            PairHoleSection { side: 1 }
                            Note { text: Localization.epoch && Localization.L("Exposure and placement changes re-compose the whole piece; date changes reprint the same negative. Undo/redo walks the pair's own history.") }
                        }
                        // Output histogram: 4×256 bins over the published
                        // preview pixels (geometry + grade applied), the same
                        // surface the canvas draws. Four normalized polylines —
                        // luma behind, R/G/B over it — against the max bin.
                        Section {
                            id: histogramSection
                            title: Localization.epoch && Localization.L("Histogram")
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
                                HistogramKey { label: Localization.epoch && Localization.L("Red"); swatch: "#e8524e" }
                                HistogramKey { label: Localization.epoch && Localization.L("Green"); swatch: "#62c462" }
                                HistogramKey { label: Localization.epoch && Localization.L("Blue"); swatch: "#5d7de8" }
                                HistogramKey { label: Localization.epoch && Localization.L("Brightness"); swatch: "#bdbdbd" }
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
                            title: Localization.epoch && Localization.L("Latitude")
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
                                    parts.push(Localization.epoch && Localization.L("Latitude %@ stops", [Number(m.latitude_stops).toFixed(1)]))
                                var s = latitudeSection.reply && latitudeSection.reply.suggested !== undefined ? latitudeSection.reply.suggested : null
                                if (s && s.valid !== undefined)
                                    parts.push(Localization.epoch && Localization.L("Suggested pull: highlights %@ / shadows %@ EV", [Number(s.highlight_pull_back === undefined ? 0 : s.highlight_pull_back).toFixed(1), Number(s.shadow_pull_back === undefined ? 0 : s.shadow_pull_back).toFixed(1)]))
                                var fit = latitudeSection.reply && latitudeSection.reply.fit !== undefined ? latitudeSection.reply.fit : null
                                if (fit && fit.valid !== undefined && !fit.valid) parts.push(Localization.epoch && Localization.L("The fit did not pass"))
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
                                    text: preview.latitudeBusy ? Localization.epoch && Localization.L("Analysing…") : Localization.epoch && Localization.L("Analyse")
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
                                    text: Localization.epoch && Localization.L("Click Analyse to measure the current film and scene")
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
                                    text: Localization.epoch && Localization.L("Apply mapping"); font.pixelSize: 11
                                    palette.windowText: Theme.text
                                    checked: preview.sceneLatitudeActive
                                    enabled: preview.settingsEditable
                                    onToggled: preview.setSceneLatitude(checked)
                                }
                                Item { Layout.fillWidth: true }
                                Text {
                                    text: !preview.sceneLatitudeActive ? Localization.epoch && Localization.L("Close") : Localization.epoch && Localization.L("Applied")
                                    color: Theme.muted; font.pixelSize: 10
                                }
                            }
                            ActionButton {
                                objectName: "commitFitButton"
                                Layout.fillWidth: true
                                text: Localization.epoch && Localization.L("Commit fit")
                                // The fit lands on the first curve; a pair's
                                // commit waits for the per-hole region (§6.3),
                                // and the controller refuses it on a pair.
                                enabled: latitudeSection.fitValid && preview.settingsEditable && preview.hasFrame && !window.pairActive
                                onClicked: preview.commitLatitudeFit()
                            }
                            Note { visible: latitudeSection.summary.length > 0; text: latitudeSection.summary; color: Theme.text }
                            Note {
                                visible: window.pairActive
                                text: Localization.epoch && Localization.L("Half-frame pair · Analyse and Commit fit do not yet apply to a pair (per-hole region measurement is a later task); Apply mapping still acts on the whole piece.")
                                color: Theme.accent
                            }
                            Note { text: Localization.epoch && Localization.L("Analyse only reads the current develop and does not change the picture; Apply mapping places the scene with the committed curve, and Commit fit writes the solved highlight/shadow pull-backs.") }
                        }
                        Section { visible: window.editMode;
                            title: Localization.epoch && Localization.L("Edit · Light")
                            Layer2Adjustment { label: Localization.epoch && Localization.L("Exposure"); setting: "exposure"; from: -5; to: 5; step: 0.05; decimals: 2; suffix: " EV" }
                            Layer2Adjustment { label: Localization.epoch && Localization.L("Brightness"); setting: "brightness"; from: -100; to: 100; step: 1; decimals: 0 }
                            Layer2Adjustment { label: Localization.epoch && Localization.L("Contrast"); setting: "contrast"; from: -100; to: 100; step: 1; decimals: 0 }
                            Layer2Adjustment { label: Localization.epoch && Localization.L("Highlights"); setting: "highlights"; from: -100; to: 100; step: 1; decimals: 0 }
                            Layer2Adjustment { label: Localization.epoch && Localization.L("Shadows"); setting: "shadows"; from: -100; to: 100; step: 1; decimals: 0 }
                            Layer2Adjustment { label: Localization.epoch && Localization.L("Black point"); setting: "blackPoint"; from: 0; to: 100; step: 1; decimals: 0 }
                            Layer2Adjustment { label: Localization.epoch && Localization.L("White point"); setting: "whitePoint"; from: 0; to: 100; step: 1; decimals: 0 }
                            ActionButton { Layout.fillWidth: true; text: Localization.epoch && Localization.L("Reset light"); enabled: preview.settingsEditable && preview.hasFrame; onClicked: window.setLayer2Zero(["exposure", "brightness", "contrast", "highlights", "shadows", "blackPoint", "whitePoint"]) }
                            Note { text: Localization.epoch && Localization.L("Graded adjustments act on the developed picture: live, without triggering a re-render, and pixel-identical to the export.") }
                        }
                        Section { visible: window.editMode;
                            title: Localization.epoch && Localization.L("Edit · Colour")
                            Layer2Adjustment { label: Localization.epoch && Localization.L("Temperature"); setting: "temperature"; from: -100; to: 100; step: 1; decimals: 0 }
                            Layer2Adjustment { label: Localization.epoch && Localization.L("Tint"); setting: "tint"; from: -100; to: 100; step: 1; decimals: 0 }
                            Layer2Adjustment { label: Localization.epoch && Localization.L("Saturation"); setting: "saturation"; from: -100; to: 100; step: 1; decimals: 0 }
                            ActionButton { Layout.fillWidth: true; text: Localization.epoch && Localization.L("Reset colour"); enabled: preview.settingsEditable && preview.hasFrame; onClicked: window.setLayer2Zero(["temperature", "tint", "saturation"]) }
                        }
                        Section { visible: false;
                            title: Localization.epoch && Localization.L("Curve")
                            CurveEditor {}
                        }
                        Section { visible: window.editMode;
                            title: Localization.epoch && Localization.L("Edit · Colour balance")
                            ColorBalance {}
                            ActionButton { Layout.fillWidth: true; text: Localization.epoch && Localization.L("Reset colour balance"); enabled: preview.settingsEditable && preview.hasFrame; onClicked: window.resetColorBalance() }
                        }
                        Section { visible: window.editMode;
                            title: Localization.epoch && Localization.L("Edit · Vignette")
                            Layer2Adjustment { label: Localization.epoch && Localization.L("Amount"); setting: "vignetteAmount"; readKey: "vignette.amount"; from: -100; to: 100; step: 1; decimals: 0 }
                            Layer2Adjustment { label: Localization.epoch && Localization.L("Midpoint"); setting: "vignetteMidpoint"; readKey: "vignette.midpoint"; fallback: 50; from: 0; to: 100; step: 1; decimals: 0 }
                            ActionButton { Layout.fillWidth: true; text: Localization.epoch && Localization.L("Reset vignette"); enabled: preview.settingsEditable && preview.hasFrame; onClicked: { preview.setLayer2("vignetteAmount", 0); preview.setLayer2("vignetteMidpoint", 50) } }
                            Note { text: Localization.epoch && Localization.L("Negative values darken the picture's edges, positive values brighten; the midpoint decides where in the picture the vignette begins.") }
                        }
                        Section { visible: !window.editMode;
                            id: cameraSection
                            title: Localization.epoch && Localization.L("Input / camera")
                            Note {
                                visible: window.pairActive
                                text: Localization.epoch && Localization.L("Half-frame pair · film exposure, white balance and metering are controlled at piece level: the pair's exposure is always 0 (each hole's exposure is in the Half-frame pair panel) and the decode follows each hole's source photo.")
                                color: Theme.accent
                            }
                            Adjustment { label: Localization.epoch && Localization.L("Film Exposure"); setting: "filmExposureEv"; from: -4; to: 4; step: 0.1; decimals: 1; suffix: " EV"; available: !window.pairActive }
                            Note { text: Localization.epoch && Localization.L("Adjusts the scene's brightness before the film exposure. Each photo's effects and crop save automatically and return on the next import.") }
                            // 白平衡（RAW 解码侧的 Temperature/Tint，Adobe DNG 口径）：
                            // 0 = 自动（As Shot）。改动是解码参数，会重新解码当前照片
                            // 而不是增量重渲染；高光扩展解码不接受覆盖，控件随之禁用。
                            // 对件没有自己的解码（两孔各随源照片），行随对禁用。
                            readonly property bool wbEditable: preview.hasFrame && preview.decodeMode === 0 && !window.pairActive
                            readonly property bool wbAsShot: preview.wbTemperature <= 0
                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: Localization.epoch && Localization.L("Temperature"); color: cameraSection.wbEditable ? Theme.text : Theme.muted; font.pixelSize: 11 }
                                Item { Layout.fillWidth: true }
                                Text { text: cameraSection.wbAsShot ? Localization.epoch && Localization.L("Auto (As Shot)") : Math.round(preview.wbTemperature) + " K"; color: Theme.muted; font.pixelSize: 10 }
                                ActionButton { text: "As Shot"; implicitHeight: 23; font.pixelSize: 10; enabled: preview.settingsEditable && cameraSection.wbEditable && preview.wbAsShotAvailable; onClicked: preview.applyWbAsShot(); ToolTip.visible: hovered; ToolTip.text: Localization.epoch && Localization.L("Backfills the as-shot white balance the photo's decode reported") }
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
                                Text { text: Localization.epoch && Localization.L("Tint"); color: cameraSection.wbEditable ? Theme.text : Theme.muted; font.pixelSize: 11 }
                                Item { Layout.fillWidth: true }
                                Text { text: cameraSection.wbAsShot ? Localization.epoch && Localization.L("Auto (As Shot)") : (preview.wbTint > 0 ? "+" : "") + Math.round(preview.wbTint); color: Theme.muted; font.pixelSize: 10 }
                                ActionButton { text: "As Shot"; implicitHeight: 23; font.pixelSize: 10; enabled: preview.settingsEditable && cameraSection.wbEditable && preview.wbAsShotAvailable; onClicked: preview.applyWbAsShot(); ToolTip.visible: hovered; ToolTip.text: Localization.epoch && Localization.L("Backfills the as-shot white balance the photo's decode reported") }
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
                                Text { text: Localization.epoch && Localization.L("White Balance"); color: cameraSection.wbEditable ? Theme.text : Theme.muted; font.pixelSize: 11 }
                                ComboBox {
                                    objectName: "wbPresetCombo"
                                    Layout.fillWidth: true
                                    model: Localization.epoch && [Localization.L("Auto (As Shot)"), Localization.L("Daylight 5500K"), Localization.L("Cloudy 6500K"), Localization.L("Shade 8000K"), Localization.L("Tungsten 3200K"), Localization.L("Fluorescent 4000K"), Localization.L("Flash 5500K")]
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
                                    displayText: currentIndex < 0 ? Localization.epoch && Localization.L("Custom") : currentText
                                    onActivated: presets[currentIndex] === "" ? preview.resetWb() : preview.applyWbPreset(presets[currentIndex])
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Switch {
                                    id: meterSwitch
                                    text: Localization.epoch && Localization.L("Meter"); font.pixelSize: 11
                                    palette.windowText: Theme.text
                                    // pairWireOf forces the meter off on a pair
                                    // (each hole's exposure rides its own
                                    // pixels), so the shared switch stands down.
                                    checked: preview.autoExposure
                                    enabled: preview.settingsEditable && !window.pairActive
                                    onToggled: preview.autoExposure = checked
                                }
                                Item { Layout.fillWidth: true }
                                Text { text: meterSwitch.checked ? Localization.epoch && Localization.L("Centre-weighted average") : Localization.epoch && Localization.L("Off (the scene is placed without metering)"); color: Theme.muted; font.pixelSize: 10 }
                            }
                            Note { text: Localization.epoch && Localization.L("White balance acts on the RAW decode and a change re-decodes the photo; As Shot backfills the as-shot white balance the decode reported. With the meter off, the film exposure places the scene directly.") }
                        }
                        Section { visible: !window.editMode;
                            title: Localization.epoch && Localization.L("Film format & effects")
                            RowLayout {
                                Text { text: Localization.epoch && Localization.L("Format long edge"); color: Theme.text; font.pixelSize: 11 }
                                ComboBox {
                                    Layout.fillWidth: true
                                    model: Localization.epoch && ["18 mm", "24 mm", Localization.L("35 mm (default)"), "36 mm（135）", "56 mm", "70 mm", "90 mm"]
                                    readonly property var lengths: [18, 24, 35, 36, 56, 70, 90]
                                    readonly property real actualLength: Number(preview.adjustments.filmFormatMm === undefined ? 35 : preview.adjustments.filmFormatMm)
                                    currentIndex: lengths.indexOf(actualLength)
                                    displayText: currentIndex < 0 ? Localization.epoch && Localization.L("%@ mm (custom)", [actualLength.toFixed(1)]) : currentText
                                    // pairWireOf derives a pair's format from
                                    // the piece itself (36 mm + the gap), so
                                    // the shared row stands down rather than
                                    // edit a value the wire overrides.
                                    enabled: preview.settingsEditable && !window.pairActive
                                    onActivated: preview.setAdjustment("filmFormatMm", lengths[currentIndex])
                                }
                            }
                            Note { visible: window.pairActive; text: Localization.epoch && Localization.L("Half-frame pair · the format's long edge is decided by the whole piece (36 mm + the gap); it is not set separately."); color: Theme.accent }
                            Note { text: Localization.epoch && Localization.L("The physical format sets the grain and halation scale; cropping does not change the format.") }
                            Effect { label: Localization.epoch && Localization.L("Grain"); activeKey: "grainActive"; amountKey: "grainAmount" }
                            Effect { label: Localization.epoch && Localization.L("Halation"); activeKey: "halationActive"; amountKey: "halationAmount"; maximum: 4 }
                            Effect { label: Localization.epoch && Localization.L("Glare"); activeKey: "glareActive"; amountKey: "glareAmount"; maximum: 30 }
                        }
                        // 数字中间片 (RFC-028): replaces the paper with a Cineon
                        // log rendering of the film's own negative. Engine-side
                        // settings like the meter — setDi goes through changed()
                        // and re-renders the session; negative film only.
                        Section {
                            id: diSection
                            title: Localization.epoch && Localization.L("Digital Intermediate")
                            readonly property bool diActive: preview.di.active === undefined ? false : preview.di.active
                            readonly property bool diAvailable: preview.settingsEditable && !preview.filmIsPositive
                            RowLayout {
                                Layout.fillWidth: true
                                Switch {
                                    objectName: "diActiveSwitch"
                                    text: Localization.epoch && Localization.L("Digital Intermediate"); font.pixelSize: 11
                                    palette.windowText: Theme.text
                                    checked: diSection.diActive
                                    enabled: diSection.diAvailable
                                    onToggled: preview.setDi("active", checked)
                                }
                                Item { Layout.fillWidth: true }
                                Text { text: diSection.diActive ? Localization.epoch && Localization.L("Cineon log") : Localization.epoch && Localization.L("Paper print"); color: Theme.muted; font.pixelSize: 10 }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Switch {
                                    objectName: "diBlueCompensationSwitch"
                                    text: Localization.epoch && Localization.L("Blue compensation"); font.pixelSize: 11
                                    palette.windowText: Theme.text
                                    checked: preview.di.blueCompensation === undefined ? false : preview.di.blueCompensation
                                    enabled: diSection.diAvailable && diSection.diActive
                                    onToggled: preview.setDi("blueCompensation", checked)
                                }
                                Item { Layout.fillWidth: true }
                                Text { text: Localization.epoch && Localization.L("Corrects the color cast in the blues"); color: Theme.muted; font.pixelSize: 10 }
                            }
                            Note { visible: preview.filmIsPositive; text: Localization.epoch && Localization.L("Slide film is scanned directly — there is no negative, so the digital intermediate does not apply."); color: Theme.accent }
                            Note { text: Localization.epoch && Localization.L("The digital intermediate renders the negative's positive image in Cineon log encoding, replacing the paper print; blue compensation rotates the hue inside the film-fitted blue window.") }
                        }
                        Section { visible: !window.editMode;
                            id: rawSection
                            title: Localization.epoch && Localization.L("RAW decode")
                            // A pair opens no RAW of its own: each hole reads
                            // its source photo's own decode, so the shared
                            // choice stands down on a pair item.
                            ComboBox { id: decode; Layout.fillWidth: true; model: Localization.epoch && [Localization.L("Compatible (default)"), Localization.L("Highlight extension (experimental)")]; currentIndex: preview.decodeMode; enabled: preview.settingsEditable && !window.pairActive; onActivated: preview.decodeMode = currentIndex }
                            Note { visible: window.pairActive; text: Localization.epoch && Localization.L("Half-frame pair · the decode follows each hole's source photo; it is not set on the piece."); color: Theme.accent }
                            Note { text: preview.decodeMode ? Localization.epoch && Localization.L("Keeps some of the highlights and negative values after demosaicing; the demosaic stays integer.") : Localization.epoch && Localization.L("Matches the proven native RAW pipeline.") }
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
                                    text: Localization.epoch && Localization.L("HDR scan"); font.pixelSize: 11
                                    palette.windowText: Theme.text
                                    checked: rawSection.hdrOn
                                    enabled: rawSection.hdrAvailable
                                    onToggled: preview.setScanHdr("on", checked)
                                }
                                Item { Layout.fillWidth: true }
                                Text {
                                    text: !rawSection.hdrOn ? Localization.epoch && Localization.L("Close")
                                        : Number(preview.scanHdr.headroom).toFixed(1) + "×"
                                    color: Theme.muted; font.pixelSize: 10
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: Localization.epoch && Localization.L("Headroom"); color: rawSection.hdrOn && rawSection.hdrAvailable ? Theme.text : Theme.muted; font.pixelSize: 11 }
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
                            Note { visible: preview.hasFrame && !preview.filmIsPositive; text: Localization.epoch && Localization.L("HDR scan applies only to slide film scanned directly; negatives go to the paper print and cannot use it."); color: Theme.accent }
                            Note { text: Localization.epoch && Localization.L("HDR scan pushes each of the slide's layer shoulders past their knees and lifts them inside the spectral scan, so the clear base lands on today's scan headroom multiple; it is an engine parameter and re-renders without re-decoding.") }
                        }
                        // 胶片边缘 (RFC-032, phase 2 port): the film outside
                        // the frame as real exposure — the gate, the rebate,
                        // the perforations, the maker's edge print. Engine-side
                        // settings like the HDR scan: setOverscan goes through
                        // changed() and re-renders the session (开 is a
                        // shoot-layer field, so its edits re-develop the film
                        // and the picture's size becomes the film canvas's).
                        Section { visible: !window.editMode;
                            id: overscanSection
                            title: Localization.epoch && Localization.L("Film rebate")
                            readonly property bool osOn: preview.overscan.on === undefined ? false : preview.overscan.on
                            readonly property bool osEditable: preview.settingsEditable
                            readonly property var formats: [
                                {"id": "135", "key": "135 full (36×24)"},
                                {"id": "135_half", "key": "135 half (18×24)"},
                                {"id": "120_645", "key": "120 6×4.5"},
                                {"id": "120_6x6", "key": "120 6×6"},
                                {"id": "120_6x7", "key": "120 6×7"},
                                {"id": "120_6x8", "key": "120 6×8"},
                                {"id": "120_6x9", "key": "120 6×9"},
                                {"id": "135_xpan", "key": "135 wide XPan (65×24)"},
                                {"id": "120_6x12", "key": "120 6×12"},
                                {"id": "120_6x17", "key": "120 6×17"}
                            ]
                            readonly property var gates: ["auto", "square", "rounded", "eared", "shouldered", "kicked"]
                            readonly property var modes: ["strip", "filed"]
                            RowLayout {
                                Layout.fillWidth: true
                                Switch {
                                    objectName: "overscanSwitch"
                                    text: Localization.epoch && Localization.L("Film rebate"); font.pixelSize: 11
                                    palette.windowText: Theme.text
                                    checked: overscanSection.osOn
                                    enabled: overscanSection.osEditable
                                    onToggled: preview.setOverscan("on", checked)
                                }
                                Item { Layout.fillWidth: true }
                                Text {
                                    text: !overscanSection.osOn ? Localization.epoch && Localization.L("Close") : (preview.overscan.format === undefined ? "" : preview.overscan.format)
                                    color: Theme.muted; font.pixelSize: 10
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: Localization.epoch && Localization.L("Mode"); color: overscanSection.osEditable && overscanSection.osOn ? Theme.text : Theme.muted; font.pixelSize: 11 }
                                ComboBox {
                                    objectName: "overscanModeCombo"
                                    Layout.fillWidth: true
                                    model: Localization.epoch && [Localization.L("Strip (the full film width)"), Localization.L("Single frame (little rebate)")]
                                    readonly property var values: overscanSection.modes
                                    enabled: overscanSection.osEditable && overscanSection.osOn
                                    currentIndex: values.indexOf(preview.overscan.mode === undefined ? "strip" : preview.overscan.mode)
                                    onActivated: preview.setOverscan("mode", values[currentIndex])
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: Localization.epoch && Localization.L("Frame"); color: overscanSection.osEditable && overscanSection.osOn ? Theme.text : Theme.muted; font.pixelSize: 11 }
                                ComboBox {
                                    objectName: "overscanFormatCombo"
                                    Layout.fillWidth: true
                                    model: Localization.epoch && overscanSection.formats.map(function(f) { return Localization.L(f.key) })
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
                                Text { text: Localization.epoch && Localization.L("Gate"); color: overscanSection.osEditable && overscanSection.osOn ? Theme.text : Theme.muted; font.pixelSize: 11 }
                                ComboBox {
                                    objectName: "overscanGateCombo"
                                    Layout.fillWidth: true
                                    model: Localization.epoch && [Localization.L("Auto (real cameras)"), Localization.L("Square"), Localization.L("Rounded"), Localization.L("Eared"), Localization.L("Shouldered"), Localization.L("Kicked corner")]
                                    readonly property var values: overscanSection.gates
                                    enabled: overscanSection.osEditable && overscanSection.osOn
                                    currentIndex: values.indexOf(preview.overscan.gate === undefined ? "auto" : preview.overscan.gate)
                                    onActivated: preview.setOverscan("gate", values[currentIndex])
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: Localization.epoch && Localization.L("Holes"); color: overscanSection.osEditable && overscanSection.osOn ? Theme.text : Theme.muted; font.pixelSize: 11 }
                                ComboBox {
                                    objectName: "overscanHolesCombo"
                                    Layout.fillWidth: true
                                    model: Localization.epoch && [Localization.L("White holes (scan light through)"), Localization.L("Black holes (black backing)")]
                                    enabled: overscanSection.osEditable && overscanSection.osOn
                                    currentIndex: preview.overscan.holes === "black" ? 1 : 0
                                    onActivated: preview.setOverscan("holes", currentIndex === 1 ? "black" : "white")
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: Localization.epoch && Localization.L("Carrier"); color: overscanSection.osEditable && overscanSection.osOn ? Theme.text : Theme.muted; font.pixelSize: 11 }
                                ComboBox {
                                    objectName: "overscanCarrierCombo"
                                    Layout.fillWidth: true
                                    model: Localization.epoch && [Localization.L("None (canvas ends at the film's edge)"), Localization.L("Black carrier"), Localization.L("Open (scan bright)")]
                                    enabled: overscanSection.osEditable && overscanSection.osOn
                                    currentIndex: preview.overscan.carrier === "black" ? 1 : preview.overscan.carrier === "open" ? 2 : 0
                                    onActivated: preview.setOverscan("carrier", ["none", "black", "open"][currentIndex])
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: Localization.epoch && Localization.L("Edge print"); color: overscanSection.osEditable && overscanSection.osOn ? Theme.text : Theme.muted; font.pixelSize: 11 }
                                TextField {
                                    objectName: "overscanEdgeTextInput"
                                    Layout.fillWidth: true
                                    placeholderText: Localization.epoch && Localization.L("Edge print (e.g. KODAK PORTRA 400)")
                                    text: preview.overscan.edgeText === undefined ? "" : preview.overscan.edgeText
                                    enabled: overscanSection.osEditable && overscanSection.osOn
                                    font.pixelSize: 11
                                    onEditingFinished: if (text !== (preview.overscan.edgeText === undefined ? "" : preview.overscan.edgeText)) preview.setOverscan("edgeText", text)
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: Localization.epoch && Localization.L("Frame no."); color: overscanSection.osEditable && overscanSection.osOn ? Theme.text : Theme.muted; font.pixelSize: 11 }
                                Item { Layout.fillWidth: true }
                                Text {
                                    text: Number(preview.overscan.frameNumber === undefined ? 0 : preview.overscan.frameNumber) === 0
                                          ? Localization.epoch && Localization.L("Drawn from the seeds") : String(preview.overscan.frameNumber)
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
                                Text { text: Localization.epoch && Localization.L("Camera seed"); color: overscanSection.osEditable && overscanSection.osOn ? Theme.text : Theme.muted; font.pixelSize: 11 }
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
                                Text { text: Localization.epoch && Localization.L("Frame seed"); color: overscanSection.osEditable && overscanSection.osOn ? Theme.text : Theme.muted; font.pixelSize: 11 }
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
                            OverscanSlider { label: Localization.epoch && Localization.L("Edge fog"); name: "fog"; from: 0; to: 4; step: 0.05 }
                            OverscanSlider { label: Localization.epoch && Localization.L("Light leaks"); name: "leaks"; from: 0; to: 4; step: 0.05 }
                            Note {
                                visible: window.pairActive
                                text: Localization.epoch && Localization.L("Half-frame pair · the switch mounts and unmounts the piece on its strip; the format is fixed to a 135 half strip (the gap follows the Half-frame pair panel's spacing), while the gate, perforations, edge print and fog still act on the whole piece.")
                                color: Theme.accent
                            }
                            Note { text: Localization.epoch && Localization.L("The film rebate draws the film outside the picture as real exposure: gate shadow, film base, perforations, edge print, edge fog and light leaks develop together; the switch and choices are engine parameters and re-render without re-decoding. The format must match the photo's shape (crop by the gate first).") }
                        }
                        // 日期背印 (RFC-031, phase 2 port): the date exposed
                        // onto the negative from behind — with or without the
                        // film edge (画面内 placement works on the bare
                        // frame). lcd/dots are the engine's own faces; the
                        // data face prints the shooting data beside the date.
                        Section { visible: !window.editMode;
                            id: dateSection
                            title: Localization.epoch && Localization.L("Date back")
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
                                text: Localization.epoch && Localization.L("Half-frame pair · the date switch and text are set per hole (see the Half-frame pair panel); the typeface, placement and size still act on the whole piece.")
                                color: Theme.accent
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Switch {
                                    objectName: "dateImprintSwitch"
                                    text: Localization.epoch && Localization.L("Date back"); font.pixelSize: 11
                                    palette.windowText: Theme.text
                                    checked: dateSection.dtOn
                                    enabled: dateSection.dtItemEditable
                                    onToggled: preview.setDateImprint("on", checked)
                                }
                                Item { Layout.fillWidth: true }
                                Text {
                                    text: !dateSection.dtLive ? Localization.epoch && Localization.L("Close") : (preview.dateImprint.style === undefined ? "" : preview.dateImprint.style)
                                    color: Theme.muted; font.pixelSize: 10
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: Localization.epoch && Localization.L("Date"); color: dateSection.dtItemEditable && dateSection.dtLive ? Theme.text : Theme.muted; font.pixelSize: 11 }
                                TextField {
                                    objectName: "dateImprintTextInput"
                                    Layout.fillWidth: true
                                    placeholderText: Localization.epoch && Localization.L("e.g. '26 11 15")
                                    text: preview.dateImprint.text === undefined ? "" : preview.dateImprint.text
                                    enabled: dateSection.dtItemEditable && dateSection.dtLive
                                    font.pixelSize: 11
                                    onEditingFinished: if (text !== (preview.dateImprint.text === undefined ? "" : preview.dateImprint.text)) preview.setDateImprint("text", text)
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: Localization.epoch && Localization.L("Data"); color: dateSection.dtEditable && dateSection.dtLive ? Theme.text : Theme.muted; font.pixelSize: 11 }
                                TextField {
                                    objectName: "dateImprintDataTextInput"
                                    Layout.fillWidth: true
                                    placeholderText: Localization.epoch && Localization.L("Shooting data (e.g. 1/125 F2.8 A 50mm)")
                                    text: preview.dateImprint.dataText === undefined ? "" : preview.dateImprint.dataText
                                    enabled: dateSection.dtEditable && dateSection.dtLive
                                    font.pixelSize: 11
                                    onEditingFinished: if (text !== (preview.dateImprint.dataText === undefined ? "" : preview.dateImprint.dataText)) preview.setDateImprint("dataText", text)
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: Localization.epoch && Localization.L("Typeface"); color: dateSection.dtEditable && dateSection.dtLive ? Theme.text : Theme.muted; font.pixelSize: 11 }
                                ActionButton {
                                    objectName: "dateStyleLcd"
                                    text: "LCD"; accented: preview.dateImprint.style === "lcd"
                                    enabled: dateSection.dtEditable && dateSection.dtLive
                                    onClicked: preview.setDateImprint("style", "lcd")
                                }
                                ActionButton {
                                    objectName: "dateStyleDots"
                                    text: Localization.epoch && Localization.L("Dots"); accented: preview.dateImprint.style === "dots"
                                    enabled: dateSection.dtEditable && dateSection.dtLive
                                    onClicked: preview.setDateImprint("style", "dots")
                                }
                                ActionButton {
                                    objectName: "dateStyleData"
                                    text: Localization.epoch && Localization.L("Data"); accented: preview.dateImprint.style === "data"
                                    enabled: dateSection.dtEditable && dateSection.dtLive
                                    onClicked: preview.setDateImprint("style", "data")
                                }
                                Item { Layout.fillWidth: true }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: Localization.epoch && Localization.L("Where"); color: dateSection.dtEditable && dateSection.dtLive ? Theme.text : Theme.muted; font.pixelSize: 11 }
                                ComboBox {
                                    objectName: "datePlacementCombo"
                                    Layout.fillWidth: true
                                    model: Localization.epoch && [Localization.L("In the picture (corner)"), Localization.L("Rebate (between frames)")]
                                    enabled: dateSection.dtEditable && dateSection.dtLive
                                    currentIndex: preview.dateImprint.placement === "rebate" ? 1 : 0
                                    onActivated: preview.setDateImprint("placement", currentIndex === 1 ? "rebate" : "frame")
                                }
                                Text { text: Localization.epoch && Localization.L("Corner"); color: dateSection.dtEditable && dateSection.dtLive ? Theme.text : Theme.muted; font.pixelSize: 11 }
                                ComboBox {
                                    objectName: "dateCornerCombo"
                                    Layout.preferredWidth: 88
                                    model: Localization.epoch && [Localization.L("bottom right"), Localization.L("bottom left"), Localization.L("top right"), Localization.L("top left")]
                                    readonly property var values: ["br", "bl", "tr", "tl"]
                                    enabled: dateSection.dtEditable && dateSection.dtLive
                                    currentIndex: values.indexOf(preview.dateImprint.corner === undefined ? "br" : preview.dateImprint.corner)
                                    onActivated: preview.setDateImprint("corner", values[currentIndex])
                                }
                            }
                            DateSlider { label: Localization.epoch && Localization.L("Exposure"); name: "ev"; from: -2; to: 8; step: 0.1; suffix: " EV" }
                            DateSlider { label: Localization.epoch && Localization.L("Size"); name: "size"; from: 0.4; to: 3; step: 0.05; decimals: 2; suffix: "×" }
                            DateSlider { label: Localization.epoch && Localization.L("Horizontal inset"); name: "insetX"; from: 0; to: 30; step: 0.5; suffix: " mm" }
                            DateSlider { label: Localization.epoch && Localization.L("Vertical inset"); name: "insetY"; from: 0; to: 30; step: 0.5; suffix: " mm" }
                            Note { text: Localization.epoch && Localization.L("The date back exposes the date and the shooting data through the film's back: use it with the film rebate, or print it alone in the picture's corner; the Rebate placement prints the date between frames and needs the film rebate on.") }
                        }
                        Section {
                            title: Localization.epoch && Localization.L("Photo info")
                            Note { text: preview.hasFrame ? preview.metadata.camera : Localization.epoch && Localization.L("No photo is open"); color: Theme.text }
                            Note { text: preview.hasFrame ? Localization.epoch && Localization.L("%@ × %@ px\n%@", [preview.metadata.width, preview.metadata.height, preview.metadata.mode === "headroom" ? Localization.L("Highlight-extension decode") : Localization.L("Compatible decode")]) : "" }
                            Note { text: preview.hasFrame ? Localization.epoch && Localization.L("This render took %@ s · %@", [(preview.metadata.renderMs / 1000).toFixed(2), preview.metadata.cached ? Localization.L("Negative reused") : Localization.L("Full render")]) : "" }
                            ActionButton { Layout.fillWidth: true; text: Localization.epoch && Localization.L("Reset this photo's settings"); enabled: preview.settingsEditable; onClicked: preview.resetSettings() }
                        }
                        Section {
                            title: preview.batchExporting ? Localization.epoch && Localization.L("Batch export") : preview.failed ? Localization.epoch && Localization.L("The operation cannot be completed") : Localization.epoch && Localization.L("Status")
                            Note { text: window.closePending ? Localization.epoch && Localization.L("Waiting for the current operation to finish before closing…") : preview.status; color: preview.failed ? "#ffb39f" : Theme.muted }
                            Note { objectName: "savedStatusNote"; text: preview.savedStatus; visible: text.length > 0 }
                            Note { objectName: "persistenceWarningNote"; text: preview.persistenceWarning; visible: text.length > 0; color: "#ffb39f" }
                            ProgressBar { Layout.fillWidth: true; visible: preview.batchExporting; from: 0; to: Math.max(1, preview.batchTotal); value: preview.batchProgress }
                            ActionButton { Layout.fillWidth: true; text: Localization.epoch && Localization.L("Stop after the current photo"); visible: preview.batchExporting; onClicked: preview.cancelExport() }
                            ActionButton { Layout.fillWidth: true; text: Localization.epoch && Localization.L("Retry the preview"); visible: preview.failed && preview.hasFrame && !preview.batchExporting; enabled: !preview.busy; onClicked: preview.apply() }
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
            Text { text: Localization.epoch && Localization.L("Export photos"); color: Theme.text; font.pixelSize: 20; font.bold: true }
            Note { text: Localization.epoch && Localization.L("The current frame or the selected photos · %@\nThe crop and effects are written into the file.", [Localization.L(window.exportColourNames[window.exportColour])]) }
            Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.line; opacity: 0.5 }
            RowLayout {
                Text { text: Localization.epoch && Localization.L("Format"); color: Theme.text; Layout.preferredWidth: 76 }
                ComboBox { Layout.fillWidth: true; model: Localization.epoch && [Localization.L("TIFF · 16-bit"), Localization.L("PNG · 8-bit"), Localization.L("PNG · 16-bit"), Localization.L("JPEG · 8-bit")]; currentIndex: window.exportFormat; onActivated: window.exportFormat = currentIndex }
            }
            RowLayout {
                Text { text: Localization.epoch && Localization.L("Colour"); color: Theme.text; Layout.preferredWidth: 76 }
                ComboBox {
                    Layout.fillWidth: true
                    model: Localization.epoch && window.exportColourNames.map(function (name) { return Localization.L(name) })
                    currentIndex: window.exportColour
                    onActivated: window.exportColour = currentIndex
                }
            }
            RowLayout {
                visible: window.exportFormat === 3
                Text { text: Localization.epoch && Localization.L("JPEG quality"); color: Theme.text; Layout.preferredWidth: 76 }
                Slider { Layout.fillWidth: true; from: 1; to: 100; stepSize: 1; value: window.exportQuality; onMoved: window.exportQuality = value }
                Text { text: window.exportQuality; color: Theme.muted; Layout.preferredWidth: 28 }
            }
            RowLayout {
                Text { text: Localization.epoch && Localization.L("Output size"); color: Theme.text; Layout.preferredWidth: 76 }
                ComboBox {
                    Layout.fillWidth: true; model: Localization.epoch && [Localization.L("Original crop size"), Localization.L("Long edge 3840 px"), Localization.L("Long edge 2560 px"), Localization.L("Long edge 1920 px"), Localization.L("Long edge 1080 px")]
                    readonly property var sizes: [0, 3840, 2560, 1920, 1080]
                    currentIndex: Math.max(0, sizes.indexOf(window.exportLongEdge))
                    onActivated: window.exportLongEdge = sizes[currentIndex]
                }
            }
            Note { text: Localization.epoch && Localization.L("The aspect ratio is kept and images are never enlarged. PNG saves losslessly; JPEG compresses with loss.") }
            Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.line; opacity: 0.5 }
            ActionButton { Layout.fillWidth: true; accented: true; text: Localization.epoch && Localization.L("Export the displayed frame…"); enabled: preview.hasFrame && !preview.busy; onClicked: { saveDialog.open(); exportPopup.close() } }
            ActionButton { Layout.fillWidth: true; text: Localization.epoch && Localization.L("Export %@ selected to folder…", [preview.selectedCount]); enabled: preview.selectedCount > 0 && !preview.busy; onClicked: { batchDialog.open(); exportPopup.close() } }
            Note { text: Localization.epoch && Localization.L("Exporting the current frame never overwrites an existing file. Batch export goes photo by photo and skips files that already exist; photos never opened still render by their own settings.") }
            ActionButton { Layout.alignment: Qt.AlignRight; text: Localization.epoch && Localization.L("Cancel"); onClicked: exportPopup.close() }
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
            Text { text: Localization.epoch && Localization.L("Copy settings"); color: Theme.text; font.pixelSize: 16; font.bold: true }
            Note { text: Localization.epoch && Localization.L("Tick the groups to keep with the copy; pasting writes only those settings — the target photos are placed again by their own metering and keep their own As Shot white balance, crop and decode mode.") }
            Repeater {
                model: library.offeredClipboardGroups()
                delegate: CheckBox {
                    required property string modelData
                    Layout.fillWidth: true
                    text: Localization.epoch && window.groupName(modelData)
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
                    text: Localization.epoch && Localization.L("Copy %@ selected", [library.selectedCount])
                    enabled: copySheet.ticks.length > 0 && library.selectedCount > 0
                    onClicked: {
                        library.setClipboardGroups(copySheet.ticks)
                        library.copySettings()
                        copySheet.close()
                    }
                }
                ActionButton { text: Localization.epoch && Localization.L("Cancel"); onClicked: copySheet.close() }
            }
        }
    }
}
}
