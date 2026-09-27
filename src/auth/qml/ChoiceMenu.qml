import QtQuick
import Cutefish.Auth

// One of a few names, the account or the session to log in with: the current
// choice as a button, and the others in a list while it is open.
Item {
    id: root

    property var names: []
    property int currentIndex: -1
    property bool open: false
    // The panel is an image of a fixed size; a list that would run past its
    // bottom opens upward instead of being cut off.
    property bool upward: false

    signal activated(int index)

    // Every entry as wide as the widest name, so their highlights line up.
    readonly property real entryWidth: {
        let widest = 0
        for (let i = 0; i < names.length; ++i)
            widest = Math.max(widest, metrics.advanceWidth(names[i]))
        return Math.max(160, Math.ceil(widest) + 24)
    }

    implicitWidth: Math.max(button.implicitWidth, 120)
    implicitHeight: 30

    onEnabledChanged: {
        if (!enabled)
            open = false
    }

    FontMetrics {
        id: metrics

        font.pixelSize: 13
        font.weight: Font.DemiBold
    }

    // A press anywhere else closes the list and still reaches what it pressed:
    // a passive PointHandler over the whole panel never takes the press.
    Item {
        parent: root.Window.window ? root.Window.window.contentItem : root
        anchors.fill: parent
        z: 1000

        PointHandler {
            enabled: root.open

            onActiveChanged: {
                if (!active)
                    return
                const onButton = button.contains(button.mapFromItem(null, point.scenePressPosition))
                const onList = popup.contains(popup.mapFromItem(null, point.scenePressPosition))
                if (!onButton && !onList)
                    root.open = false
            }
        }
    }

    RoundedRectangle {
        id: button

        anchors.centerIn: parent
        implicitWidth: current.implicitWidth + 28
        width: implicitWidth
        height: parent.height
        radius: height / 2
        color: Qt.rgba(1, 1, 1, press.containsMouse || root.open ? 0.24 : 0.16)

        Text {
            id: current

            anchors.centerIn: parent
            text: root.currentIndex >= 0 && root.currentIndex < root.names.length
                  ? root.names[root.currentIndex] : ""
            color: "white"
            font.pixelSize: 13
            renderType: Text.NativeRendering
        }

        MouseArea {
            id: press

            anchors.fill: parent
            hoverEnabled: true
            enabled: root.enabled
            onClicked: {
                if (!root.open) {
                    const bottom = root.mapToItem(null, 0, root.height).y
                    root.upward = root.Window.window
                            && bottom + popup.height + 6 > root.Window.window.height
                }
                root.open = !root.open
            }
        }
    }

    RoundedRectangle {
        id: popup

        y: root.upward ? -height - 6 : root.height + 6
        anchors.horizontalCenter: parent.horizontalCenter
        width: list.width + 12
        height: list.height + 12
        radius: 12
        color: Qt.rgba(0, 0, 0, 0.55)
        visible: root.open

        Column {
            id: list

            anchors.centerIn: parent

            Repeater {
                model: root.names

                delegate: RoundedRectangle {
                    id: entry

                    required property int index
                    required property string modelData

                    width: root.entryWidth
                    height: 30
                    radius: 8
                    color: choose.containsMouse ? Qt.rgba(1, 1, 1, 0.16) : "transparent"

                    Text {
                        id: label

                        anchors.verticalCenter: parent.verticalCenter
                        x: 12
                        text: entry.modelData
                        color: "white"
                        font.pixelSize: 13
                        font.weight: entry.index === root.currentIndex ? Font.DemiBold : Font.Normal
                        renderType: Text.NativeRendering
                    }

                    MouseArea {
                        id: choose

                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
                            root.open = false
                            root.activated(entry.index)
                        }
                    }
                }
            }
        }
    }
}
