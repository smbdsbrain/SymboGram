import QtQuick 1.0
import SymboGram 1.0
import "../control"

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
            if (atYEnd) {
                messagesModel.markRead();
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
        onReplyRequested: {
            messageEdit.startReplying(messagesModel.selectedMessageId());
            globalState = "NO_SELECT";
        }

        onEditRequested: {
            // The source text, not the rendered copy: the rendering applies
            // entities and rewrites spoilers, and cannot be turned back.
            messageEdit.startEditing(messagesModel.selectedMessageId(),
                                     messagesModel.selectedMessageSource());
            globalState = "NO_SELECT";
        }

        onDeleteRequested: {
            var count = messagesModel.selectionCount();
            var subject = count == 1 ? "this message" : count + " messages";

            // "Delete for everyone" is offered only while it applies to the
            // whole selection: the server refuses it for anything else, and a
            // choice that fails is worse than one that is absent.
            deleteConfirm.open("Delete " + subject + "?",
                               "Delete for me",
                               messagesModel.selectionIsRevocable()
                                    ? "Delete for everyone" : "");
        }
    }

    ConfirmBar {
        id: deleteConfirm
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom

        onConfirmed: {
            messagesModel.deleteSelected(secondary);
            globalState = "NO_SELECT";
        }
    }
}
