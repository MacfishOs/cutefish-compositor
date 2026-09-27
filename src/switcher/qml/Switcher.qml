import QtQuick

// The row of applications being switched through, most recently used first,
// with the selected one's name under its icon. The compositor draws the glass,
// the selection's included, and the edge; this is only what goes on them.
Item {
    id: root

    property var entries: []
    property int selectedIndex: -1
    property bool darkMode: false
    property real rowX: 0
    property real padding: 16
    // The row's top: centred, whatever snapping did to the panel's height.
    readonly property real rowY: (height - cellSize) / 2
    property real cellSize: 120
    property real iconSize: 96
    // The logical size icons are rendered at: cells shrink, the cached image
    // does not. Image asks for it at the output's density.
    property real iconSourceSize: 96

    // A count, not the list: switching between as many applications again
    // keeps every cell and only changes what it shows.
    Repeater {
        model: root.entries.length

        delegate: Image {
            required property int index

            x: root.rowX + index * root.cellSize + (root.cellSize - root.iconSize) / 2
            y: root.rowY + (root.cellSize - root.iconSize) / 2
            width: root.iconSize
            height: root.iconSize
            sourceSize: Qt.size(root.iconSourceSize, root.iconSourceSize)
            source: "image://appicon/" + encodeURIComponent(root.entries[index].icon)
            smooth: true
            mipmap: true
        }
    }

    // Only the selected application's name is shown, so only it is laid out.
    // Centred under its icon, kept inside the panel at the ends of the row.
    Text {
        readonly property real centre: root.rowX + (root.selectedIndex + 0.5) * root.cellSize
        x: Math.max(root.padding, Math.min(centre - width / 2, root.width - root.padding - width))
        // Its middle a little under half way down from the selection to the
        // panel's edge, scaled with the cells.
        y: root.rowY + (root.cellSize + root.iconSize) / 2 + root.cellSize * 10.5 / 108 - height / 2
        width: Math.min(implicitWidth, root.width - 2 * root.padding)
        text: visible ? root.entries[root.selectedIndex].name : ""
        elide: Text.ElideRight
        font.pixelSize: 11
        font.weight: Font.DemiBold
        renderType: Text.NativeRendering
        color: root.darkMode ? Qt.rgba(1, 1, 1, 0.92) : Qt.rgba(0, 0, 0, 0.85)
        visible: root.selectedIndex >= 0 && root.selectedIndex < root.entries.length
    }
}
