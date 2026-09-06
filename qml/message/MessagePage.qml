import QtQuick 1.0
import SymboGram 1.0

Rectangle {
    property string globalState: "NO_SELECT"

    onGlobalStateChanged: {
        if (globalState != "SHOW_SELECT") {
            messagesModel.clearSelection();
        }
    }
    property alias messageEdit: messageEdit
    property alias messagesView: messagesView

    ListView {
        id: messagesView
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: messageEdit.top
        clip: true

        boundsBehavior: Flickable.StopAtBounds

        anchors.topMargin: Math.max(0, parent.height - messageEdit.height - childrenRect.height)

        cacheBuffer: Math.max(parent.height / 6, 0)

        onMovementEnded: {
            if (atYBeginning && messagesModel.canFetchMoreUpwards()) {
                messagesModel.fetchMoreUpwards();
            }
            if (atYEnd && messagesModel.canFetchMoreDownwards()) {
                messagesModel.fetchMoreDownwards();
            }
        }

        model: messagesModel

        delegate: MessageItem {
            state: globalState
        }
    }

    //TODO Hide MessageEdit when user is restricted
    MessageEdit {
        id: messageEdit
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        visible: globalState != "SHOW_SELECT"
    }

    // Same geometry as the composer it stands in for, so the list above does
    // not move when the selection opens and closes.
    MessageActionBar {
        id: messageActionBar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        visible: globalState == "SHOW_SELECT"
        onClosed: globalState = "NO_SELECT"
    }
}
