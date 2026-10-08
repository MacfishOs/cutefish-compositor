import QtQuick
import QtQuick.Layouts
import Cutefish.Auth

Item {
    id: root

    property bool primary: true

    // Set by the compositor while the session is blocked; the clock only
    // ticks then, so a prepared, unseen panel never wakes it.
    property bool blocked: false

    property date now: new Date()

    readonly property bool failed: authController.failed

    function focusPassword() { passwordField.forceActiveFocus() }

    function tick() {
        now = new Date()
        // Just past the next minute, so the time never trails the real one.
        clockTimer.interval = 60000 - now.getSeconds() * 1000 - now.getMilliseconds() + 20
    }

    focus: true

    onBlockedChanged: tick()

    Timer {
        id: clockTimer

        running: root.blocked
        repeat: true
        onTriggered: root.tick()
    }

    Column {
        id: clock

        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 56
        spacing: -4

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: root.now.toLocaleDateString(Qt.locale(), "ddd MMM d")
            color: "white"
            font.pixelSize: 20
            font.weight: Font.DemiBold
            style: Text.Raised
            styleColor: Qt.rgba(0, 0, 0, 0.3)
            renderType: Text.NativeRendering
        }

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: root.now.toLocaleTimeString(Qt.locale(), "hh:mm")
            color: "white"
            font.pixelSize: 84
            font.weight: Font.DemiBold
            style: Text.Raised
            styleColor: Qt.rgba(0, 0, 0, 0.3)
            renderType: Text.NativeRendering
        }
    }

    ColumnLayout {
        id: pod

        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 64
        width: Math.min(parent.width - 48, 240)
        spacing: 18

        visible: root.primary
        opacity: root.primary ? 1 : 0

        Behavior on opacity {
            NumberAnimation { duration: 200; easing.type: Easing.OutCubic }
        }

        UserView {
            Layout.alignment: Qt.AlignHCenter
            displayName: authController.displayName
            iconSource: authController.iconSource
        }

        // Another account, when there is more than one to log in as.
        ChoiceMenu {
            Layout.alignment: Qt.AlignHCenter
            z: 1
            visible: authController.canPickUser
            names: authController.userNames
            currentIndex: authController.userIndex
            enabled: !authController.busy && !authController.succeeded

            onActivated: function (index) {
                authController.userIndex = index
                if (root.failed)
                    authController.reset()
                passwordField.clear()
                passwordField.forceActiveFocus()
            }
        }

        // Only where no account could be listed: the name is typed.
        RoundedRectangle {
            Layout.fillWidth: true
            implicitHeight: 40
            radius: 20
            color: Qt.rgba(1, 1, 1, 0.16)
            visible: authController.typesUserName
            TextInput {
                anchors.fill: parent
                anchors.margins: 10
                clip: true
                color: "white"
                text: authController.userName
                enabled: !authController.busy && !authController.succeeded
                onTextEdited: authController.userName = text
                onAccepted: passwordField.forceActiveFocus()
                Text {
                    anchors.centerIn: parent
                    visible: parent.text.length === 0
                    text: qsTr("User name")
                    color: "#aaffffff"
                }
            }
        }

        PasswordField {
            id: passwordField

            Layout.alignment: Qt.AlignHCenter
            Layout.fillWidth: true

            placeholder: authController.prompt !== ""
                         ? authController.prompt
                         : qsTr("Enter Password")
            enabled: !authController.busy && !authController.succeeded

            onAccepted: function (password) {
                authController.authenticate(password)
            }

            // Any edit means the user has moved on from the rejection; the
            // error goes away without waiting for another attempt.
            onEdited: {
                if (root.failed)
                    authController.reset()
            }
        }

        AuthError {
            Layout.alignment: Qt.AlignHCenter
            Layout.fillWidth: true
            message: authController.errorMessage
            shown: root.failed
        }

        ChoiceMenu {
            Layout.alignment: Qt.AlignHCenter
            // The open list goes over what follows the panel, not under it.
            z: 1
            visible: authController.canSelectSession
            names: authController.sessionNames
            currentIndex: authController.sessionIndex
            enabled: !authController.busy && !authController.succeeded

            onActivated: function (index) {
                authController.sessionIndex = index
                passwordField.forceActiveFocus()
            }
        }
    }

    Connections {
        target: authController

        function onAuthenticationFailed() {
            passwordField.clear()
            passwordField.shake()
            passwordField.forceActiveFocus()
        }
    }

    // The keyboard is the compositor's to give; when it arrives it belongs in
    // the password field and nowhere else.
    onPrimaryChanged: {
        if (primary)
            passwordField.forceActiveFocus()
    }

    Component.onCompleted: {
        if (primary)
            passwordField.forceActiveFocus()
    }
}
