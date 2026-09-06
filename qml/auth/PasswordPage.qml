import QtQuick 1.0
import "../control"

Rectangle {
    id: passwordPage

    property string hint: ""
    // -1 while idle. The key derivation is seconds on the device, and an
    // indeterminate spinner over a wait that long reads as a hang.
    property int progress: -1

    function reset() {
        passwordEdit.text = "";
        progress = -1;
    }

    Column {
        anchors.centerIn: parent
        width: parent.width * 2 / 3
        spacing: 5

        Text {
            anchors.left: parent.left
            anchors.right: parent.right
            text: "Two-Step Verification"
            font.bold: true
            wrapMode: Text.Wrap
            font.pixelSize: 12 * kgScaling
        }

        Text {
            anchors.left: parent.left
            anchors.right: parent.right
            text: "This account is protected by a cloud password. Please enter it."
            wrapMode: Text.Wrap
            font.pixelSize: 12 * kgScaling
        }

        Text {
            anchors.left: parent.left
            anchors.right: parent.right
            text: "Hint: " + passwordPage.hint
            visible: passwordPage.hint.length != 0
            wrapMode: Text.Wrap
            font.pixelSize: 12 * kgScaling
            color: "#666666"
        }

        LineEdit {
            id: passwordEdit
            anchors.left: parent.left
            anchors.right: parent.right
            echoMode: TextInput.Password
        }

        Text {
            anchors.left: parent.left
            anchors.right: parent.right
            text: "Checking your password. This can take a few seconds."
            visible: passwordPage.progress >= 0
            wrapMode: Text.Wrap
            font.pixelSize: 12 * kgScaling
            color: "#666666"
        }

        Item {
            anchors.left: parent.left
            anchors.right: parent.right
            height: 2 * kgScaling
            visible: passwordPage.progress >= 0

            Rectangle {
                anchors.fill: parent
                color: "#999999"
            }

            Rectangle {
                anchors.left: parent.left
                height: parent.height
                color: globalAccent
                width: parent.width * Math.max(0, passwordPage.progress) / 100

                Behavior on width {
                    NumberAnimation {
                        easing.type: Easing.InOutQuad
                    }
                }
            }
        }

        Button {
            anchors.left: parent.left
            anchors.right: parent.right
            text: "Log in"
            enabled: !root.authProgress
            onClicked: {
                if (passwordEdit.text.length == 0) {
                    snackBar.text = "Please enter your password.";
                    return;
                }

                snackBar.close();

                setAuthProgress(true);
                passwordPage.progress = 0;
                telegramClient.authCheckPasswordSRP(passwordEdit.text);

                // The client holds it for as long as the proof needs it, and
                // for a retry if the parameters go stale. Nothing here does.
                passwordEdit.text = "";
            }
        }
    }
}
