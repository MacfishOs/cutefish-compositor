import QtQuick
import Cutefish.Auth

Column {
    id: root

    property string displayName
    property string iconSource

    spacing: 12

    Item {
        id: avatar

        anchors.horizontalCenter: parent.horizontalCenter
        width: 96
        height: 96

        RoundedRectangle {
            id: disc

            anchors.fill: parent
            radius: width / 2
            color: Qt.rgba(1, 1, 1, 0.16)
            border.width: 1
            border.color: Qt.rgba(1, 1, 1, 0.28)

            // No clip: Item::clip is a rectangular scissor and would leave the
            // picture's corners showing. The provider crops it to a circle.
            Image {
                id: picture

                anchors.fill: parent
                anchors.margins: 1
                source: root.iconSource
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
                sourceSize.width: width * Screen.devicePixelRatio
                sourceSize.height: height * Screen.devicePixelRatio
            }

            // The account's initial, for an account with no picture - which is
            // most of them on a fresh install.
            Text {
                anchors.centerIn: parent
                visible: picture.status !== Image.Ready
                text: root.displayName.length > 0 ? root.displayName.charAt(0).toUpperCase() : "?"
                color: "white"
                font.pixelSize: 42
                font.weight: Font.Light
            }
        }
    }

    Text {
        anchors.horizontalCenter: parent.horizontalCenter
        text: root.displayName
        color: "white"
        font.pixelSize: 19
        font.weight: Font.DemiBold
        style: Text.Raised
        styleColor: Qt.rgba(0, 0, 0, 0.3)
        renderType: Text.NativeRendering
    }
}
