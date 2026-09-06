import QtQuick 1.0

// Replaces the composer while messages are selected.
//
// Actions appear here as they are implemented. A control that is visible but
// does nothing is worse than one that is absent: it reads as a defect, and on
// a touch screen it is tried repeatedly before that is clear.
Rectangle {
    id: actionBarRoot

    // Reported rather than acted on directly: the page owns the selection
    // state, and reaching out of a component to set it relies on the id being
    // visible from whatever context happens to create this.
    signal closed()

    height: 40 * kgScaling
    color: "#FFFFFF"

    // Swallows anything that falls through the buttons, so a stray tap on the
    // bar does not reach the list underneath and change the selection.
    MouseArea {
        anchors.fill: parent
    }

    Item {
        id: closeButton
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        width: height

        Image {
            anchors.centerIn: parent
            source: "../../img/close.png"
            width: 20 * kgScaling
            height: width
            smooth: true
            asynchronous: true
        }

        MouseArea {
            anchors.fill: parent
            onClicked: actionBarRoot.closed()
        }
    }

    Text {
        anchors.left: closeButton.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: 5 * kgScaling
        // selectionChanged is what refreshes this: a QML binding cannot see
        // into a C++ container on its own.
        text: actionBarRoot.selectionCount == 1
                ? "1 message selected"
                : actionBarRoot.selectionCount + " messages selected"
        font.pixelSize: 12 * kgScaling
        color: "#666666"
    }

    property int selectionCount: 0

    function refresh() {
        selectionCount = messagesModel.selectionCount();
    }

    Connections {
        target: messagesModel
        onSelectionChanged: actionBarRoot.refresh()
    }

    Rectangle {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 1 * kgScaling
        color: "#DDDDDD"
    }
}
