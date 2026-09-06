import QtQuick 1.0

// A confirmation for something that cannot be undone.
//
// Qt Quick 1 has no Dialog, and an unconfirmed destructive action on a touch
// screen is not acceptable -- a delete is one stray tap away from a message
// nobody meant to lose. Shaped like the SnackBar so it reads as the same kind
// of thing rising from the bottom of the screen.
Rectangle {
    id: confirmRoot

    // Which button was pressed, rather than what to do about it: the caller
    // owns the action.
    signal confirmed(bool secondary)
    signal dismissed()

    property string text: ""
    property string primaryText: ""
    // Left empty when the second choice does not apply -- revoking someone
    // else's message, or one past the window where the server allows it.
    property string secondaryText: ""

    function open(message, primary, secondary) {
        text = message;
        primaryText = primary;
        secondaryText = secondary;
        state = "OPEN";
    }

    function close() {
        state = "CLOSED";
    }

    height: contentColumn.height + 20 * kgScaling
    color: "#333333"
    state: "CLOSED"

    states: [
        State {
            name: "CLOSED"
            PropertyChanges { target: confirmRoot; opacity: 0; visible: false }
        },
        State {
            name: "OPEN"
            PropertyChanges { target: confirmRoot; opacity: 1; visible: true }
        }
    ]

    transitions: [
        Transition {
            NumberAnimation { properties: "opacity"; duration: 150 }
        }
    ]

    // Nothing behind this is reachable while it is up.
    MouseArea {
        anchors.fill: parent
    }

    Column {
        id: contentColumn
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: 10 * kgScaling
        anchors.rightMargin: 10 * kgScaling
        spacing: 8 * kgScaling

        Text {
            anchors.left: parent.left
            anchors.right: parent.right
            text: confirmRoot.text
            color: "#FFFFFF"
            wrapMode: Text.Wrap
            font.pixelSize: 12 * kgScaling
        }

        Row {
            anchors.right: parent.right
            spacing: 15 * kgScaling

            Text {
                text: "Cancel"
                color: "#BBBBBB"
                font.bold: true
                font.pixelSize: 12 * kgScaling

                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -8 * kgScaling
                    onClicked: {
                        confirmRoot.close();
                        confirmRoot.dismissed();
                    }
                }
            }

            Text {
                text: confirmRoot.secondaryText
                visible: confirmRoot.secondaryText.length != 0
                color: globalAccent
                font.bold: true
                font.pixelSize: 12 * kgScaling

                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -8 * kgScaling
                    onClicked: {
                        confirmRoot.close();
                        confirmRoot.confirmed(true);
                    }
                }
            }

            Text {
                text: confirmRoot.primaryText
                color: globalAccent
                font.bold: true
                font.pixelSize: 12 * kgScaling

                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -8 * kgScaling
                    onClicked: {
                        confirmRoot.close();
                        confirmRoot.confirmed(false);
                    }
                }
            }
        }
    }
}
