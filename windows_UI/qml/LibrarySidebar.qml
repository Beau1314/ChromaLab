import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

// The library sidebar (task 4.4; MAC-LIBRARY-UI §3): three tabs — 库 (the
// source picker, then the collections outline with counts), 过滤 (the facet
// check rows with their cross-counts), 信息 (the selected frame's fields).
// The tree's outline row is 24 high with a 14-point indent per level and a
// dim monospaced count; the selected row wears the white capsule. The bottom
// line carries the model's state: busy, the last error, and the import's
// live progress (the worker seam's frames land mid-import).
Rectangle {
    id: sidebar
    color: Theme.card

    property int tabIndex: 0
    // The uuid the Info tab describes: the grid's current selection (its
    // first row, or the empty map that leaves the tab quiet).
    readonly property var infoUuid: library.selectedCount > 0 ? library.selectedUuids()[0] : ""
    readonly property var infoRow: infoUuid.length > 0 ? library.rowInfo(infoUuid) : ({})
    // MacColorLabel's seven (1 red .. 7 purple), for the colour-label dots.
    readonly property var labelColors: [,
        "#e1574e", "#e6954f", "#e3c14f", "#6bbd5f", "#5588d8", "#d967ad", "#9067c9"]

    // The helper's facet map is {facet: [{value, count}]}; 0 when absent.
    function facetCount(facet, value) {
        var list = library.facets[facet] === undefined ? [] : library.facets[facet]
        for (var i = 0; i < list.length; ++i)
            if (Number(list[i].value) === value) return Number(list[i].count)
        return 0
    }
    function facetTextCount(facet, value) {
        var list = library.facets[facet] === undefined ? [] : library.facets[facet]
        for (var i = 0; i < list.length; ++i)
            if (String(list[i].value) === value) return Number(list[i].count)
        return 0
    }
    // The outline's depth: walk the parent chain (the tree rows are flat).
    function depthOf(row) {
        var depth = 0
        var parent = row.parent
        while (parent !== undefined && parent.length > 0 && depth < 8) {
            for (var i = 0; i < library.collectionsTree.length; ++i)
                if (library.collectionsTree[i].id === parent) {
                    parent = library.collectionsTree[i].parent
                    break
                }
            ++depth
        }
        return depth
    }
    // The kind facet's stable vocabulary (the helper's kinds), shown even
    // when a kind has no rows yet.
    function kindValues() { return ["raw", "raster", "pair", "movie", "other"] }
    function kindLabel(kind) {
        if (kind === "raw") return "RAW"
        if (kind === "raster") return Localization.L("Bitmap")
        if (kind === "pair") return Localization.L("Half-frame pair")
        if (kind === "movie") return Localization.L("Movie")
        return Localization.L("Other")
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // The tab bar: 30 high, the chosen tab in text on the card, the
        // others dim (§4's navigator bar, small).
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 30
            color: Theme.wellDark
            RowLayout {
                anchors.fill: parent
                spacing: 0
                component NavTab: Item {
                    required property int tabIndex_
                    required property string title
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: 4
                        radius: 4
                        color: sidebar.tabIndex === tabIndex_ ? Theme.card : "transparent"
                    }
                    Text {
                        anchors.centerIn: parent
                        text: title
                        color: sidebar.tabIndex === tabIndex_ ? Theme.text : Theme.muted
                        font.pixelSize: 11
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: sidebar.tabIndex = tabIndex_
                    }
                }
                NavTab { tabIndex_: 0; title: Localization.epoch && Localization.L("Library") }
                NavTab { tabIndex_: 1; title: Localization.epoch && Localization.L("Filter") }
                NavTab { tabIndex_: 2; title: Localization.epoch && Localization.L("Info") }
            }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: sidebar.tabIndex

            // --- 库: the source picker, then the collections outline -----------
            ColumnLayout {
                spacing: 0
                ComboBox {
                    id: sourcePicker
                    objectName: "sourcePicker"
                    Layout.fillWidth: true
                    Layout.margins: 8
                    model: library.sources
                    textRole: "name"
                    currentIndex: {
                        for (var i = 0; i < library.sources.length; ++i)
                            if (library.sources[i].selected === true) return i
                        return -1
                    }
                    onActivated: library.selectSource(library.sources[currentIndex].id)
                    ToolTip.visible: hovered
                    ToolTip.text: library.selectedSource.path === undefined
                                  ? "" : String(library.selectedSource.path)
                }
                ListView {
                    id: tree
                    objectName: "collectionTree"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: library.collectionsTree
                    ScrollBar.vertical: ScrollBar {}
                    delegate: Item {
                        id: treeRow
                        required property var modelData
                        width: tree.width
                        height: 24
                        readonly property bool chosen:
                            modelData.id === library.selectedCollectionId
                        readonly property int depth: sidebar.depthOf(modelData)
                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: 3
                            radius: 5
                            color: treeRow.chosen ? Theme.selection : "transparent"
                        }
                        Text {
                            x: 12 + treeRow.depth * 14
                            anchors.verticalCenter: parent.verticalCenter
                            text: treeRow.modelData.kind === "folder" ? "▸" : "·"
                            color: treeRow.chosen ? Theme.selectionText : Theme.muted
                            font.pixelSize: 11
                        }
                        Text {
                            x: 30 + treeRow.depth * 14
                            width: parent.width - x - 44
                            anchors.verticalCenter: parent.verticalCenter
                            text: treeRow.modelData.title
                            color: treeRow.chosen ? Theme.selectionText : Theme.text
                            font.pixelSize: 12
                            elide: Text.ElideRight
                        }
                        Text {
                            anchors.right: parent.right
                            anchors.rightMargin: 14
                            anchors.verticalCenter: parent.verticalCenter
                            text: treeRow.modelData.imageCount === undefined
                                  ? "" : String(treeRow.modelData.imageCount)
                            color: treeRow.chosen ? Theme.selectionText : Theme.muted
                            font.pixelSize: 11
                        }
                        MouseArea {
                            anchors.fill: parent
                            onClicked: library.selectCollection(treeRow.modelData.id)
                        }
                    }
                }
            }

            // --- 过滤: the facet check rows, each with its cross-count ---------
            ColumnLayout {
                spacing: 0
                TextField {
                    id: searchField
                    objectName: "librarySearchField"
                    Layout.fillWidth: true
                    Layout.margins: 8
                    placeholderText: Localization.epoch && Localization.L("Filter photos")
                    font.pixelSize: 11
                    onTextChanged: searchDebounce.restart()
                }
                Timer {
                    id: searchDebounce
                    interval: 300
                    onTriggered: library.textFilter = searchField.text
                }
                ScrollView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    contentWidth: availableWidth
                    ColumnLayout {
                        width: parent.width
                        spacing: 0

                        component FacetHeader: Text {
                            required property string title
                            Layout.fillWidth: true
                            leftPadding: 12
                            topPadding: 10
                            bottomPadding: 2
                            text: title
                            color: Theme.muted
                            font.pixelSize: 10
                            font.bold: true
                        }
                        component FacetCheck: RowLayout {
                            required property string label
                            required property int count
                            required property bool on
                            required property var values
                            required property var value
                            property color swatch: "transparent"
                            Layout.fillWidth: true
                            Layout.leftMargin: 6
                            Layout.rightMargin: 6
                            spacing: 4
                            Rectangle {
                                Layout.preferredWidth: 10
                                Layout.preferredHeight: 10
                                radius: 5
                                color: swatch
                                border.color: Theme.line
                                border.width: swatch.a > 0 ? 0 : 1
                                visible: swatch.a > 0
                            }
                            Item { Layout.preferredWidth: swatch.a > 0 ? 0 : 10 }
                            CheckBox {
                                Layout.fillWidth: true
                                text: label + (count > 0 ? "   " + count : "")
                                font.pixelSize: 11
                                checked: on
                                onToggled: {
                                    var has = library[values].indexOf(value) >= 0
                                    var next = library[values].filter(function(v) { return v !== value })
                                    if (!has) next.push(value)
                                    library[values] = next
                                }
                            }
                        }

                        FacetHeader { title: Localization.epoch && Localization.L("Rating") }
                        FacetCheck {
                            objectName: "ratingFilter0"
                            label: Localization.epoch && Localization.L("No rating")
                            count: sidebar.facetCount("rating", 0)
                            on: library.ratingFilter.indexOf(0) >= 0
                            values: "ratingFilter"
                            value: 0
                        }
                        Repeater {
                            model: 5
                            delegate: FacetCheck {
                                required property int index
                                objectName: "ratingFilter" + (index + 1)
                                label: "★★★★★".slice(0, index + 1)
                                count: sidebar.facetCount("rating", index + 1)
                                on: library.ratingFilter.indexOf(index + 1) >= 0
                                values: "ratingFilter"
                                value: index + 1
                            }
                        }
                        FacetHeader { title: Localization.epoch && Localization.L("Colour Label") }
                        FacetCheck {
                            objectName: "colorFilter0"
                            label: Localization.epoch && Localization.L("No colour label")
                            count: sidebar.facetCount("color_tag", 0)
                            on: library.colorTagFilter.indexOf(0) >= 0
                            values: "colorTagFilter"
                            value: 0
                        }
                        Repeater {
                            model: 7
                            delegate: FacetCheck {
                                required property int index
                                objectName: "colorFilter" + (index + 1)
                                label: Localization.epoch && Localization.L("Colour label %@", [index + 1])
                                count: sidebar.facetCount("color_tag", index + 1)
                                on: library.colorTagFilter.indexOf(index + 1) >= 0
                                values: "colorTagFilter"
                                value: index + 1
                                swatch: sidebar.labelColors[index + 1]
                            }
                        }
                        FacetHeader { title: Localization.epoch && Localization.L("Kind") }
                        Repeater {
                            model: sidebar.kindValues()
                            delegate: FacetCheck {
                                required property string modelData
                                objectName: "kindFilter_" + modelData
                                label: Localization.epoch && sidebar.kindLabel(modelData)
                                count: sidebar.facetTextCount("kind", modelData)
                                on: library.kindFilter.indexOf(modelData) >= 0
                                values: "kindFilter"
                                value: modelData
                            }
                        }
                    }
                }
            }

            // --- 信息: the selected frame's label/value rows -------------------
            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                contentWidth: availableWidth
                ColumnLayout {
                    width: parent.width
                    spacing: 0
                    Text {
                        visible: sidebar.infoRow.fileName === undefined
                        Layout.fillWidth: true
                        Layout.margins: 12
                        // Same wrap semantics as Main.qml's Note: keep the
                        // layout's implicit width honest so the note wraps
                        // inside the sidebar instead of stretching it.
                        Layout.preferredWidth: 0
                        Layout.minimumWidth: 0
                        text: Localization.epoch && Localization.L("Select a photo in the grid to see its info")
                        color: Theme.muted
                        font.pixelSize: 11
                        wrapMode: Text.WordWrap
                    }
                    component InfoRow: RowLayout {
                        required property string label
                        required property string value
                        visible: value.length > 0
                        Layout.fillWidth: true
                        spacing: 6
                        Item { Layout.preferredWidth: 12 }
                        Text {
                            text: label
                            color: Theme.muted
                            font.pixelSize: 11
                            Layout.preferredWidth: 76
                            horizontalAlignment: Text.AlignRight
                        }
                        Text {
                            text: value
                            color: Theme.text
                            font.pixelSize: 11
                            Layout.fillWidth: true
                            elide: Text.ElideMiddle
                        }
                    }
                    InfoRow { label: Localization.epoch && Localization.L("File"); value: sidebar.infoRow.fileName === undefined ? "" : String(sidebar.infoRow.fileName) }
                    InfoRow { label: Localization.epoch && Localization.L("Kind"); value: sidebar.infoRow.kind === undefined ? "" : String(sidebar.infoRow.kind) }
                    InfoRow { label: Localization.epoch && Localization.L("Capture"); value: sidebar.infoRow.capturedLocal === undefined ? "" : String(sidebar.infoRow.capturedLocal) }
                    InfoRow {
                        label: Localization.epoch && Localization.L("Rating")
                        value: sidebar.infoRow.rating === undefined || Number(sidebar.infoRow.rating) <= 0
                               ? "" : "★★★★★".slice(0, Number(sidebar.infoRow.rating))
                    }
                    InfoRow {
                        label: Localization.epoch && Localization.L("Colour Label")
                        value: sidebar.infoRow.colorTag === undefined || Number(sidebar.infoRow.colorTag) <= 0
                               ? "" : Localization.epoch && Localization.L("Colour label %@", [Number(sidebar.infoRow.colorTag)])
                    }
                    InfoRow {
                        label: Localization.epoch && Localization.L("Develop")
                        value: sidebar.infoRow.develop === undefined ? ""
                               : sidebar.infoRow.develop.state === "developed"
                                   ? (String(sidebar.infoRow.develop.film) + " · " +
                                      String(sidebar.infoRow.develop.paper))
                                   : Localization.epoch && Localization.L("Not developed")
                    }
                    InfoRow { label: Localization.epoch && Localization.L("Path"); value: sidebar.infoRow.path === undefined ? "" : String(sidebar.infoRow.path) }
                }
            }
        }

        // The "+" bar (§3): the way to make a catalog, a session or a watched
        // folder — the empty grid's hint sends the user here, so it sits at
        // the bottom of the sidebar in every tab.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 32
            color: Theme.card
            Rectangle { width: parent.width; height: 1; color: Theme.line; opacity: 0.35; anchors.top: parent.top }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                spacing: 8
                ToolButton {
                    id: plusButton
                    objectName: "libraryPlusButton"
                    text: "+"
                    font.pixelSize: 16
                    implicitHeight: 24
                    implicitWidth: 28
                    onClicked: plusMenu.popup()
                    ToolTip.visible: hovered
                    ToolTip.text: Localization.epoch && Localization.L("New catalog, new session or a watched folder")
                }
                Text {
                    Layout.fillWidth: true
                    text: Localization.epoch && Localization.L("New catalog · session · folder")
                    color: Theme.muted; font.pixelSize: 10
                    MouseArea { anchors.fill: parent; onClicked: plusMenu.popup() }
                }
            }
            Menu {
                id: plusMenu
                MenuItem {
                    objectName: "plusNewCatalog"
                    text: Localization.epoch && Localization.L("New catalog…")
                    onTriggered: { nameDialog.kind = "catalog"; nameField.text = ""; nameDialog.open() }
                }
                MenuItem {
                    objectName: "plusNewSession"
                    text: Localization.epoch && Localization.L("New session…")
                    onTriggered: { nameDialog.kind = "session"; nameField.text = ""; nameDialog.open() }
                }
                MenuItem {
                    objectName: "plusAddFolder"
                    text: Localization.epoch && Localization.L("Add a watched folder…")
                    onTriggered: watchedFolderDialog.open()
                }
            }
            Popup {
                id: nameDialog
                property string kind: "catalog"
                width: 280
                padding: 14
                modal: true
                // Centre on the window: the popup's default anchors to the
                // "+" bar at the screen's bottom-left corner, where a
                // downward dialog runs off the screen.
                anchors.centerIn: Overlay.overlay
                closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
                background: Rectangle { color: Theme.card; border.color: Theme.line; radius: 6 }
                ColumnLayout {
                    width: parent.width
                    spacing: 10
                    Text {
                        text: nameDialog.kind === "catalog"
                              ? Localization.epoch && Localization.L("New catalog")
                              : Localization.epoch && Localization.L("New session")
                        color: Theme.text; font.pixelSize: 13; font.bold: true
                    }
                    TextField {
                        id: nameField
                        objectName: "libraryNameField"
                        Layout.fillWidth: true
                        font.pixelSize: 12
                        placeholderText: Localization.epoch && Localization.L("Name")
                        onAccepted: createButton.clicked()
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: nameField.text.trim().length === 0
                        text: Localization.epoch && Localization.L("The name cannot be empty")
                        color: "#ffb39f"; font.pixelSize: 10
                        wrapMode: Text.WordWrap
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Item { Layout.fillWidth: true }
                        ActionButton { text: Localization.epoch && Localization.L("Cancel"); implicitHeight: 25; onClicked: nameDialog.close() }
                        ActionButton {
                            id: createButton
                            objectName: "libraryCreateButton"
                            text: Localization.epoch && Localization.L("Create"); implicitHeight: 25
                            enabled: nameField.text.trim().length > 0
                            onClicked: {
                                var ok = nameDialog.kind === "catalog"
                                         ? library.createCatalog(nameField.text.trim())
                                         : library.createSession(nameField.text.trim())
                                if (ok) nameDialog.close()
                            }
                        }
                    }
                }
            }
            FolderDialog {
                id: watchedFolderDialog
                onAccepted: library.addFolder(selectedFolder)
            }
        }

        // The status line: the model's busy, its last error and the live
        // import progress (worker seam, task 4.4).
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 28
            color: Theme.wellDark
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                spacing: 6
                BusyIndicator {
                    visible: library.busy
                    running: library.busy
                    implicitWidth: 16
                    implicitHeight: 16
                }
                Text {
                    visible: !library.busy && Number(library.importProgress.total) > 0 &&
                             library.importProgress.phase !== undefined
                    text: Localization.epoch && Localization.L("Import %@ / %@", [Number(library.importProgress.done), Number(library.importProgress.total)])
                    color: Theme.accent
                    font.pixelSize: 10
                }
                Text {
                    Layout.fillWidth: true
                    text: library.lastError
                    color: "#ffb39f"
                    font.pixelSize: 10
                    elide: Text.ElideRight
                    visible: library.lastError.length > 0
                }
                Item { Layout.fillWidth: true; visible: library.lastError.length === 0 }
            }
        }
    }
}
