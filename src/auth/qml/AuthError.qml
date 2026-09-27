import QtQuick

Item {
    id: root

    property string message
    property bool shown

    implicitHeight: 20
    opacity: shown && message.length > 0 ? 1 : 0

    Behavior on opacity {
        NumberAnimation { duration: 180; easing.type: Easing.OutCubic }
    }

    Text {
        anchors.centerIn: parent
        width: parent.width
        text: root.message
        color: Qt.rgba(1, 0.72, 0.72, 1)
        font.pixelSize: 13
        horizontalAlignment: Text.AlignHCenter
        elide: Text.ElideRight
        style: Text.Raised
        styleColor: Qt.rgba(0, 0, 0, 0.35)
        renderType: Text.NativeRendering
    }
}
