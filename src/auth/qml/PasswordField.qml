
import QtQuick
import Cutefish.Auth

FocusScope {
    id: root

    property alias placeholder: prompt.text
    property alias text: input.text

    signal accepted(string password)
    signal edited()

    implicitHeight: 32

    function clear() {
        input.clear()
    }

    function shake() {
        shakeAnimation.restart()
    }

    RoundedRectangle {
        id: pill

        // The compositor blurs the wallpaper behind it.
        objectName: "passwordPill"

        anchors.fill: parent
        radius: height / 2
        color: Qt.rgba(1, 1, 1, !root.enabled ? 0.12 : input.activeFocus ? 0.26 : 0.20)

        // The shake moves a transform rather than x: the pill is anchor-filled,
        // and animating x under anchors fights the layout.
        transform: Translate { id: shakeOffset }
        border.width: 1
        border.color: Qt.rgba(1, 1, 1, !root.enabled ? 0.16 : input.activeFocus ? 0.50 : 0.32)

        Behavior on color {
            ColorAnimation { duration: 140 }
        }

        Behavior on border.color {
            ColorAnimation { duration: 140 }
        }

        Text {
            id: prompt

            anchors.left: parent.left
            anchors.leftMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            visible: input.text.length === 0
            color: Qt.rgba(1, 1, 1, 0.72)
            font.pixelSize: 13
            renderType: Text.NativeRendering
        }

        TextInput {
            id: input

            anchors.fill: parent
            anchors.leftMargin: 14
            anchors.rightMargin: 40
            // TextInput scrolls a long line but still paints past its bounds.
            clip: true
            verticalAlignment: TextInput.AlignVCenter
            horizontalAlignment: TextInput.AlignLeft

            focus: true
            enabled: root.enabled
            color: root.enabled ? "white" : Qt.rgba(1, 1, 1, 0.45)
            font.family: "monospace"
            font.pixelSize: 13
            echoMode: TextInput.Password
            passwordCharacter: "•"
            // No reveal delay: the last character is not echoed even briefly.
            passwordMaskDelay: 0

            // Every one of these is a copy of the credential that would outlive
            // the attempt.
            persistentSelection: false
            selectByMouse: false
            mouseSelectionMode: TextInput.SelectCharacters
            activeFocusOnPress: true

            onTextEdited: root.edited()

            // The text stays, grayed out while the field is disabled; only a
            // rejection clears it (ash's LoginAuthUserView::OnAuthComplete).
            onAccepted: {
                if (text.length === 0)
                    return
                root.accepted(text)
            }

            Keys.onPressed: function (event) {
                const clipboardKeys = [Qt.Key_Copy, Qt.Key_Cut, Qt.Key_Paste]
                if (clipboardKeys.indexOf(event.key) !== -1) {
                    event.accepted = true
                    return
                }
                if ((event.modifiers & (Qt.ControlModifier | Qt.ShiftModifier))
                        && [Qt.Key_V, Qt.Key_C, Qt.Key_X, Qt.Key_Insert].indexOf(event.key) !== -1) {
                    event.accepted = true
                    return
                }
                if (event.key === Qt.Key_Z && (event.modifiers & Qt.ControlModifier)) {
                    event.accepted = true
                }
            }

            // The primary-selection paste. Refused for the same reason.
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.IBeamCursor
                hoverEnabled: true
                acceptedButtons: Qt.MiddleButton
                onClicked: {}
            }
        }

        // The submit button: a round arrow.
        RoundedRectangle {
            id: go

            anchors.right: parent.right
            anchors.rightMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            width: 24
            height: 24
            radius: width / 2
            color: goArea.containsMouse && goArea.enabled ? Qt.rgba(1, 1, 1, 0.32)
                                                          : Qt.rgba(1, 1, 1, 0.22)
            opacity: input.text.length > 0 && root.enabled ? 1 : 0.35
            visible: opacity > 0

            Behavior on opacity {
                NumberAnimation { duration: 140 }
            }

            Behavior on color {
                ColorAnimation { duration: 120 }
            }

            Canvas {
                id: glyph

                anchors.fill: parent
                renderStrategy: Canvas.Immediate

                onPaint: {
                    const ctx = getContext("2d")
                    ctx.reset()
                    ctx.clearRect(0, 0, width, height)
                    ctx.strokeStyle = "white"
                    ctx.lineCap = "round"
                    ctx.lineJoin = "round"

                    const cx = width / 2
                    const cy = height / 2

                    ctx.lineWidth = 1.5
                    ctx.beginPath()
                    ctx.moveTo(cx - 4, cy)
                    ctx.lineTo(cx + 4, cy)
                    ctx.moveTo(cx + 1, cy - 3)
                    ctx.lineTo(cx + 4, cy)
                    ctx.lineTo(cx + 1, cy + 3)
                    ctx.stroke()
                }
            }

            MouseArea {
                id: goArea

                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                enabled: root.enabled && input.text.length > 0
                onClicked: input.accepted()
            }
        }
    }

    // Ash shakes the login pod on a rejection; this shakes the field, which is
    // where the user is looking.
    SequentialAnimation {
        id: shakeAnimation

        loops: 2

        NumberAnimation {
            target: shakeOffset; property: "x"; to: -8; duration: 50; easing.type: Easing.OutQuad
        }
        NumberAnimation {
            target: shakeOffset; property: "x"; to: 8; duration: 100; easing.type: Easing.InOutQuad
        }
        NumberAnimation {
            target: shakeOffset; property: "x"; to: 0; duration: 50; easing.type: Easing.InQuad
        }
    }
}
